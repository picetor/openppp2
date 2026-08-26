//! Core launcher: spawns the C++ headless core as a child process and
//! discovers its RPC endpoint from the `RPC_LISTEN=` line on stdout.
//!
//! Lifecycle: `spawn()` starts the child; `stop()` terminates it.  The TUI
//! owns the core when launched this way (single-program UX): closing the UI
//! closes the VPN.

use std::fs;
use std::io::{BufRead, BufReader, Write};
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::mpsc::{channel, Receiver};
use std::thread;
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use anyhow::{bail, Context, Result};

use super::command::command_value;
use super::embedded;
use super::settings::resolve_from_working_dir;
use crate::rpc::{CoreCommand, Response, RpcClient};

/// Wait for the core to print its RPC endpoint after spawn.
const LAUNCH_TIMEOUT: Duration = Duration::from_secs(20);

/// Windows Job Object handle with KILL_ON_JOB_CLOSE: closing the handle
/// (on drop) force-terminates every process assigned to the job.  This makes
/// the spawned core a well-behaved child: whatever happens to the TUI
/// (normal exit, crash, task-manager kill), the core cannot outlive it and
/// become an orphan "second core" in the task manager.
#[cfg(windows)]
struct JobHandle {
    handle: *mut core::ffi::c_void,
}
#[cfg(windows)]
unsafe impl Send for JobHandle {}
#[cfg(windows)]
impl Drop for JobHandle {
    fn drop(&mut self) {
        unsafe {
            win32::CloseHandle(self.handle);
        }
    }
}
#[cfg(not(windows))]
struct JobHandle;

#[cfg(windows)]
mod win32 {
    use super::JobHandle;
    use std::os::windows::io::AsRawHandle;
    use std::process::Child;

    const JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE: u32 = 0x0000_2000;
    const JOB_OBJECT_EXTENDED_LIMIT_INFORMATION: u32 = 9;

    #[repr(C)]
    struct JOBOBJECT_EXTENDED_LIMIT_INFORMATION {
        per_process_user_time_limit: i64,
        per_job_user_time_limit: i64,
        limit_flags: u32,
        min_working_set_size: usize,
        max_working_set_size: usize,
        active_process_limit: u32,
        affinity: usize,
        priority_class: u32,
        scheduling_class: u32,
        io_info: [u64; 6],
        process_memory_limit: usize,
        job_memory_limit: usize,
        peak_process_memory_used: usize,
        peak_job_memory_used: usize,
    }

    extern "system" {
        fn CreateJobObjectW(
            lp_job_attributes: *const u8,
            lp_name: *const u16,
        ) -> *mut core::ffi::c_void;
        fn SetInformationJobObject(
            h_job: *mut core::ffi::c_void,
            job_object_information_class: u32,
            lp_job_object_information: *const u8,
            cb_job_object_information_length: u32,
        ) -> i32;
        fn AssignProcessToJobObject(
            h_job: *mut core::ffi::c_void,
            h_process: *mut core::ffi::c_void,
        ) -> i32;
        pub(super) fn CloseHandle(h_object: *mut core::ffi::c_void) -> i32;
    }

    /// Create a kill-on-close job and assign the child to it.  Returns the
    /// job handle on success (it must be kept alive for the child's life).
    pub(super) fn attach_kill_on_close(child: &mut Child) -> Option<JobHandle> {
        unsafe {
            let job = CreateJobObjectW(core::ptr::null(), core::ptr::null());
            if job.is_null() {
                return None;
            }
            let info = JOBOBJECT_EXTENDED_LIMIT_INFORMATION {
                per_process_user_time_limit: 0,
                per_job_user_time_limit: 0,
                limit_flags: JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE,
                min_working_set_size: 0,
                max_working_set_size: 0,
                active_process_limit: 0,
                affinity: 0,
                priority_class: 0,
                scheduling_class: 0,
                io_info: [0; 6],
                process_memory_limit: 0,
                job_memory_limit: 0,
                peak_process_memory_used: 0,
                peak_job_memory_used: 0,
            };
            let set_ok = SetInformationJobObject(
                job,
                JOB_OBJECT_EXTENDED_LIMIT_INFORMATION,
                &info as *const _ as *const u8,
                core::mem::size_of::<JOBOBJECT_EXTENDED_LIMIT_INFORMATION>() as u32,
            ) != 0;
            if !set_ok {
                CloseHandle(job);
                return None;
            }
            let assigned =
                AssignProcessToJobObject(job, child.as_raw_handle() as *mut core::ffi::c_void) != 0;
            if !assigned {
                CloseHandle(job);
                return None;
            }
            Some(JobHandle { handle: job })
        }
    }
}

pub struct Launcher {
    child: Child,
    pub endpoint: String,
    pub token: String,
    cleanup_dir: Option<PathBuf>,
    shutdown_attempted: bool,
    shutdown_acknowledged: bool,
    /// Kill-on-close job handle; keeps the core from outliving the TUI.
    /// Held only for its Drop side effect (closing the handle kills the job).
    #[allow(dead_code)]
    job: Option<JobHandle>,
}

fn display_path(path: &Path) -> String {
    path.to_string_lossy().replace('\\', "/")
}

fn random_token() -> String {
    let nanos = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_nanos())
        .unwrap_or(0);
    let pid = std::process::id() as u128;
    let mixed = nanos ^ (pid << 32) ^ (nanos.rotate_left(17) ^ pid.rotate_right(11));
    format!("ppp-{mixed:032x}")
}

/// Deliver a graceful shutdown over a fresh RPC session and wait for the
/// core to acknowledge that it accepted the request. A live UI session is
/// intentionally not reused: it may be unauthenticated, disconnected, or
/// holding an in-flight snapshot when the user closes the UI.
pub fn request_graceful_shutdown(endpoint: &str, token: &str) -> bool {
    let mut rpc = RpcClient::new(endpoint.to_string(), token.to_string());
    let connect_deadline = std::time::Instant::now() + Duration::from_secs(8);
    loop {
        if rpc.connect().is_ok() {
            break;
        }
        if std::time::Instant::now() >= connect_deadline {
            return false;
        }
        thread::sleep(Duration::from_millis(200));
        rpc = RpcClient::new(endpoint.to_string(), token.to_string());
    }

    let hello_deadline = std::time::Instant::now() + Duration::from_millis(1500);
    while !rpc.is_authenticated() && std::time::Instant::now() < hello_deadline {
        match rpc.poll() {
            Ok(_) => {}
            Err(_) => return false,
        }
        thread::sleep(Duration::from_millis(10));
    }
    if !rpc.is_authenticated() {
        return false;
    }

    if rpc
        .request_command(CoreCommand::Shutdown { restart: false })
        .is_err()
    {
        return false;
    }

    let shutdown_deadline = std::time::Instant::now() + Duration::from_secs(5);
    while std::time::Instant::now() < shutdown_deadline {
        match rpc.poll() {
            Ok(Some(Response::Result { method, value, .. })) if method == "shutdown" => {
                return value
                    .get("accepted")
                    .and_then(|accepted| accepted.as_bool())
                    .unwrap_or(false);
            }
            Ok(Some(_)) | Ok(None) => {}
            Err(_) => return false,
        }
        thread::sleep(Duration::from_millis(5));
    }
    false
}

/// Spawn a thread that forwards the core's stdout lines; returns a channel
/// that receives each line as it arrives.  Every line is also appended to
/// `tee_path` (when given) so a crashed core's last diagnostics survive even
/// though the channel is only consumed while waiting for `RPC_LISTEN=`.
fn pipe_stdout(child: &mut Child, tee_path: Option<PathBuf>) -> Option<Receiver<String>> {
    let stdout = child.stdout.take()?;
    let (tx, rx) = channel();
    thread::spawn(move || {
        let mut tee = tee_path.and_then(|path| {
            fs::OpenOptions::new()
                .create(true)
                .append(true)
                .open(path)
                .ok()
        });
        let reader = BufReader::new(stdout);
        for line in reader.lines() {
            match line {
                Ok(line) => {
                    if let Some(file) = tee.as_mut() {
                        let _ = writeln!(file, "{line}");
                    }
                    if tx.send(line).is_err() {
                        break;
                    }
                }
                Err(_) => break,
            }
        }
        if let Some(file) = tee.as_mut() {
            let _ = file.flush();
        }
    });
    Some(rx)
}

impl Launcher {
    /// Spawn the headless core with the given extra arguments (mode, config,
    /// tun-*, bypass-*, ... are forwarded verbatim).
    pub fn spawn(core_path: &Path, extra_args: &[String]) -> Result<Launcher> {
        let working_dir = core_path
            .parent()
            .filter(|path| !path.as_os_str().is_empty())
            .unwrap_or_else(|| Path::new("."));
        Self::spawn_in(core_path, extra_args, working_dir)
    }

    /// Spawn an external core while resolving relative configuration paths
    /// against an explicitly selected working directory.
    pub fn spawn_in(
        core_path: &Path,
        extra_args: &[String],
        working_dir: &Path,
    ) -> Result<Launcher> {
        if !core_path.exists() {
            bail!("core not found at {}", display_path(core_path));
        }
        Self::spawn_process(core_path, extra_args, working_dir, None)
    }

    /// Materialize the embedded core in a private temporary directory and
    /// launch it without requiring a separate ppp.exe beside the TUI.
    ///
    /// The process still communicates over the existing loopback RPC channel;
    /// this keeps the C++ core and its C++ TUI implementation untouched.
    pub fn spawn_embedded(extra_args: &[String]) -> Result<Launcher> {
        let working_dir = std::env::current_dir().context("get TUI working directory")?;
        Self::spawn_embedded_in(extra_args, &working_dir)
    }

    /// Same as `spawn_embedded`, but resolves relative configuration paths
    /// against an explicit directory selected by the desktop client.
    pub fn spawn_embedded_in(extra_args: &[String], working_dir: &Path) -> Result<Launcher> {
        let staging_dir = embedded_staging_dir()?;
        let core_path = staging_dir.join(if cfg!(windows) {
            "ppp-tui-core.exe"
        } else {
            "ppp-tui-core"
        });

        let write_result = (|| -> Result<()> {
            let mut file = fs::OpenOptions::new()
                .write(true)
                .create_new(true)
                .open(&core_path)
                .with_context(|| format!("create embedded core {}", display_path(&core_path)))?;
            file.write_all(embedded::CORE_EXE)
                .with_context(|| format!("write embedded core {}", display_path(&core_path)))?;
            file.flush()
                .with_context(|| format!("flush embedded core {}", display_path(&core_path)))?;
            #[cfg(unix)]
            {
                use std::os::unix::fs::PermissionsExt;
                fs::set_permissions(&core_path, fs::Permissions::from_mode(0o700))
                    .context("make embedded core executable")?;
            }
            Ok(())
        })();
        if let Err(error) = write_result {
            let _ = fs::remove_dir_all(&staging_dir);
            return Err(error);
        }

        // Relative --config/--dns-rules/--geo-rules arguments should resolve
        // against the user's launch directory, not the private staging dir.
        match Self::spawn_process(
            &core_path,
            extra_args,
            working_dir,
            Some(staging_dir.clone()),
        ) {
            Ok(launcher) => Ok(launcher),
            Err(error) => {
                let _ = fs::remove_dir_all(staging_dir);
                Err(error)
            }
        }
    }

    fn spawn_process(
        core_path: &Path,
        extra_args: &[String],
        working_dir: &Path,
        cleanup_dir: Option<PathBuf>,
    ) -> Result<Launcher> {
        let token = random_token();

        let mut command = Command::new(core_path);
        command.current_dir(working_dir);
        command
            .arg("--headless")
            .arg("--rpc-listen=127.0.0.1:0")
            .arg(format!("--rpc-token={token}"))
            // Allow more than one RPC client: the live TUI session holds one
            // slot, and the graceful-shutdown fallback
            // must be able to open a SECOND connection to deliver the
            // shutdown frame.  With the core's default max_clients=1 the
            // fallback connection is rejected (socket closed immediately),
            // the core never receives shutdown, and the 30s launcher timeout
            // force-kills it leaving Windows DNS/routes unrestored.
            .arg("--rpc-max-clients=4")
            .args(extra_args);
        // Keep the core's log separate from the TUI log: give the core its
        // own file unless the user already forwarded --log-file.
        let core_log_file = if let Some(value) = command_value(extra_args, "--log-file") {
            resolve_from_working_dir(working_dir, &value)
        } else {
            command.arg("--log-file=./ppp-core.log");
            working_dir.join("ppp-core.log")
        };
        // TUI 拥有终端：嵌入式核心绝不能把原始诊断写到同一屏幕（与 ratatui
        // 光标移动交错会产生重复/乱码行）。核心诊断已由 --log-file 捕获
        // （默认 ./ppp-core.log），stdout 保持管道化用于 RPC_LISTEN，并且
        // 每行都会追加到核心日志文件 —— 崩溃后最后输出也能保留。
        command.stdout(Stdio::piped()).stderr(Stdio::null());

        // The embedded core is a Windows console-subsystem executable, but
        // the desktop client owns the only visible window.  Without this
        // flag Windows creates a second console window for the child even
        // though it is headless and its stdout is piped into the TUI.
        #[cfg(windows)]
        {
            use std::os::windows::process::CommandExt;
            const CREATE_NO_WINDOW: u32 = 0x0800_0000;
            command.creation_flags(CREATE_NO_WINDOW);
        }

        let mut child = command
            .spawn()
            .with_context(|| format!("failed to start core {}", display_path(core_path)))?;

        // The core must never outlive the TUI: kill-on-close job object.
        #[cfg(windows)]
        let job = win32::attach_kill_on_close(&mut child);
        #[cfg(not(windows))]
        let job: Option<JobHandle> = None;

        // Append core stdout to the core's own log file so all core output
        // (diagnostics + retained stdout) lands in ./ppp-core.log.
        let tee_path = Some(core_log_file);
        let rx = pipe_stdout(&mut child, tee_path).context("cannot pipe core stdout")?;

        // Wait for the RPC_LISTEN= line (the core prints it once its RPC
        // server is up; log lines start with '[' and are ignored).  If the
        // child exits early (bad args, instance lock, ...) report its last
        // stdout lines instead of waiting out the timeout.
        let deadline = std::time::Instant::now() + LAUNCH_TIMEOUT;
        let mut tail: Vec<String> = Vec::new();
        let mut endpoint = None;
        loop {
            if let Ok(Some(status)) = child.try_wait() {
                let last_lines = tail.join(" | ");
                bail!(
                    "core exited early (status {status}) before reporting its RPC endpoint. \
                     Last output: {last_lines}"
                );
            }
            let remaining = deadline.saturating_duration_since(std::time::Instant::now());
            if remaining.is_zero() {
                break;
            }
            match rx.recv_timeout(remaining) {
                Ok(line) => {
                    // Windows stdout lines end with \r\n; lines() strips \n
                    // only, so trim the trailing \r (and any whitespace).
                    let line = line.trim();
                    if line.contains("Non-administrators are not allowed to run.") {
                        let _ = child.kill();
                        let _ = child.wait();
                        bail!("VPN/TUN 核心需要管理员权限，请右键以管理员身份启动 ppp-tui.exe。");
                    }
                    if line.contains("Repeat runs are not allowed.") {
                        let _ = child.kill();
                        let _ = child.wait();
                        bail!(
                            "另一个 ppp 核心实例已在运行（Repeat runs are not allowed）。\
                             请先退出其他 ppp-tui / ppp-tui-cli / ppp 窗口，或使用任务管理器结束残留的 ppp-core.exe 进程。"
                        );
                    }
                    if line.contains("Failed to open the vpn client.")
                        || line.contains("Open tun/tap driver failure.")
                        || line.contains("No available nic could be found.")
                    {
                        let _ = child.kill();
                        let _ = child.wait();
                        bail!("VPN/TUN core startup failed: {line}");
                    }
                    if let Some(rest) = line.strip_prefix("RPC_LISTEN=") {
                        endpoint = Some(rest.trim().to_string());
                        break;
                    }
                    tail.push(line.to_string());
                    if tail.len() > 8 {
                        tail.remove(0);
                    }
                }
                Err(std::sync::mpsc::RecvTimeoutError::Timeout) => break,
                Err(std::sync::mpsc::RecvTimeoutError::Disconnected) => {
                    // Child closed stdout without printing RPC_LISTEN.
                    if let Ok(Some(status)) = child.try_wait() {
                        bail!(
                            "core exited (status {status}) without reporting its RPC endpoint. \
                             Last output: {}",
                            tail.join(" | ")
                        );
                    }
                    break;
                }
            }
        }

        let endpoint = match endpoint {
            Some(endpoint) => endpoint,
            None => {
                let _ = child.kill();
                let _ = child.wait();
                bail!(
                    "core did not report its RPC endpoint within {}s. \
                     Last output: {}",
                    LAUNCH_TIMEOUT.as_secs(),
                    tail.join(" | ")
                );
            }
        };

        Ok(Launcher {
            child,
            endpoint,
            token,
            cleanup_dir,
            job,
            shutdown_attempted: false,
            shutdown_acknowledged: false,
        })
    }

    /// Whether the core process has exited on its own (crash, self-restart,
    /// external kill).  Returns the exit code when it did.
    pub fn has_exited(&mut self) -> Option<i32> {
        match self.child.try_wait() {
            Ok(Some(status)) => status.code(),
            _ => None,
        }
    }

    /// PID of the spawned core process (used by the console-close watchdog
    /// to wait for the core to finish restoring DNS/routes before exit).
    pub fn pid(&self) -> u32 {
        self.child.id()
    }

    /// Ask the owned core to perform its synchronous network cleanup. The
    /// attempt is latched so `stop()`/`Drop` never send a second request after
    /// a caller has already tried to shut the core down.
    pub fn request_graceful_shutdown(&mut self) -> bool {
        if self.shutdown_attempted {
            return self.shutdown_acknowledged;
        }
        self.shutdown_attempted = true;
        self.shutdown_acknowledged = request_graceful_shutdown(&self.endpoint, &self.token);
        self.shutdown_acknowledged
    }

    /// Terminate the core.  Prefer an RPC `shutdown` first (caller), then
    /// wait for the core's synchronous network cleanup.  Force-kill is only
    /// the fallback for a hung or already disconnected core; killing
    /// immediately after queuing shutdown can leave Windows DNS/routes in
    /// the temporary VPN state.
    pub fn stop(&mut self) {
        let running = !matches!(self.child.try_wait(), Ok(Some(_)));
        if running && !self.shutdown_attempted {
            // Keep Launcher safe even when a caller forgets to perform the
            // explicit graceful-shutdown step.
            let _ = self.request_graceful_shutdown();
        }
        // The deadline must cover the core's full restoration: Dispose()
        // restores routes + physical-NIC DNS and then waits up to 10s for
        // its DNS guard workers to stop, so a short kill timeout aborts the
        // DNS restore halfway and leaves the host without working DNS. Keep
        // this longer than the core's 10s DNS-guard deadline plus final exit
        // scheduling, because killing is the last resort only.
        let deadline = std::time::Instant::now() + Duration::from_secs(30);
        loop {
            match self.child.try_wait() {
                Ok(Some(_)) => break,
                Ok(None) if std::time::Instant::now() < deadline => {
                    thread::sleep(Duration::from_millis(50));
                }
                Ok(None) | Err(_) => {
                    let _ = self.child.kill();
                    break;
                }
            }
        }
        let _ = self.child.wait();
        self.cleanup_staging_dir();
    }

    fn cleanup_staging_dir(&mut self) {
        if let Some(path) = self.cleanup_dir.take() {
            let _ = fs::remove_dir_all(path);
        }
    }
}

impl Drop for Launcher {
    fn drop(&mut self) {
        self.stop();
    }
}

fn embedded_staging_dir() -> Result<PathBuf> {
    let mut path = std::env::temp_dir();
    path.push(format!("openppp2-ppp-tui-{}", random_token()));
    fs::create_dir(&path).with_context(|| {
        format!(
            "create embedded core staging directory {}",
            display_path(&path)
        )
    })?;
    Ok(path)
}
