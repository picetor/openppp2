//! Exercises the real embedded runtime against a local endpoint that rejects
//! handshakes. Proxy mode uses no TUN, system proxy, or listening proxy ports.

use ppp_tui::core::in_process::CoreHandle;
use serde_json::{json, Value};
use std::fs;
use std::net::TcpListener;
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

struct Fixture {
    directory: PathBuf,
    stop: Arc<AtomicBool>,
    worker: Option<JoinHandle<()>>,
}

impl Fixture {
    fn new() -> Self {
        let directory = std::env::temp_dir().join(format!(
            "ppp-restart-test-{}-{}",
            std::process::id(),
            SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        fs::create_dir_all(&directory).unwrap();
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let port = listener.local_addr().unwrap().port();
        listener.set_nonblocking(true).unwrap();
        let stop = Arc::new(AtomicBool::new(false));
        let worker_stop = stop.clone();
        let worker = thread::spawn(move || {
            while !worker_stop.load(Ordering::Acquire) {
                if let Ok((stream, _)) = listener.accept() {
                    drop(stream); // TCP connects, PPP handshake fails.
                } else {
                    thread::sleep(Duration::from_millis(10));
                }
            }
        });

        let mut config: Value =
            serde_json::from_str(include_str!("../../appsettings.json")).unwrap();
        config["concurrent"] = json!(1);
        config["ip"] = json!({"interface": "0.0.0.0", "public": "0.0.0.0"});
        config["vmem"] = json!({"size": 0, "path": ""});
        config["client"]["server"] = json!(format!("ppp://127.0.0.1:{port}/"));
        config["client"]["server-proxy"] = json!("");
        config["client"]["mappings"] = json!([]);
        config["client"]["log"] = json!("");
        config["client"]["paper-airplane"]["tcp"] = json!(false);
        config["client"]["http-proxy"] = json!({"bind": "127.0.0.1", "port": 0});
        config["client"]["socks-proxy"] = json!({"bind": "127.0.0.1", "port": 0});
        config["udp"]["static"]["servers"] = json!([]);
        config["udp"]["static"]["aggligator"] = json!(0);
        config["udp"]["static"]["icmp"] = json!(false);
        config["tcp"]["connect"]["timeout"] = json!(1);
        fs::write(directory.join("client.json"), config.to_string()).unwrap();
        Self {
            directory,
            stop,
            worker: Some(worker),
        }
    }

    fn start(&self, limit: u32, auto_restart: u32) -> CoreHandle {
        CoreHandle::start(&[
            "--mode=proxy".into(),
            "--headless".into(),
            "--rt=no".into(),
            "--proxy-http-port=0".into(),
            "--proxy-socks-port=0".into(),
            "--set-http-proxy=no".into(),
            "--bypass-mode=no".into(),
            "--log-level=debug".into(),
            format!("--config={}", self.directory.join("client.json").display()),
            format!("--log-file={}", self.directory.join("core.log").display()),
            format!("--link-restart={limit}"),
            format!("--auto-restart={auto_restart}"),
        ])
        .expect("embedded proxy core must start")
    }
}

impl Drop for Fixture {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::Release);
        if let Some(worker) = self.worker.take() {
            worker.join().unwrap();
        }
        if thread::panicking() {
            eprintln!(
                "core restart fixture retained at {}",
                self.directory.display()
            );
        } else {
            let _ = fs::remove_dir_all(&self.directory);
        }
    }
}

fn wait_for_exit(core: &CoreHandle) {
    let deadline = Instant::now() + Duration::from_secs(15);
    while core.is_running() && Instant::now() < deadline {
        thread::sleep(Duration::from_millis(25));
    }
    assert!(
        !core.is_running(),
        "core did not finish cleanup within deadline"
    );
}

#[test]
fn embedded_restart_reason_is_published_after_cleanup_and_reset_between_runs() {
    let fixture = Fixture::new();
    {
        let core = fixture.start(3, 0);
        assert!(!core.restart_requested());
        wait_for_exit(&core);
        assert!(
            core.restart_requested(),
            "three failed handshakes must request restart"
        );
    }
    {
        // Also proves the previous runtime released its lock/resources and
        // did not leak its restart flag into this handle.
        let core = fixture.start(0, 0);
        thread::sleep(Duration::from_secs(3));
        assert!(core.is_running(), "limit zero must keep reconnecting");
        assert!(!core.restart_requested());
        core.stop().unwrap();
        assert!(!core.is_running());
        assert!(
            !core.restart_requested(),
            "manual stop is not a planned restart"
        );
    }
    {
        let core = fixture.start(0, 1);
        wait_for_exit(&core);
        assert!(
            core.restart_requested(),
            "timer restart must carry the same planned reason"
        );
    }
}
