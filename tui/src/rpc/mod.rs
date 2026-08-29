//! Local JSON-RPC-over-TCP client for the openppp2 headless core.
//!
//! Frame format (docs/RUST_TUI_DESIGN_CN.md §4): 4-byte big-endian length
//! followed by a JSON UTF-8 body.  The client is intentionally
//! asynchronous-free: the TUI event loop drives it with non-blocking I/O.

pub mod schema;

use std::collections::VecDeque;
use std::io::{Read, Write};
use std::net::{TcpStream, ToSocketAddrs};
#[cfg(ppp_in_process_core)]
use std::sync::{Arc, Mutex};
#[cfg(ppp_in_process_core)]
use std::thread;
use std::time::Duration;

use anyhow::{bail, Context, Result};
use serde_json::{json, Value};

#[cfg(ppp_in_process_core)]
use crate::core::in_process::CoreHandle;

const MAX_FRAME_SIZE: usize = 4 * 1024 * 1024;
const CONNECT_TIMEOUT: Duration = Duration::from_secs(3);

/// Semantic commands exposed by the headless core control plane.
///
/// The wire method names remain compatible with the existing C++ RPC server,
/// but UI code must use this typed enum instead of duplicating string method
/// names and parameter objects in the GUI and terminal front-ends.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum CoreCommand {
    GetSnapshot,
    GetLogs { since_seq: u64 },
    Switch { tag: String, ranked_first: bool },
    SetLogLevel { level: String },
    Shutdown { restart: bool },
}

impl CoreCommand {
    pub fn method(&self) -> &'static str {
        match self {
            Self::GetSnapshot => "get_snapshot",
            Self::GetLogs { .. } => "get_logs",
            Self::Switch {
                ranked_first: true, ..
            } => "switch_rank1",
            Self::Switch {
                ranked_first: false,
                ..
            } => "switch_server",
            Self::SetLogLevel { .. } => "set_log_level",
            Self::Shutdown { .. } => "shutdown",
        }
    }

    pub fn params(&self) -> Value {
        match self {
            Self::GetSnapshot => json!({}),
            Self::GetLogs { since_seq } => json!({ "since_seq": since_seq }),
            Self::Switch { tag, .. } => json!({ "tag": tag }),
            Self::SetLogLevel { level } => json!({ "level": level }),
            Self::Shutdown { restart } => json!({
                "restart": restart,
                "confirm": "shutdown",
            }),
        }
    }
}

#[derive(Debug, Clone)]
pub enum Response {
    /// Successful reply carrying the method result.
    Result {
        id: u64,
        method: String,
        value: Value,
    },
    /// Error reply carrying the JSON-RPC-ish error object.
    Error {
        id: u64,
        method: String,
        code: i64,
        message: String,
    },
    /// Server-push notification without an id (e.g. `event: "log"`).
    Event { kind: String, params: Value },
}

/// Non-blocking RPC client.  All state lives on the caller's event loop.
pub struct RpcClient {
    address: String,
    token: String,
    stream: Option<TcpStream>,
    /// Frames not yet written to the socket.
    write_queue: VecDeque<Vec<u8>>,
    /// Bytes read from the socket but not yet framed.
    read_buf: Vec<u8>,
    next_id: u64,
    /// (id, method) for every request sent and not yet answered, in order.
    /// The server answers requests in order over TCP, so responses map to
    /// this queue front-to-back.
    pending: VecDeque<(u64, String)>,
    /// Parsed frames waiting for the caller (batch polling).
    frame_queue: VecDeque<Response>,
    connected: bool,
    authenticated: bool,
    pub last_error: Option<String>,
}

impl RpcClient {
    pub fn new(address: String, token: String) -> Self {
        Self {
            address,
            token,
            stream: None,
            write_queue: VecDeque::new(),
            read_buf: Vec::new(),
            next_id: 1,
            pending: VecDeque::new(),
            frame_queue: VecDeque::new(),
            connected: false,
            authenticated: false,
            last_error: None,
        }
    }

    pub fn is_connected(&self) -> bool {
        self.connected
    }

    pub fn is_authenticated(&self) -> bool {
        self.authenticated
    }

    /// Address used for the loopback RPC connection.  The desktop client uses
    /// this to perform the potentially blocking TCP connect off the UI thread.
    pub fn address(&self) -> &str {
        &self.address
    }

    /// True while a request is in flight (no response received yet).
    pub fn has_pending(&self) -> bool {
        !self.pending.is_empty()
    }

    /// Number of unanswered requests (diagnostics).
    pub fn pending_count(&self) -> usize {
        self.pending.len()
    }

    /// True while frames are still queued for writing (not yet flushed onto
    /// the socket).  Used by shutdown paths to detect that a request frame
    /// (e.g. `shutdown`) was never actually delivered because the connection
    /// broke before `poll()` could flush it.
    pub fn has_unsent_frames(&self) -> bool {
        !self.write_queue.is_empty()
    }

    /// Bytes buffered from the socket but not yet framed (diagnostics).
    pub fn buffered_bytes(&self) -> usize {
        self.read_buf.len()
    }

    /// Establish the TCP connection and queue the `hello` handshake.
    pub fn connect(&mut self) -> Result<()> {
        if self.connected {
            return Ok(());
        }

        let stream = Self::connect_socket(&self.address)?;
        self.attach_stream(stream)
    }

    /// Perform only the potentially blocking part of connecting to RPC.
    /// Callers that own a UI/event loop should run this function in a worker
    /// thread, then pass the stream to `attach_stream`.
    pub fn connect_socket(address: &str) -> Result<TcpStream> {
        let mut addrs = address
            .trim()
            .to_socket_addrs()
            .with_context(|| format!("cannot resolve rpc address '{address}'"))?;
        let addr = addrs
            .next()
            .with_context(|| format!("no address for '{address}'"))?;

        TcpStream::connect_timeout(&addr, CONNECT_TIMEOUT)
            .with_context(|| format!("connect {address} failed"))
    }

    /// Attach an already-connected socket and queue the authentication hello.
    pub fn attach_stream(&mut self, stream: TcpStream) -> Result<()> {
        stream
            .set_nonblocking(true)
            .context("set nonblocking failed")?;
        stream.set_nodelay(true).context("set nodelay failed")?;

        self.stream = Some(stream);
        self.connected = true;
        self.authenticated = false;
        self.read_buf.clear();
        self.write_queue.clear();
        self.pending.clear();
        self.frame_queue.clear();

        let id = self.next_id;
        self.next_id += 1;
        let frame = encode_frame(&json!({
            "id": id,
            "method": "hello",
            "params": {
                "token": self.token,
                "client_version": env!("CARGO_PKG_VERSION"),
                "schema_version": schema::SCHEMA_VERSION,
            }
        }));
        self.write_queue.push_back(frame);
        self.pending.push_back((id, "hello".to_string()));
        self.last_error = None;
        Ok(())
    }

    /// Drop the connection (used for retry after an error).
    pub fn disconnect(&mut self) {
        self.stream = None;
        self.connected = false;
        self.authenticated = false;
        self.read_buf.clear();
        self.write_queue.clear();
        self.pending.clear();
        self.frame_queue.clear();
    }

    /// Queue a request.  Responses are matched in order (TCP preserves the
    /// request order and the server answers one at a time).
    pub fn request(&mut self, method: &str, params: Value) -> Result<()> {
        if !self.connected {
            bail!("not connected");
        }
        let id = self.next_id;
        self.next_id += 1;
        let frame = encode_frame(&json!({
            "id": id,
            "method": method,
            "params": params,
        }));
        self.write_queue.push_back(frame);
        self.pending.push_back((id, method.to_string()));
        Ok(())
    }

    /// Queue a typed core control command.
    pub fn request_command(&mut self, command: CoreCommand) -> Result<()> {
        self.request(command.method(), command.params())
    }

    /// Drive I/O: flush queued frames, read all available bytes, parse every
    /// complete frame into a queue, then return the next parsed frame.
    /// Returns `Ok(None)` when no more frames are ready.
    pub fn poll(&mut self) -> Result<Option<Response>> {
        if !self.connected {
            return Ok(None);
        }

        // Flush pending writes.
        while let Some(frame) = self.write_queue.front() {
            match self.stream.as_mut().unwrap().write(frame) {
                Ok(0) => {
                    self.disconnect();
                    return Ok(None);
                }
                Ok(n) if n < frame.len() => {
                    // Partial write: keep the remainder for the next poll.
                    let remaining = frame[n..].to_vec();
                    self.write_queue.pop_front();
                    self.write_queue.push_front(remaining);
                    break;
                }
                Ok(_) => {
                    self.write_queue.pop_front();
                }
                Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => break,
                Err(e) => {
                    self.disconnect();
                    self.last_error = Some(format!("write failed: {e}"));
                    return Ok(None);
                }
            }
        }

        // Read everything currently available.
        let mut scratch = [0u8; 8192];
        loop {
            match self.stream.as_mut().unwrap().read(&mut scratch) {
                Ok(0) => {
                    self.disconnect();
                    self.last_error = Some("connection closed by peer".into());
                    return Ok(None);
                }
                Ok(n) => self.read_buf.extend_from_slice(&scratch[..n]),
                Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => break,
                Err(e) => {
                    self.disconnect();
                    self.last_error = Some(format!("read failed: {e}"));
                    return Ok(None);
                }
            }
        }

        // Parse every complete frame now (batch), so a burst of notifications
        // cannot stall the snapshot pipeline.
        while let Some(response) = self.try_take_frame()? {
            self.frame_queue.push_back(response);
        }
        Ok(self.frame_queue.pop_front())
    }

    fn try_take_frame(&mut self) -> Result<Option<Response>> {
        if self.read_buf.len() < 4 {
            return Ok(None);
        }
        let len = u32::from_be_bytes([
            self.read_buf[0],
            self.read_buf[1],
            self.read_buf[2],
            self.read_buf[3],
        ]) as usize;
        if len == 0 || len > MAX_FRAME_SIZE {
            bail!("invalid frame length {len}");
        }
        if self.read_buf.len() < 4 + len {
            return Ok(None);
        }

        let body = self.read_buf[4..4 + len].to_vec();
        self.read_buf.drain(..4 + len);

        let value: Value = serde_json::from_slice(&body).context("invalid JSON frame")?;
        if value.get("id").is_none() {
            // Server push notification (no request id).
            let kind = value
                .get("event")
                .and_then(|v| v.as_str())
                .unwrap_or("unknown")
                .to_string();
            let params = value.get("params").cloned().unwrap_or(Value::Null);
            return Ok(Some(Response::Event { kind, params }));
        }
        // The server answers in request order; pop the front entry and use
        // its method for the response (fall back to the empty string when
        // the id does not match, e.g. after a reconnect race).
        let (_, method) = self.pending.pop_front().unwrap_or((0, String::new()));
        let response = parse_response(value, method);
        // A successful hello authenticates the session; until then the UI
        // must not issue get_snapshot / commands.
        if let Response::Result { method: m, .. } = &response {
            if m == "hello" {
                self.authenticated = true;
                self.last_error = None;
            }
        }
        Ok(Some(response))
    }
}

/// UI-facing core client.  A normal attach still uses the legacy loopback
/// RPC client, while a core owned by the Rust binary uses the C++ runtime
/// directly.  Keeping this small adapter lets both front-ends migrate without
/// duplicating snapshot and command handling.
pub enum CoreClient {
    Rpc(RpcClient),
    #[cfg(ppp_in_process_core)]
    InProcess(InProcessClient),
}

#[cfg(ppp_in_process_core)]
pub struct InProcessClient {
    core: Arc<Mutex<CoreHandle>>,
    responses: Arc<Mutex<VecDeque<Response>>>,
    pending: Arc<Mutex<VecDeque<(u64, String)>>>,
    next_id: u64,
}

#[cfg(ppp_in_process_core)]
impl InProcessClient {
    fn start(args: &[String]) -> Result<Self> {
        Ok(Self {
            core: Arc::new(Mutex::new(CoreHandle::start(args)?)),
            responses: Arc::new(Mutex::new(VecDeque::new())),
            pending: Arc::new(Mutex::new(VecDeque::new())),
            next_id: 1,
        })
    }

    fn is_running(&self) -> bool {
        self.core
            .lock()
            .map(|core| core.is_running())
            .unwrap_or(false)
    }

    fn request_command(&mut self, command: CoreCommand) -> Result<()> {
        if !self.is_running() {
            bail!("core is not running");
        }

        let id = self.next_id;
        self.next_id = self.next_id.saturating_add(1);
        let method = command.method().to_string();
        let params = serde_json::to_string(&command.params()).context("serialize core command")?;
        self.pending
            .lock()
            .map_err(|_| anyhow::anyhow!("core pending queue poisoned"))?
            .push_back((id, method.clone()));

        let core = Arc::clone(&self.core);
        let responses = Arc::clone(&self.responses);
        let pending = Arc::clone(&self.pending);
        thread::spawn(move || {
            let result = core
                .lock()
                .map_err(|_| "core handle lock poisoned".to_string())
                .and_then(|core| {
                    core.command(&method, &params)
                        .map_err(|error| format!("{error:#}"))
                });

            if let Ok(mut queue) = pending.lock() {
                if let Some(position) = queue.iter().position(|(pending_id, _)| *pending_id == id) {
                    queue.remove(position);
                }
            }
            if let Ok(mut queue) = responses.lock() {
                match result {
                    Ok(value) => queue.push_back(Response::Result { id, method, value }),
                    Err(message) => queue.push_back(Response::Error {
                        id,
                        method,
                        code: 500,
                        message,
                    }),
                }
            }
        });
        Ok(())
    }

    fn poll(&mut self) -> Result<Option<Response>> {
        Ok(self
            .responses
            .lock()
            .map_err(|_| anyhow::anyhow!("core response queue poisoned"))?
            .pop_front())
    }

    fn has_pending(&self) -> bool {
        self.pending
            .lock()
            .map(|queue| !queue.is_empty())
            .unwrap_or(true)
    }

    fn stop(&self) -> Result<()> {
        self.core
            .lock()
            .map_err(|_| anyhow::anyhow!("core handle lock poisoned"))?
            .stop()
    }

    fn register_emergency_stop(&self) {
        crate::core::in_process::register_emergency_core(Arc::clone(&self.core));
    }

    fn clear_emergency_stop(&self) {
        crate::core::in_process::clear_emergency_core();
    }
}

#[cfg(ppp_in_process_core)]
impl Drop for InProcessClient {
    fn drop(&mut self) {
        // Do not let the process-wide console callback keep an old core alive
        // after the UI has intentionally discarded this client.
        self.clear_emergency_stop();
    }
}

impl CoreClient {
    pub fn rpc(address: String, token: String) -> Self {
        Self::Rpc(RpcClient::new(address, token))
    }

    #[cfg(ppp_in_process_core)]
    pub fn in_process(args: &[String]) -> Result<Self> {
        Ok(Self::InProcess(InProcessClient::start(args)?))
    }

    pub fn is_connected(&self) -> bool {
        match self {
            Self::Rpc(client) => client.is_connected(),
            #[cfg(ppp_in_process_core)]
            Self::InProcess(client) => client.is_running(),
        }
    }

    pub fn is_authenticated(&self) -> bool {
        match self {
            Self::Rpc(client) => client.is_authenticated(),
            #[cfg(ppp_in_process_core)]
            Self::InProcess(client) => client.is_running(),
        }
    }

    pub fn is_running(&self) -> bool {
        match self {
            Self::Rpc(client) => client.is_connected(),
            #[cfg(ppp_in_process_core)]
            Self::InProcess(client) => client.is_running(),
        }
    }

    pub fn address(&self) -> &str {
        match self {
            Self::Rpc(client) => client.address(),
            #[cfg(ppp_in_process_core)]
            Self::InProcess(_) => "in-process",
        }
    }

    pub fn has_pending(&self) -> bool {
        match self {
            Self::Rpc(client) => client.has_pending(),
            #[cfg(ppp_in_process_core)]
            Self::InProcess(client) => client.has_pending(),
        }
    }

    pub fn attach_stream(&mut self, stream: TcpStream) -> Result<()> {
        match self {
            Self::Rpc(client) => client.attach_stream(stream),
            #[cfg(ppp_in_process_core)]
            Self::InProcess(_) => bail!("cannot attach a stream to an in-process core"),
        }
    }

    pub fn request_command(&mut self, command: CoreCommand) -> Result<()> {
        match self {
            Self::Rpc(client) => client.request_command(command),
            #[cfg(ppp_in_process_core)]
            Self::InProcess(client) => client.request_command(command),
        }
    }

    pub fn poll(&mut self) -> Result<Option<Response>> {
        match self {
            Self::Rpc(client) => client.poll(),
            #[cfg(ppp_in_process_core)]
            Self::InProcess(client) => client.poll(),
        }
    }

    pub fn disconnect(&mut self) {
        match self {
            Self::Rpc(client) => client.disconnect(),
            #[cfg(ppp_in_process_core)]
            Self::InProcess(_) => {}
        }
    }

    #[cfg(ppp_in_process_core)]
    pub fn stop_owned(&self) -> Result<()> {
        match self {
            Self::InProcess(client) => {
                let result = client.stop();
                if result.is_ok() {
                    client.clear_emergency_stop();
                }
                result
            }
            Self::Rpc(_) => Ok(()),
        }
    }

    #[cfg(ppp_in_process_core)]
    pub fn register_emergency_stop(&self) {
        if let Self::InProcess(client) = self {
            client.register_emergency_stop();
        }
    }

    pub fn is_in_process(&self) -> bool {
        match self {
            Self::Rpc(_) => false,
            #[cfg(ppp_in_process_core)]
            Self::InProcess(_) => true,
        }
    }
}

fn encode_frame(value: &Value) -> Vec<u8> {
    let body = serde_json::to_vec(value).expect("serialize request");
    let mut frame = Vec::with_capacity(4 + body.len());
    frame.extend_from_slice(&(body.len() as u32).to_be_bytes());
    frame.extend_from_slice(&body);
    frame
}

fn parse_response(value: Value, method: String) -> Response {
    let id = value.get("id").and_then(|v| v.as_u64()).unwrap_or(0);
    if value.get("ok").and_then(|v| v.as_bool()).unwrap_or(false) {
        Response::Result {
            id,
            method,
            value: value.get("result").cloned().unwrap_or(Value::Null),
        }
    } else {
        let error = value.get("error").unwrap_or(&Value::Null);
        Response::Error {
            id,
            method,
            code: error.get("code").and_then(|v| v.as_i64()).unwrap_or(-1),
            message: error
                .get("message")
                .and_then(|v| v.as_str())
                .unwrap_or("unknown error")
                .to_string(),
        }
    }
}
