//! Lightweight TCP latency probing for the server catalog.
//!
//! The desktop client probes `ppp://host:port` endpoints directly so server
//! latency is visible on startup, before any core (or VPN link) is started.
//! The C++ core has its own deeper probe (WS/TLS handshake) that takes over
//! once a real core is running; these direct TCP probes only fill the gap
//! while the control-plane core has no outbound rows yet.

use std::collections::HashMap;
use std::net::{TcpStream, ToSocketAddrs};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

/// Time between probe rounds.
const PROBE_INTERVAL: Duration = Duration::from_secs(5);
/// Per-endpoint TCP connect timeout.
const CONNECT_TIMEOUT: Duration = Duration::from_secs(2);

/// Per-server probe result.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ProbeState {
    /// No result yet (first round still in flight).
    Pending,
    /// A probe round is currently in flight.
    Probing,
    /// Endpoint reached; RTT in milliseconds.
    Ok(i32),
    /// Endpoint could not be reached within the timeout.
    Unreachable,
}

/// Snapshot of the probe results for all catalog servers, keyed by profile
/// name.
#[derive(Default)]
pub struct ProbeTable {
    states: HashMap<String, ProbeState>,
}

impl ProbeTable {
    pub fn state(&self, name: &str) -> Option<ProbeState> {
        self.states.get(name).copied()
    }
}

/// Start the background probe loop.  `targets` maps profile name -> endpoint
/// URI (the first entry of each profile is used).  The loop probes every
/// target once per `PROBE_INTERVAL` and exits as soon as the app drops its
/// `Arc` reference (checked via the strong count each round).
pub fn spawn_probe_loop(targets: Arc<Vec<(String, String)>>, table: Arc<Mutex<ProbeTable>>) {
    std::thread::spawn(move || loop {
        // Exit when the app no longer holds its reference.
        if Arc::strong_count(&table) <= 1 {
            return;
        }
        let round_start = Instant::now();
        {
            let mut guard = table
                .lock()
                .unwrap_or_else(|poisoned| poisoned.into_inner());
            for (name, _) in targets.iter() {
                guard.states.insert(name.clone(), ProbeState::Probing);
            }
        }
        let mut handles = Vec::with_capacity(targets.len());
        for (name, uri) in targets.iter() {
            let name = name.clone();
            let uri = uri.clone();
            let table = Arc::clone(&table);
            handles.push(std::thread::spawn(move || {
                let result = match probe_tcp_connect(&uri) {
                    Some(rtt_ms) => ProbeState::Ok(rtt_ms),
                    None => ProbeState::Unreachable,
                };
                if let Ok(mut guard) = table.lock() {
                    guard.states.insert(name, result);
                }
            }));
        }
        for handle in handles {
            let _ = handle.join();
        }
        let elapsed = round_start.elapsed();
        if elapsed < PROBE_INTERVAL {
            std::thread::sleep(PROBE_INTERVAL - elapsed);
        }
    });
}

/// TCP connect latency in milliseconds, or `None` when the endpoint is
/// unreachable or cannot be resolved.
fn probe_tcp_connect(uri: &str) -> Option<i32> {
    let (host, port) = endpoint_host_port(uri)?;
    let start = Instant::now();
    let addrs = (host.as_str(), port).to_socket_addrs().ok()?;
    for addr in addrs {
        if TcpStream::connect_timeout(&addr, CONNECT_TIMEOUT).is_ok() {
            let rtt = start.elapsed().as_millis();
            return Some(rtt.min(i32::MAX as u128) as i32);
        }
    }
    None
}

/// Extract `host:port` from a `ppp://host:port[/path]` (or bare `host:port`)
/// endpoint URI, handling both numeric IPv4/IPv6 (`[addr]:port`) literals and
/// hostnames.
fn endpoint_host_port(uri: &str) -> Option<(String, u16)> {
    let trimmed = uri.trim();
    let authority_start = trimmed.find("://").map(|index| index + 3).unwrap_or(0);
    let authority = &trimmed[authority_start..];
    let authority = authority
        .find('/')
        .map(|index| &authority[..index])
        .unwrap_or(authority);
    if let Some(rest) = authority.strip_prefix('[') {
        let (host, port) = rest.split_once("]:")?;
        return Some((host.to_string(), port.parse().ok()?));
    }
    let (host, port) = authority.rsplit_once(':')?;
    Some((host.to_string(), port.parse().ok()?))
}
