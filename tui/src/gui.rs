//! Native desktop client UI.
//!
//! This replaces the terminal renderer with an ordinary egui window. The
//! The front-end owns the C++ core through the in-process C ABI when the
//! platform build provides it. External loopback RPC remains available for
//! attaching to an already-running headless core.

use std::fs;
use std::io::Write;
use std::net::TcpStream;
use std::path::{Component, Path, PathBuf};
use std::sync::mpsc::{channel, Receiver};
use std::sync::{Arc, Mutex, OnceLock};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use eframe::egui::{
    self, Align2, Color32, FontFamily, FontId, Pos2, Rect, RichText, Rounding, Sense, Stroke,
    TextEdit, TextStyle, TextWrapMode, Vec2,
};
use eframe::{App, CreationContext, Frame};
use serde::{Deserialize, Serialize};
use serde_json::Value;

use ppp_tui::core::launcher::Launcher;
use ppp_tui::core::probe::{spawn_probe_loop, ProbeState, ProbeTable};
use ppp_tui::core::settings::{normalize_log_level, normalize_tcp_ip_cc};
use ppp_tui::core::traffic::{format_bytes, format_rate, TrafficHistory};
use ppp_tui::rpc::schema::{Network, NetworkInterface, Outbound, Snapshot};
use ppp_tui::rpc::{CoreClient, CoreCommand, Response, RpcClient};

const ACCENT: Color32 = Color32::from_rgb(86, 166, 255);
const GOOD: Color32 = Color32::from_rgb(74, 196, 124);
const WARN: Color32 = Color32::from_rgb(242, 184, 76);
const BAD: Color32 = Color32::from_rgb(235, 97, 97);
const MUTED: Color32 = Color32::from_rgb(155, 165, 180);

// All TUI diagnostics, including startup and panic messages, must use the
// same user-configured path as runtime TUI logs.  `None` is the explicit
// opt-out controlled by `tui_log_enabled`.
static TUI_LOG_PATH: OnceLock<Mutex<Option<PathBuf>>> = OnceLock::new();

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum View {
    Overview,
    Network,
    Servers,
    Routes,
    Settings,
}

enum CoreLaunch {
    Process(Launcher),
    #[cfg(ppp_in_process_core)]
    InProcess(CoreClient),
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum KeyboardScroll {
    LineUp,
    LineDown,
    PageUp,
    PageDown,
    Home,
    End,
}

#[derive(Debug, Clone)]
struct LocalServerProfile {
    name: String,
    path: PathBuf,
    server: String,
    entries: Vec<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(default)]
pub struct StartupSettings {
    pub settings_file: String,
    pub working_dir: String,
    pub command: String,
    pub mode: String,
    pub config_path: String,
    pub server_dir: String,
    pub tun_ip: String,
    pub tun_gw: String,
    pub tun_mask: String,
    pub tun_mux: String,
    pub tun_mux_acceleration: String,
    pub link_restart: String,
    pub tun_host: bool,
    pub tun_vnet: bool,
    pub tun_static: bool,
    pub tun_flash: bool,
    pub block_quic: bool,
    pub proxy_http_port: String,
    pub proxy_socks_port: String,
    pub bypass_mode: String,
    pub bypass_file: String,
    pub bypass6_file: String,
    pub dns_rules_file: String,
    pub geo_rules_file: String,
    pub geosite_file: String,
    pub geoip_file: String,
    pub log_file: String,
    pub log_level: String,
    /// `auto` preserves the C++ core's platform/driver default; explicit
    /// values are translated to --lwip=yes/no when a TUN client is started.
    pub tcp_ip_cc: String,
    pub rt: bool,
    pub dns: String,
    pub auto_restart: String,
    pub firewall_rules: String,
    pub nic: String,
    pub ngw: String,
    pub tun: String,
    pub tun_driver: String,
    pub tun_ssmt: String,
    pub tun_lease_time: String,
    pub bypass_nic: String,
    pub bypass_ngw: String,
    pub bypass_nic6: String,
    pub bypass_ngw6: String,
    pub tun_promisc: bool,
    pub tun_route: bool,
    pub tun_protect: bool,
    #[serde(default = "default_tui_log_enabled")]
    pub tui_log_enabled: bool,
    pub tui_log_file: String,
    pub rpc_address: String,
    pub rpc_token: String,
    pub core_path: Option<String>,
    pub tun_enabled: bool,
    pub system_proxy_enabled: bool,
    #[serde(skip)]
    pub launch_direct: bool,
}

impl Default for StartupSettings {
    fn default() -> Self {
        Self {
            settings_file: "./ppp-tui.json".to_string(),
            working_dir: path_to_forward_slashes(
                &std::env::current_dir().unwrap_or_else(|_| PathBuf::from(".")),
            ),
            command: String::new(),
            mode: "client".to_string(),
            config_path: "./config/HKBN.json".to_string(),
            server_dir: "./config".to_string(),
            tun_ip: String::new(),
            tun_gw: String::new(),
            tun_mask: String::new(),
            tun_mux: String::new(),
            tun_mux_acceleration: String::new(),
            link_restart: String::new(),
            tun_host: true,
            tun_vnet: true,
            tun_static: false,
            tun_flash: false,
            block_quic: false,
            proxy_http_port: String::new(),
            proxy_socks_port: String::new(),
            bypass_mode: "ip".to_string(),
            bypass_file: "./ip.txt".to_string(),
            bypass6_file: "./ipv6.txt".to_string(),
            dns_rules_file: "./dns-rules.txt".to_string(),
            geo_rules_file: "./geo-rules.yaml".to_string(),
            geosite_file: "./geosite.dat".to_string(),
            geoip_file: "./geoip.dat".to_string(),
            log_file: "./ppp-core.log".to_string(),
            log_level: "error".to_string(),
            tcp_ip_cc: "auto".to_string(),
            rt: true,
            dns: String::new(),
            auto_restart: String::new(),
            firewall_rules: String::new(),
            nic: String::new(),
            ngw: String::new(),
            tun: String::new(),
            tun_driver: "auto".to_string(),
            tun_ssmt: String::new(),
            tun_lease_time: String::new(),
            bypass_nic: String::new(),
            bypass_ngw: String::new(),
            bypass_nic6: String::new(),
            bypass_ngw6: String::new(),
            tun_promisc: true,
            tun_route: false,
            tun_protect: true,
            tui_log_enabled: true,
            tui_log_file: "./ppp-tui.log".to_string(),
            rpc_address: String::new(),
            rpc_token: String::new(),
            core_path: None,
            tun_enabled: true,
            system_proxy_enabled: false,
            launch_direct: false,
        }
    }
}

impl StartupSettings {
    pub fn from_cli() -> Self {
        let defaults = Self::default();
        let mut rpc_address = String::new();
        let mut rpc_token = String::new();
        let mut core_path = None;
        let mut core_args = Vec::new();
        let mut iter = std::env::args().skip(1);

        while let Some(arg) = iter.next() {
            match arg.as_str() {
                "--rpc" => {
                    if let Some(value) = iter.next() {
                        rpc_address = value;
                    }
                }
                "--token" => {
                    if let Some(value) = iter.next() {
                        rpc_token = value;
                    }
                }
                "--ppp" => {
                    core_path = iter.next();
                }
                "--embedded-core" => {}
                "--help" | "-h" => {}
                _ if arg.starts_with("--rpc=") => {
                    rpc_address = arg[6..].to_string();
                }
                _ if arg.starts_with("--token=") => {
                    rpc_token = arg[8..].to_string();
                }
                _ if arg.starts_with("--ppp=") => {
                    core_path = Some(arg[6..].to_string());
                }
                _ => core_args.push(arg),
            }
        }

        let core_args = normalize_core_args(core_args);
        let launch_direct = !core_args.is_empty();
        let command = join_command_args(&core_args);
        let config_path = ["--config", "-c", "--c", "-config"]
            .iter()
            .find_map(|name| command_value(&core_args, name))
            .unwrap_or_else(|| defaults.config_path.clone());
        let server_dir = command_value(&core_args, "--server-dir")
            .unwrap_or_else(|| defaults.server_dir.clone());
        let tun_ip = command_value(&core_args, "--tun-ip").unwrap_or_default();
        let tun_gw = command_value(&core_args, "--tun-gw").unwrap_or_default();
        let tun_mask = command_value(&core_args, "--tun-mask").unwrap_or_default();
        let tun_mux = command_value(&core_args, "--tun-mux").unwrap_or_default();
        let tun_mux_acceleration =
            command_value(&core_args, "--tun-mux-acceleration").unwrap_or_default();
        let link_restart = command_value(&core_args, "--link-restart").unwrap_or_default();
        let tun_host = command_bool(&core_args, "--tun-host", defaults.tun_host);
        let tun_vnet = command_bool(&core_args, "--tun-vnet", defaults.tun_vnet);
        let tun_static = command_bool(&core_args, "--tun-static", defaults.tun_static);
        let tun_flash = command_bool(&core_args, "--tun-flash", defaults.tun_flash);
        let block_quic = command_bool(&core_args, "--block-quic", defaults.block_quic);
        let proxy_http_port = command_value(&core_args, "--proxy-http-port").unwrap_or_default();
        let proxy_socks_port = command_value(&core_args, "--proxy-socks-port").unwrap_or_default();
        let bypass_mode = command_value(&core_args, "--bypass-mode")
            .unwrap_or_else(|| defaults.bypass_mode.clone());
        let bypass_file =
            command_value(&core_args, "--bypass").unwrap_or_else(|| defaults.bypass_file.clone());
        let bypass6_file =
            command_value(&core_args, "--bypass6").unwrap_or_else(|| defaults.bypass6_file.clone());
        let dns_rules_file = command_value(&core_args, "--dns-rules")
            .unwrap_or_else(|| defaults.dns_rules_file.clone());
        let geo_rules_file = command_value(&core_args, "--geo-rules")
            .unwrap_or_else(|| defaults.geo_rules_file.clone());
        let geosite_file =
            command_value(&core_args, "--geosite").unwrap_or_else(|| defaults.geosite_file.clone());
        let geoip_file =
            command_value(&core_args, "--geoip").unwrap_or_else(|| defaults.geoip_file.clone());
        let log_file =
            command_value(&core_args, "--log-file").unwrap_or_else(|| defaults.log_file.clone());
        let log_level = command_value(&core_args, "--log-level")
            .map(|value| normalize_log_level(&value))
            .unwrap_or_else(|| defaults.log_level.clone());
        let tcp_ip_cc = command_value(&core_args, "--lwip")
            .map(|value| normalize_tcp_ip_cc(&value))
            .unwrap_or_else(|| defaults.tcp_ip_cc.clone());
        let rt = command_bool(&core_args, "--rt", defaults.rt);
        let dns = command_value(&core_args, "--dns").unwrap_or_default();
        let auto_restart = command_value(&core_args, "--auto-restart").unwrap_or_default();
        let firewall_rules = command_value(&core_args, "--firewall-rules").unwrap_or_default();
        let nic = command_value(&core_args, "--nic").unwrap_or_default();
        let ngw = command_value(&core_args, "--ngw").unwrap_or_default();
        let tun = command_value(&core_args, "--tun").unwrap_or_default();
        let tun_driver = command_value(&core_args, "--tun-driver")
            .unwrap_or_else(|| defaults.tun_driver.clone());
        let tun_ssmt = command_value(&core_args, "--tun-ssmt").unwrap_or_default();
        let tun_lease_time =
            command_value(&core_args, "--tun-lease-time-in-seconds").unwrap_or_default();
        let bypass_nic = command_value(&core_args, "--bypass-nic").unwrap_or_default();
        let bypass_ngw = command_value(&core_args, "--bypass-ngw").unwrap_or_default();
        let bypass_nic6 = command_value(&core_args, "--bypass-nic6").unwrap_or_default();
        let bypass_ngw6 = command_value(&core_args, "--bypass-ngw6").unwrap_or_default();
        let tun_promisc = command_bool(&core_args, "--tun-promisc", defaults.tun_promisc);
        let tun_route = command_bool(&core_args, "--tun-route", defaults.tun_route);
        let tun_protect = command_bool(&core_args, "--tun-protect", defaults.tun_protect);
        let tui_log_enabled =
            command_bool(&core_args, "--tui-log-enabled", defaults.tui_log_enabled);
        let tui_log_file =
            command_value(&core_args, "--tui-log").unwrap_or_else(|| defaults.tui_log_file.clone());

        let requested_mode = ["--mode", "--m", "-mode", "-m"]
            .iter()
            .find_map(|name| command_value(&core_args, name))
            .unwrap_or_default()
            .to_ascii_lowercase();
        let tun_enabled = !matches!(
            requested_mode.as_str(),
            "proxy" | "proxy-only" | "proxy_only"
        );
        let system_proxy_enabled = command_bool(
            &core_args,
            "--set-http-proxy",
            defaults.system_proxy_enabled,
        );

        let mut settings = Self {
            settings_file: defaults.settings_file,
            working_dir: defaults.working_dir,
            command,
            mode: match requested_mode.as_str() {
                "proxy" | "proxy-only" | "proxy_only" => "proxy".to_string(),
                "server" => "server".to_string(),
                _ => "client".to_string(),
            },
            config_path,
            server_dir,
            tun_ip,
            tun_gw,
            tun_mask,
            tun_mux,
            tun_mux_acceleration,
            link_restart,
            tun_host,
            tun_vnet,
            tun_static,
            tun_flash,
            block_quic,
            proxy_http_port,
            proxy_socks_port,
            bypass_mode,
            bypass_file,
            bypass6_file,
            dns_rules_file,
            geo_rules_file,
            geosite_file,
            geoip_file,
            log_file,
            log_level,
            tcp_ip_cc,
            rt,
            dns,
            auto_restart,
            firewall_rules,
            nic,
            ngw,
            tun,
            tun_driver,
            tun_ssmt,
            tun_lease_time,
            bypass_nic,
            bypass_ngw,
            bypass_nic6,
            bypass_ngw6,
            tun_promisc,
            tun_route,
            tun_protect,
            tui_log_enabled,
            tui_log_file,
            rpc_address,
            rpc_token,
            core_path,
            tun_enabled,
            system_proxy_enabled,
            launch_direct,
        };

        if std::env::args_os().nth(1).is_none() {
            settings.load_saved();
        }
        settings.normalize_paths();
        settings
    }

    fn load_saved(&mut self) {
        let path = resolve_settings_path(&self.settings_file);
        let Ok(contents) = fs::read_to_string(path) else {
            return;
        };
        let Ok(mut saved) = serde_json::from_str::<StartupSettings>(&contents) else {
            return;
        };
        if !contents.contains("\"tui_log_enabled\"") {
            // Older configurations used an empty path as the opt-out. Keep
            // that intent only during migration; the boolean is authoritative
            // for all subsequently saved configurations.
            saved.tui_log_enabled = !saved.tui_log_file.trim().is_empty();
        }
        if !contents.contains("\"mode\"") {
            saved.mode = if saved.tun_enabled {
                "client".to_string()
            } else {
                "proxy".to_string()
            };
        }
        saved.normalize_paths();
        *self = saved;
    }

    fn save(&self) -> anyhow::Result<()> {
        let path = resolve_settings_path(&self.settings_file);
        if let Some(parent) = path.parent() {
            fs::create_dir_all(parent)?;
        }
        let mut normalized = self.clone();
        normalized.normalize_paths();
        let json = serde_json::to_string_pretty(&normalized)?;
        fs::write(path, json)?;
        Ok(())
    }

    fn normalize_paths(&mut self) {
        normalize_path_field(&mut self.settings_file);
        normalize_path_field(&mut self.working_dir);
        normalize_path_field(&mut self.config_path);
        normalize_path_field(&mut self.server_dir);
        normalize_path_field(&mut self.bypass_file);
        normalize_path_field(&mut self.bypass6_file);
        normalize_path_field(&mut self.dns_rules_file);
        normalize_path_field(&mut self.geo_rules_file);
        normalize_path_field(&mut self.geosite_file);
        normalize_path_field(&mut self.geoip_file);
        normalize_path_field(&mut self.log_file);
        normalize_path_field(&mut self.firewall_rules);
        self.log_level = normalize_log_level(&self.log_level);
        self.tcp_ip_cc = normalize_tcp_ip_cc(&self.tcp_ip_cc);
        normalize_path_field(&mut self.tui_log_file);
        if let Some(path) = self.core_path.as_mut() {
            normalize_path_field(path);
        }
    }
}

pub struct DesktopApp {
    view: View,
    settings: StartupSettings,
    local_servers: Vec<LocalServerProfile>,
    selected_local_server: Option<usize>,
    runtime_server_selection: usize,
    catalog_core: bool,
    rpc: Option<CoreClient>,
    launcher: Option<Launcher>,
    in_process_core: bool,
    launch_rx: Option<Receiver<Result<CoreLaunch, String>>>,
    rpc_connect_rx: Option<Receiver<Result<TcpStream, String>>>,
    rpc_connecting: bool,
    snapshot: Option<Snapshot>,
    traffic: TrafficHistory,
    last_refresh: Instant,
    last_connect_attempt: Instant,
    launching: bool,
    status: String,
    error: Option<String>,
    first_frame_rendered: bool,
    frame_counter: u64,
    startup_core_pending: bool,
    startup_direct: bool,
    catalog_snapshot_logged: bool,
    auto_restart_count: u32,
    probe_table: Arc<Mutex<ProbeTable>>,
}

impl DesktopApp {
    pub fn new(cc: &CreationContext<'_>, settings: StartupSettings) -> Self {
        boot_log("DesktopApp::new entered");
        cc.egui_ctx.set_visuals(egui::Visuals::dark());
        cc.egui_ctx.set_pixels_per_point(1.16);
        boot_log("installing CJK font");
        install_cjk_font(&cc.egui_ctx);
        boot_log("CJK font installed");
        install_tui_style(&cc.egui_ctx);
        let (local_servers, catalog_error) = load_server_catalog(&settings);
        boot_log(&format!(
            "catalog loaded: {} servers, error={:?}",
            local_servers.len(),
            catalog_error
        ));
        // Probe the server catalog directly so latency is visible before any
        // core (or VPN link) is started; the control-plane core has no
        // outbound rows to probe with.
        let probe_table = Arc::new(Mutex::new(ProbeTable::default()));
        let probe_targets: Arc<Vec<(String, String)>> = Arc::new(
            local_servers
                .iter()
                .map(|profile| (profile.name.clone(), profile.server.clone()))
                .collect(),
        );
        spawn_probe_loop(probe_targets, Arc::clone(&probe_table));
        boot_log(&format!(
            "tcp probe loop started for {} server(s)",
            local_servers.len()
        ));
        let selected_local_server = find_selected_local_server(&settings, &local_servers);
        let view = if settings.rpc_address.trim().is_empty() {
            View::Servers
        } else {
            View::Overview
        };
        let status = if local_servers.is_empty() {
            "未启动核心，尚未读取到服务器配置".to_string()
        } else {
            format!("已读取 {} 个服务器配置，等待选择", local_servers.len())
        };
        let startup_core_pending = settings.rpc_address.trim().is_empty();
        let startup_direct = settings.launch_direct;
        let app = Self {
            view,
            settings,
            local_servers,
            selected_local_server,
            runtime_server_selection: 0,
            catalog_core: false,
            rpc: None,
            launcher: None,
            in_process_core: false,
            launch_rx: None,
            rpc_connect_rx: None,
            rpc_connecting: false,
            snapshot: None,
            traffic: TrafficHistory::new(),
            last_refresh: Instant::now() - Duration::from_secs(2),
            last_connect_attempt: Instant::now() - Duration::from_secs(5),
            launching: false,
            status,
            error: catalog_error,
            first_frame_rendered: false,
            frame_counter: 0,
            startup_core_pending,
            startup_direct,
            catalog_snapshot_logged: false,
            auto_restart_count: 0,
            probe_table,
        };
        boot_log(&format!(
            "startup control plane pending={} direct_command={}",
            startup_core_pending, startup_direct
        ));
        boot_log("DesktopApp::new done");
        app
    }

    fn has_owned_core(&self) -> bool {
        self.launcher.is_some() || self.in_process_core
    }

    fn needs_admin(&self) -> bool {
        normalized_launch_mode(&self.settings.mode) == "client" && self.settings.tun_enabled
    }

    fn verbose_frame_log(&self) -> bool {
        self.frame_counter <= 10 || self.frame_counter % 60 == 0
    }

    fn relaunch_as_administrator(&mut self) {
        if is_process_elevated() {
            boot_log("UAC: already elevated; relaunch skipped");
            self.status = "当前已经是管理员运行".to_string();
            return;
        }
        boot_log("UAC: badge clicked; stopping owned core before relaunch");
        self.stop_core(true);
        boot_log("UAC: requesting ShellExecuteW(runas)");
        match relaunch_elevated() {
            Ok(()) => {
                boot_log("UAC: elevated child requested; exiting parent");
                std::process::exit(0)
            }
            Err(error) => {
                boot_log(&format!("UAC: relaunch failed: {error:#}"));
                self.status = "管理员启动未完成".to_string();
                self.error = Some(format!("无法请求 UAC 提升：{error:#}"));
            }
        }
    }

    fn poll_core(&mut self, ctx: &egui::Context) {
        if let Some(rx) = &self.launch_rx {
            match rx.try_recv() {
                Ok(result) => {
                    boot_log("poll_core: launch worker returned");
                    self.launch_rx = None;
                    self.launching = false;
                    match result {
                        Ok(CoreLaunch::Process(launcher)) => {
                            boot_log(&format!(
                                "poll_core: process core launched endpoint={} catalog_core={}",
                                launcher.endpoint, self.catalog_core
                            ));
                            self.status = format!("核心已启动 · RPC {}", launcher.endpoint);
                            self.rpc = Some(CoreClient::rpc(
                                launcher.endpoint.clone(),
                                launcher.token.clone(),
                            ));
                            self.launcher = Some(launcher);
                            self.in_process_core = false;
                            self.error = None;
                            self.auto_restart_count = 0;
                        }
                        #[cfg(ppp_in_process_core)]
                        Ok(CoreLaunch::InProcess(client)) => {
                            boot_log("poll_core: in-process core started");
                            self.status = "核心已启动 · 同进程".to_string();
                            self.rpc = Some(client);
                            self.launcher = None;
                            self.in_process_core = true;
                            self.error = None;
                            self.auto_restart_count = 0;
                        }
                        Err(error) => {
                            boot_log(&format!("poll_core: core launch failed: {error}"));
                            self.status = "核心启动失败".to_string();
                            let message = if error.contains("Repeat runs") {
                                "检测到残留核心进程（Repeat runs）。请先在任务管理器结束 ppp-tui-core.exe / ppp.exe 再重试。".to_string()
                            } else {
                                error
                            };
                            self.error = Some(message);
                            if let Some(error) = self.error.as_deref() {
                                self.write_tui_log(error);
                            }
                        }
                    }
                }
                Err(std::sync::mpsc::TryRecvError::Disconnected) => {
                    boot_log("poll_core: launch worker channel disconnected");
                    self.launch_rx = None;
                    self.launching = false;
                    self.status = "核心启动线程已退出".to_string();
                }
                Err(std::sync::mpsc::TryRecvError::Empty) => {}
            }
        }

        // TCP connect can wait for several seconds.  Never perform it on the
        // egui thread: eframe keeps the native window hidden until the first
        // frame completes, so a blocking connect here makes the process look
        // like it never opened.
        if let Some(rx) = self.rpc_connect_rx.take() {
            match rx.try_recv() {
                Ok(Ok(stream)) => {
                    self.rpc_connecting = false;
                    match self.rpc.as_mut() {
                        Some(rpc) => match rpc.attach_stream(stream) {
                            Ok(()) => self.status = "正在连接核心".to_string(),
                            Err(error) => {
                                self.status = "RPC 连接失败".to_string();
                                self.error = Some(format!("{error:#}"));
                            }
                        },
                        None => {}
                    }
                }
                Ok(Err(error)) => {
                    self.rpc_connecting = false;
                    self.status = "RPC 连接失败".to_string();
                    self.error = Some(error);
                }
                Err(std::sync::mpsc::TryRecvError::Empty) => {
                    self.rpc_connect_rx = Some(rx);
                }
                Err(std::sync::mpsc::TryRecvError::Disconnected) => {
                    self.rpc_connecting = false;
                    self.status = "RPC 连接线程已退出".to_string();
                }
            }
        }

        let should_start_connect = self
            .rpc
            .as_ref()
            .map(|rpc| {
                !rpc.is_connected()
                    && !self.rpc_connecting
                    && self.last_connect_attempt.elapsed() >= Duration::from_secs(2)
            })
            .unwrap_or(false);
        if should_start_connect {
            let address = self
                .rpc
                .as_ref()
                .map(|rpc| rpc.address().to_string())
                .unwrap_or_default();
            let (tx, rx) = channel();
            self.rpc_connect_rx = Some(rx);
            self.rpc_connecting = true;
            self.last_connect_attempt = Instant::now();
            boot_log(&format!(
                "poll_core: starting async RPC connect address={address}"
            ));
            std::thread::spawn(move || {
                let result =
                    RpcClient::connect_socket(&address).map_err(|error| format!("{error:#}"));
                let _ = tx.send(result);
            });
        }

        let mut frames = Vec::new();
        if let Some(rpc) = self.rpc.as_mut() {
            if rpc.is_connected() {
                for _ in 0..64 {
                    match rpc.poll() {
                        Ok(Some(response)) => frames.push(response),
                        Ok(None) => break,
                        Err(error) => {
                            self.status = "RPC 通道错误".to_string();
                            self.error = Some(format!("{error:#}"));
                            rpc.disconnect();
                            break;
                        }
                    }
                }

                if rpc.is_authenticated()
                    && !rpc.has_pending()
                    && self.last_refresh.elapsed() >= Duration::from_secs(1)
                {
                    if rpc.request_command(CoreCommand::GetSnapshot).is_ok() {
                        self.last_refresh = Instant::now();
                    }
                }
            }
        }

        for response in frames {
            self.handle_response(response);
        }

        // Mature child-process handling: if the core exits on its own
        // (crash, external kill, or the core's own --link-restart respawn),
        // relaunch it automatically (bounded) so the TUI never silently
        // loses its core.  An intentional stop_core() already took the
        // launcher out of self.launcher, so this only fires on unexpected
        // exits.
        let process_exited = self
            .launcher
            .as_mut()
            .map(|launcher| launcher.has_exited().is_some())
            .unwrap_or(false);
        #[cfg(ppp_in_process_core)]
        let in_process_exited = self.in_process_core
            && self
                .rpc
                .as_ref()
                .map(|core| !core.is_running())
                .unwrap_or(true);
        #[cfg(not(ppp_in_process_core))]
        let in_process_exited = false;
        if !self.launching && (process_exited || in_process_exited) {
            let was_catalog = self.catalog_core;
            let view = self.view;
            boot_log("poll_core: core exited unexpectedly; scheduling auto-restart");
            self.rpc = None;
            self.rpc_connect_rx = None;
            self.rpc_connecting = false;
            self.launcher = None;
            self.in_process_core = false;
            self.snapshot = None;
            self.traffic.reset();
            self.auto_restart_count += 1;
            if self.auto_restart_count <= 3 {
                self.status = format!("核心异常退出，自动重启中（{}/3）…", self.auto_restart_count);
                boot_log(&format!(
                    "poll_core: auto-restart attempt {}/3 catalog_core={}",
                    self.auto_restart_count, was_catalog
                ));
                if was_catalog {
                    self.start_catalog_core();
                } else {
                    self.start_embedded_to(view);
                }
            } else {
                self.status = "核心多次异常退出，已停止自动重启".to_string();
                self.error = Some(
                    "核心连续 3 次异常退出，已停止自动重启；请检查网络与配置后手动启动。"
                        .to_string(),
                );
                boot_log("poll_core: auto-restart limit reached");
                self.auto_restart_count = 0;
            }
        }
        ctx.request_repaint_after(Duration::from_millis(100));
    }

    fn handle_response(&mut self, response: Response) {
        match response {
            Response::Result { method, value, .. } => match method.as_str() {
                "hello" => {
                    self.status = "已连接核心".to_string();
                    self.error = None;
                }
                "get_snapshot" => match serde_json::from_value::<Snapshot>(value) {
                    Ok(snapshot) => {
                        if self.catalog_core && !self.catalog_snapshot_logged {
                            let probe_summary = snapshot
                                .outbounds
                                .iter()
                                .filter(|outbound| {
                                    outbound.server_menu
                                        || outbound.tag.eq_ignore_ascii_case("main")
                                        || outbound.tag.to_ascii_lowercase().starts_with("server:")
                                })
                                .map(|outbound| {
                                    format!(
                                        "{}|{}|checked={}|reachable={}|rtt={}",
                                        outbound.tag,
                                        outbound.display_name,
                                        outbound.probe_checked,
                                        outbound.probe_reachable,
                                        outbound.probe_rtt_ms
                                    )
                                })
                                .collect::<Vec<_>>();
                            boot_log(&format!(
                                "catalog snapshot: outbounds={} menu={} probes=[{}]",
                                snapshot.outbounds.len(),
                                probe_summary.len(),
                                probe_summary.join("; ")
                            ));
                            self.catalog_snapshot_logged = true;
                        }
                        if let Some(previous) = &self.snapshot {
                            if snapshot.generation < previous.generation {
                                self.traffic.reset();
                            }
                        }
                        self.traffic.feed(
                            snapshot.traffic.in_bytes,
                            snapshot.traffic.out_bytes,
                            snapshot.monotonic_ms,
                        );
                        self.snapshot = Some(snapshot);
                        self.error = None;
                    }
                    Err(error) => self.error = Some(format!("快照解析失败: {error}")),
                },
                "switch_server" | "switch_rank1" => {
                    let accepted = value
                        .get("accepted")
                        .and_then(|v| v.as_bool())
                        .unwrap_or(false);
                    self.status = if accepted {
                        "服务器切换请求已提交".to_string()
                    } else {
                        "服务器切换被核心拒绝".to_string()
                    };
                }
                _ => {}
            },
            Response::Error { code, message, .. } => {
                if code == 401 {
                    if let Some(rpc) = self.rpc.as_mut() {
                        rpc.disconnect();
                    }
                }
                self.error = Some(format!("核心错误 {code}: {message}"));
            }
            Response::Event { .. } => {
                // Logs are intentionally not shown in the desktop client.
                // The core continues writing its normal log file.
            }
        }
    }

    #[cfg(not(ppp_in_process_core))]
    fn catalog_core_args(&self) -> Vec<String> {
        let mut args = normalize_core_args(split_command_line(&self.settings.command));
        for name in [
            "--headless",
            "--rpc-listen",
            "--rpc-token",
            "--rpc-max-clients",
            "--log-level",
            "--tui-log",
            "--tui-log-enabled",
            "--lwip",
            "--rt",
            "--dns",
            "--auto-restart",
            "--firewall-rules",
            "--config",
            "--set-http-proxy",
            "--nic",
            "--ngw",
            "--tun",
            "--tun-driver",
            "--tun-ip",
            "--tun-gw",
            "--tun-mask",
            "--tun-host",
            "--tun-vnet",
            "--tun-static",
            "--tun-flash",
            "--block-quic",
            "--tun-ssmt",
            "--tun-lease-time-in-seconds",
            "--tun-promisc",
            "--tun-route",
            "--tun-protect",
            "--proxy-http-port",
            "--proxy-socks-port",
            "--bypass-mode",
            "--bypass",
            "--bypass-nic",
            "--bypass-ngw",
            "--bypass6",
            "--bypass-nic6",
            "--bypass-ngw6",
            "--dns-rules",
            "--geo-rules",
            "--geosite",
            "--geoip",
        ] {
            remove_command_argument(&mut args, name);
        }
        set_command_argument(&mut args, "--mode", "proxy");
        set_optional_command_argument(&mut args, "--server-dir", &self.settings.server_dir);
        set_command_argument(&mut args, "--proxy-http-port", "0");
        set_command_argument(&mut args, "--proxy-socks-port", "0");
        set_command_argument(&mut args, "--catalog-only", "yes");
        set_bool_if_non_default(&mut args, "--rt", self.settings.rt, true);
        set_optional_if_not_default(&mut args, "--tun-mux", &self.settings.tun_mux, "");
        set_optional_if_not_default(
            &mut args,
            "--tun-mux-acceleration",
            &self.settings.tun_mux_acceleration,
            "",
        );
        set_optional_if_not_default(
            &mut args,
            "--log-file",
            &self.settings.log_file,
            "./ppp-core.log",
        );
        set_command_argument(&mut args, "--log-level", &self.settings.log_level);
        args
    }

    fn prepared_core_args(&self) -> Vec<String> {
        let mut args = normalize_core_args(split_command_line(&self.settings.command));
        let configured_mode = normalized_launch_mode(&self.settings.mode);
        let mode = if configured_mode == "client" && !self.settings.tun_enabled {
            "proxy"
        } else {
            configured_mode
        };
        let tun_active = mode == "client" && self.settings.tun_enabled;
        remove_command_argument(&mut args, "--lwip");
        if tun_active {
            match normalize_tcp_ip_cc(&self.settings.tcp_ip_cc).as_str() {
                "lwip" => set_command_argument(&mut args, "--lwip", "yes"),
                "ctcp" => set_command_argument(&mut args, "--lwip", "no"),
                _ => {}
            }
        }
        // These are owned by the Rust desktop launcher and must not be copied
        // from a pasted command line into the child core.
        for name in [
            "--headless",
            "--rpc-listen",
            "--rpc-token",
            "--rpc-max-clients",
            "--catalog-only",
            "--log-level",
            "--tui-log",
            "--tui-log-enabled",
        ] {
            remove_command_argument(&mut args, name);
        }
        set_command_argument(&mut args, "--mode", mode);
        remove_command_argument(&mut args, "--set-http-proxy");
        if self.settings.system_proxy_enabled && (mode == "client" || mode == "proxy") {
            set_command_argument(&mut args, "--set-http-proxy", "yes");
        }
        set_optional_command_argument(&mut args, "--config", &self.settings.config_path);
        set_optional_command_argument(&mut args, "--server-dir", &self.settings.server_dir);
        set_bool_if_non_default(&mut args, "--rt", self.settings.rt, true);
        set_optional_command_argument(&mut args, "--dns", &self.settings.dns);
        set_optional_command_argument(&mut args, "--auto-restart", &self.settings.auto_restart);
        if mode == "server" {
            set_optional_command_argument(
                &mut args,
                "--firewall-rules",
                &self.settings.firewall_rules,
            );
        } else {
            remove_command_argument(&mut args, "--firewall-rules");
        }
        if tun_active {
            set_optional_command_argument(&mut args, "--nic", &self.settings.nic);
            set_optional_command_argument(&mut args, "--ngw", &self.settings.ngw);
            set_optional_command_argument(&mut args, "--tun", &self.settings.tun);
            set_optional_command_argument(&mut args, "--tun-ip", &self.settings.tun_ip);
            set_optional_command_argument(&mut args, "--tun-gw", &self.settings.tun_gw);
            set_optional_command_argument(&mut args, "--tun-mask", &self.settings.tun_mask);
            set_bool_if_non_default(&mut args, "--tun-host", self.settings.tun_host, true);
            set_bool_if_non_default(&mut args, "--tun-vnet", self.settings.tun_vnet, true);
            set_bool_if_non_default(&mut args, "--tun-static", self.settings.tun_static, false);
            set_bool_if_non_default(&mut args, "--tun-flash", self.settings.tun_flash, false);
            set_bool_if_non_default(&mut args, "--block-quic", self.settings.block_quic, false);
        } else {
            for name in [
                "--nic",
                "--ngw",
                "--tun",
                "--tun-driver",
                "--tun-ip",
                "--tun-gw",
                "--tun-mask",
                "--tun-ssmt",
                "--tun-lease-time-in-seconds",
                "--tun-promisc",
                "--tun-route",
                "--tun-protect",
                "--tun-host",
                "--tun-vnet",
                "--tun-static",
                "--tun-flash",
                "--block-quic",
            ] {
                remove_command_argument(&mut args, name);
            }
        }
        if tun_active && cfg!(target_os = "windows") {
            set_optional_if_not_default(
                &mut args,
                "--tun-driver",
                &self.settings.tun_driver,
                "auto",
            );
            set_optional_command_argument(
                &mut args,
                "--tun-lease-time-in-seconds",
                &self.settings.tun_lease_time,
            );
        } else {
            remove_command_argument(&mut args, "--tun-driver");
            remove_command_argument(&mut args, "--tun-lease-time-in-seconds");
        }
        if tun_active && cfg!(any(target_os = "linux", target_os = "macos")) {
            set_optional_command_argument(&mut args, "--tun-ssmt", &self.settings.tun_ssmt);
            set_bool_if_non_default(&mut args, "--tun-promisc", self.settings.tun_promisc, true);
        } else {
            remove_command_argument(&mut args, "--tun-ssmt");
            remove_command_argument(&mut args, "--tun-promisc");
        }
        if tun_active && cfg!(target_os = "linux") {
            set_bool_if_non_default(&mut args, "--tun-route", self.settings.tun_route, false);
            set_bool_if_non_default(&mut args, "--tun-protect", self.settings.tun_protect, true);
        } else {
            remove_command_argument(&mut args, "--tun-route");
            remove_command_argument(&mut args, "--tun-protect");
        }
        if mode == "server" {
            for name in [
                "--tun-mux",
                "--tun-mux-acceleration",
                "--link-restart",
                "--proxy-http-port",
                "--proxy-socks-port",
            ] {
                remove_command_argument(&mut args, name);
            }
        } else {
            set_optional_command_argument(&mut args, "--tun-mux", &self.settings.tun_mux);
            set_optional_command_argument(
                &mut args,
                "--tun-mux-acceleration",
                &self.settings.tun_mux_acceleration,
            );
            set_optional_command_argument(&mut args, "--link-restart", &self.settings.link_restart);
        }
        if tun_active {
            set_optional_command_argument(
                &mut args,
                "--proxy-http-port",
                &self.settings.proxy_http_port,
            );
            set_optional_command_argument(
                &mut args,
                "--proxy-socks-port",
                &self.settings.proxy_socks_port,
            );
        } else if mode == "proxy" {
            // Proxy mode is used here as a stateless transport/control mode:
            // load the server directory and keep the RPC connection, but do
            // not create local HTTP/SOCKS listeners.
            set_command_argument(&mut args, "--proxy-http-port", "0");
            set_command_argument(&mut args, "--proxy-socks-port", "0");
        } else {
            remove_command_argument(&mut args, "--proxy-http-port");
            remove_command_argument(&mut args, "--proxy-socks-port");
        }
        let bypass_mode = match self
            .settings
            .bypass_mode
            .trim()
            .to_ascii_lowercase()
            .as_str()
        {
            "geo" => "geo",
            "no" => "no",
            _ => "ip",
        };
        remove_command_argument(&mut args, "--bypass-mode");
        if bypass_mode != "ip" {
            set_command_argument(&mut args, "--bypass-mode", bypass_mode);
        }
        if mode == "server" {
            for name in [
                "--bypass-mode",
                "--bypass",
                "--bypass6",
                "--dns-rules",
                "--geo-rules",
                "--geosite",
                "--geoip",
            ] {
                remove_command_argument(&mut args, name);
            }
        } else {
            match bypass_mode {
                "geo" => {
                    set_optional_if_not_default(
                        &mut args,
                        "--geo-rules",
                        &self.settings.geo_rules_file,
                        "./geo-rules.yaml",
                    );
                    set_optional_if_not_default(
                        &mut args,
                        "--geosite",
                        &self.settings.geosite_file,
                        "./geosite.dat",
                    );
                    set_optional_if_not_default(
                        &mut args,
                        "--geoip",
                        &self.settings.geoip_file,
                        "./geoip.dat",
                    );
                    for name in ["--bypass", "--bypass6", "--dns-rules"] {
                        remove_command_argument(&mut args, name);
                    }
                }
                "no" => {
                    for name in [
                        "--bypass",
                        "--bypass6",
                        "--dns-rules",
                        "--geo-rules",
                        "--geosite",
                        "--geoip",
                    ] {
                        remove_command_argument(&mut args, name);
                    }
                }
                _ => {
                    set_optional_if_not_default(
                        &mut args,
                        "--bypass",
                        &self.settings.bypass_file,
                        "./ip.txt",
                    );
                    set_optional_if_not_default(
                        &mut args,
                        "--bypass6",
                        &self.settings.bypass6_file,
                        "./ipv6.txt",
                    );
                    set_optional_if_not_default(
                        &mut args,
                        "--dns-rules",
                        &self.settings.dns_rules_file,
                        "./dns-rules.txt",
                    );
                    for name in ["--geo-rules", "--geosite", "--geoip"] {
                        remove_command_argument(&mut args, name);
                    }
                }
            }
        }
        if mode == "server" {
            for name in [
                "--bypass-nic",
                "--bypass-ngw",
                "--bypass-nic6",
                "--bypass-ngw6",
            ] {
                remove_command_argument(&mut args, name);
            }
        } else {
            set_optional_command_argument(&mut args, "--bypass-ngw", &self.settings.bypass_ngw);
            set_optional_command_argument(&mut args, "--bypass-ngw6", &self.settings.bypass_ngw6);
            if cfg!(target_os = "linux") {
                set_optional_command_argument(&mut args, "--bypass-nic", &self.settings.bypass_nic);
                set_optional_command_argument(
                    &mut args,
                    "--bypass-nic6",
                    &self.settings.bypass_nic6,
                );
            } else {
                remove_command_argument(&mut args, "--bypass-nic");
                remove_command_argument(&mut args, "--bypass-nic6");
            }
        }
        set_optional_if_not_default(
            &mut args,
            "--log-file",
            &self.settings.log_file,
            "./ppp-core.log",
        );
        set_command_argument(&mut args, "--log-level", &self.settings.log_level);
        args
    }

    fn start_embedded(&mut self) {
        self.start_embedded_to(View::Overview);
    }

    fn start_embedded_to(&mut self, view: View) {
        if self.launching {
            return;
        }
        if self.has_owned_core() {
            if self.catalog_core {
                self.stop_core(true);
            } else {
                return;
            }
        }
        self.save_settings();
        let args = self.prepared_core_args();
        self.write_tui_log(&format!("starting core: {}", format_command_preview(&args)));
        self.launch_core_with_args(args, false, view, "正在启动 VPN 核心…");
    }

    fn start_catalog_core(&mut self) {
        if self.launching || self.has_owned_core() {
            return;
        }
        #[cfg(ppp_in_process_core)]
        {
            // Server discovery/probing is already implemented in Rust.  The
            // in-process C++ core is reserved for the actual VPN runtime;
            // starting a temporary catalog core would initialize the global
            // C++ executor twice in one process.
            self.catalog_core = false;
            self.view = View::Servers;
            self.status = "服务器配置已就绪，请选择服务器后启动核心".to_string();
            return;
        }
        #[cfg(not(ppp_in_process_core))]
        {
            let args = self.catalog_core_args();
            self.launch_core_with_args(args, true, View::Servers, "正在准备服务器配置…");
        }
    }

    fn launch_core_with_args(
        &mut self,
        args: Vec<String>,
        catalog_core: bool,
        view: View,
        status: &str,
    ) {
        let working_dir = working_directory(&self.settings);
        boot_log(&format!(
            "launch_core_with_args: catalog_core={catalog_core} view={view:?} working_dir={} args={}",
            display_path_string(&working_dir),
            format_command_preview(&args),
        ));
        if !working_dir.is_dir() {
            self.error = Some(format!(
                "启动目录不存在: {}",
                display_path_string(&working_dir)
            ));
            return;
        }

        let core_path = self.settings.core_path.clone().map(|path| {
            let path = PathBuf::from(path);
            if path.is_relative() {
                working_dir.join(path)
            } else {
                path
            }
        });
        let (tx, rx) = channel();
        self.launch_rx = Some(rx);
        self.launching = true;
        self.catalog_core = catalog_core;
        self.error = None;
        self.status = status.to_string();
        self.view = view;
        std::thread::spawn(move || {
            boot_log("launch worker: spawn begin");
            let result = match core_path {
                Some(path) => {
                    Launcher::spawn_in(&path, &args, &working_dir).map(CoreLaunch::Process)
                }
                None => {
                    #[cfg(ppp_in_process_core)]
                    {
                        // The C++ core must not render its legacy console
                        // dashboard into the Rust window. Headless here
                        // means “no core UI”, not “use RPC”.
                        let mut in_process_args = args.clone();
                        set_command_argument(&mut in_process_args, "--headless", "yes");
                        CoreClient::in_process(&in_process_args).map(CoreLaunch::InProcess)
                    }
                    #[cfg(not(ppp_in_process_core))]
                    {
                        Launcher::spawn_embedded_in(&args, &working_dir).map(CoreLaunch::Process)
                    }
                }
            }
            .map_err(|error| format!("{error:#}"));
            boot_log(&format!(
                "launch worker: spawn end result={}",
                if result.is_ok() { "ok" } else { "error" }
            ));
            let _ = tx.send(result);
        });
    }

    fn attach_existing(&mut self) {
        let address = self.settings.rpc_address.trim().to_string();
        if address.is_empty() || self.settings.rpc_token.trim().is_empty() {
            self.error = Some("连接已有核心需要填写 RPC 地址和 Token".to_string());
            return;
        }
        self.save_settings();
        self.stop_core(false);
        self.rpc = Some(CoreClient::rpc(
            address,
            self.settings.rpc_token.trim().to_string(),
        ));
        self.in_process_core = false;
        self.status = "准备连接已有核心".to_string();
        self.error = None;
    }

    fn save_settings(&mut self) {
        // Apply the new destination before the confirmation log is emitted so
        // Save takes effect immediately, including when logging was disabled.
        configure_tui_logging(&self.settings);
        match self.settings.save() {
            Ok(()) => {
                self.write_tui_log("settings saved");
                self.status = format!("软件设置已保存到 {}", self.settings.settings_file);
                self.error = None;
            }
            Err(error) => {
                self.error = Some(format!("保存软件设置失败: {error:#}"));
            }
        }
    }

    fn handle_keyboard_shortcuts(&mut self, ctx: &egui::Context) {
        let mut page = None;
        let mut ctrl_s = false;
        let mut ctrl_r = false;
        let mut ctrl_enter = false;
        let mut ctrl_t = false;
        let mut ctrl_p = false;
        let mut route_mode = None;
        ctx.input(|input| {
            page = if input.key_pressed(egui::Key::F1)
                || (input.modifiers.ctrl && input.key_pressed(egui::Key::Num1))
            {
                Some(View::Overview)
            } else if input.key_pressed(egui::Key::F2)
                || (input.modifiers.ctrl && input.key_pressed(egui::Key::Num2))
            {
                Some(View::Network)
            } else if input.key_pressed(egui::Key::F3)
                || (input.modifiers.ctrl && input.key_pressed(egui::Key::Num3))
            {
                Some(View::Servers)
            } else if input.key_pressed(egui::Key::F4)
                || (input.modifiers.ctrl && input.key_pressed(egui::Key::Num4))
            {
                Some(View::Routes)
            } else if input.key_pressed(egui::Key::F5)
                || (input.modifiers.ctrl && input.key_pressed(egui::Key::Num5))
            {
                Some(View::Settings)
            } else {
                None
            };
            ctrl_s = input.modifiers.ctrl && input.key_pressed(egui::Key::S);
            ctrl_r = input.modifiers.ctrl && input.key_pressed(egui::Key::R);
            ctrl_enter = input.modifiers.ctrl && input.key_pressed(egui::Key::Enter);
            ctrl_t = input.modifiers.ctrl && input.key_pressed(egui::Key::T);
            ctrl_p = input.modifiers.ctrl && input.key_pressed(egui::Key::P);
            if input.modifiers.ctrl && input.key_pressed(egui::Key::I) {
                route_mode = Some("ip");
            } else if input.modifiers.ctrl && input.key_pressed(egui::Key::G) {
                route_mode = Some("geo");
            } else if input.modifiers.ctrl && input.key_pressed(egui::Key::N) {
                route_mode = Some("no");
            }
        });

        if let Some(view) = page {
            if let Some(id) = ctx.memory(|memory| memory.focused()) {
                ctx.memory_mut(|memory| memory.surrender_focus(id));
            }
            self.view = view;
            return;
        }
        if ctrl_s {
            self.save_settings();
        }
        if self.view == View::Routes {
            if let Some(mode) = route_mode {
                self.settings.bypass_mode = mode.to_string();
                self.status = format!(
                    "已选择 {} 分流；按 Ctrl+S 保存，按 Ctrl+R 应用并重启",
                    route_mode_title(mode)
                );
                self.error = None;
            }
            if ctrl_r {
                self.restart_core();
                return;
            }
        }
        if ctrl_t && !self.catalog_core && normalized_launch_mode(&self.settings.mode) == "client" {
            self.settings.tun_enabled = !self.settings.tun_enabled;
            self.status = format!(
                "TUN VPN 已{}；按 Ctrl+R 应用",
                if self.settings.tun_enabled {
                    "开启"
                } else {
                    "关闭"
                }
            );
        }
        if ctrl_p {
            self.settings.system_proxy_enabled = !self.settings.system_proxy_enabled;
            self.status = format!(
                "系统代理已{}；按 Ctrl+R 应用",
                if self.settings.system_proxy_enabled {
                    "开启"
                } else {
                    "关闭"
                }
            );
        }
        if ctrl_enter {
            match self.view {
                View::Servers if self.catalog_core || self.snapshot.is_none() => {
                    if !self.local_servers.is_empty() {
                        let index = self
                            .selected_local_server
                            .unwrap_or(0)
                            .min(self.local_servers.len() - 1);
                        self.select_local_server(index);
                        self.start_embedded_to(View::Servers);
                    }
                }
                View::Servers => {
                    if let Some(snapshot) = self.snapshot.clone() {
                        let outbounds: Vec<Outbound> = snapshot
                            .outbounds
                            .iter()
                            .filter(|outbound| outbound.server_menu || outbound.tag == "main")
                            .cloned()
                            .collect();
                        if let Some(outbound) = outbounds.get(
                            self.runtime_server_selection
                                .min(outbounds.len().saturating_sub(1)),
                        ) {
                            self.request_switch(outbound);
                        }
                    }
                }
                View::Settings => self.start_embedded_to(View::Settings),
                _ => {}
            }
        }
    }

    fn write_tui_log(&self, message: &str) {
        configure_tui_logging(&self.settings);
        let timestamp = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map(|duration| duration.as_secs())
            .unwrap_or_default();
        append_tui_log(&format!("[{timestamp}] {message}\n"));
    }

    fn refresh_server_catalog(&mut self) {
        let (local_servers, catalog_error) = load_server_catalog(&self.settings);
        self.local_servers = local_servers;
        self.selected_local_server =
            find_selected_local_server(&self.settings, &self.local_servers);
        self.error = catalog_error;
        self.status = if self.local_servers.is_empty() {
            "没有读取到服务器配置".to_string()
        } else {
            format!("已读取 {} 个服务器配置", self.local_servers.len())
        };
    }

    fn select_local_server(&mut self, index: usize) {
        let Some(profile) = self.local_servers.get(index).cloned() else {
            return;
        };
        let working_dir = working_directory(&self.settings);
        self.settings.config_path = core_path_string(&working_dir, &profile.path);
        self.selected_local_server = Some(index);
        self.status = format!("已选择 {}，点击启动核心进入总览", profile.name);
        self.error = None;
    }

    fn import_command_fields(&mut self) {
        let args = normalize_core_args(split_command_line(&self.settings.command));
        if args.is_empty() {
            self.error = Some("高级命令为空，没有可导入的参数".to_string());
            return;
        }

        if let Some(mode) = command_value(&args, "--mode") {
            self.settings.mode = normalized_launch_mode(&mode).to_string();
            self.settings.tun_enabled = self.settings.mode == "client";
        }
        import_text(&args, "--config", &mut self.settings.config_path);
        import_text(&args, "--server-dir", &mut self.settings.server_dir);
        import_text(&args, "--tun-ip", &mut self.settings.tun_ip);
        import_text(&args, "--tun-gw", &mut self.settings.tun_gw);
        import_text(&args, "--tun-mask", &mut self.settings.tun_mask);
        import_text(&args, "--tun-mux", &mut self.settings.tun_mux);
        import_text(
            &args,
            "--tun-mux-acceleration",
            &mut self.settings.tun_mux_acceleration,
        );
        import_text(&args, "--link-restart", &mut self.settings.link_restart);
        import_text(
            &args,
            "--proxy-http-port",
            &mut self.settings.proxy_http_port,
        );
        import_text(
            &args,
            "--proxy-socks-port",
            &mut self.settings.proxy_socks_port,
        );
        import_text(&args, "--bypass-mode", &mut self.settings.bypass_mode);
        import_text(&args, "--bypass", &mut self.settings.bypass_file);
        import_text(&args, "--bypass6", &mut self.settings.bypass6_file);
        import_text(&args, "--dns-rules", &mut self.settings.dns_rules_file);
        import_text(&args, "--geo-rules", &mut self.settings.geo_rules_file);
        import_text(&args, "--geosite", &mut self.settings.geosite_file);
        import_text(&args, "--geoip", &mut self.settings.geoip_file);
        import_text(&args, "--log-file", &mut self.settings.log_file);
        if let Some(value) = command_value(&args, "--log-level") {
            self.settings.log_level = normalize_log_level(&value);
        }
        if let Some(value) = command_value(&args, "--lwip") {
            self.settings.tcp_ip_cc = normalize_tcp_ip_cc(&value);
        }
        self.settings.rt = command_bool(&args, "--rt", self.settings.rt);
        import_text(&args, "--dns", &mut self.settings.dns);
        import_text(&args, "--auto-restart", &mut self.settings.auto_restart);
        import_text(&args, "--firewall-rules", &mut self.settings.firewall_rules);
        import_text(&args, "--nic", &mut self.settings.nic);
        import_text(&args, "--ngw", &mut self.settings.ngw);
        import_text(&args, "--tun", &mut self.settings.tun);
        import_text(&args, "--tun-driver", &mut self.settings.tun_driver);
        import_text(&args, "--tun-ssmt", &mut self.settings.tun_ssmt);
        import_text(
            &args,
            "--tun-lease-time-in-seconds",
            &mut self.settings.tun_lease_time,
        );
        import_text(&args, "--bypass-nic", &mut self.settings.bypass_nic);
        import_text(&args, "--bypass-ngw", &mut self.settings.bypass_ngw);
        import_text(&args, "--bypass-nic6", &mut self.settings.bypass_nic6);
        import_text(&args, "--bypass-ngw6", &mut self.settings.bypass_ngw6);
        self.settings.tun_promisc = command_bool(&args, "--tun-promisc", self.settings.tun_promisc);
        self.settings.tun_route = command_bool(&args, "--tun-route", self.settings.tun_route);
        self.settings.tun_protect = command_bool(&args, "--tun-protect", self.settings.tun_protect);
        self.settings.tui_log_enabled =
            command_bool(&args, "--tui-log-enabled", self.settings.tui_log_enabled);
        import_text(&args, "--tui-log", &mut self.settings.tui_log_file);
        self.settings.tun_host = command_bool(&args, "--tun-host", self.settings.tun_host);
        self.settings.tun_vnet = command_bool(&args, "--tun-vnet", self.settings.tun_vnet);
        self.settings.tun_static = command_bool(&args, "--tun-static", self.settings.tun_static);
        self.settings.tun_flash = command_bool(&args, "--tun-flash", self.settings.tun_flash);
        self.settings.block_quic = command_bool(&args, "--block-quic", self.settings.block_quic);
        self.settings.normalize_paths();
        self.settings.system_proxy_enabled = command_bool(
            &args,
            "--set-http-proxy",
            self.settings.system_proxy_enabled,
        );
        self.status = "已将高级命令参数导入到结构化设置".to_string();
        self.error = None;
    }

    fn restart_core(&mut self) {
        if !self.has_owned_core() {
            self.error = Some("只有由本窗口启动的核心才能在这里重启".to_string());
            return;
        }
        let view = self.view;
        self.stop_core(true);
        self.start_embedded_to(view);
    }

    fn stop_core(&mut self, shutdown: bool) {
        boot_log(&format!(
            "stop_core: shutdown={shutdown} launcher={} rpc={} launching={}",
            self.has_owned_core(),
            self.rpc.is_some(),
            self.launching,
        ));
        let owns_core = self.has_owned_core();
        if let Some(rpc) = self.rpc.as_mut() {
            // Never use the live UI session as the shutdown transport. It
            // may be unauthenticated or have an in-flight snapshot; the
            // owned Launcher performs a fresh authenticated shutdown below.
            rpc.disconnect();
        }
        #[cfg(ppp_in_process_core)]
        if shutdown && self.in_process_core {
            if let Some(rpc) = self.rpc.as_ref() {
                boot_log("stop_core: stopping in-process core and restoring network");
                let _ = rpc.stop_owned();
            }
        }
        if shutdown && owns_core {
            if let Some(launcher) = self.launcher.as_mut() {
                boot_log("stop_core: requesting graceful shutdown and network restore");
                let _ = launcher.request_graceful_shutdown();
            }
        }
        self.rpc = None;
        self.rpc_connect_rx = None;
        self.rpc_connecting = false;
        if let Some(mut launcher) = self.launcher.take() {
            launcher.stop();
        }
        self.in_process_core = false;
        self.catalog_core = false;
        self.snapshot = None;
        self.traffic.reset();
        self.status = if owns_core {
            "核心已停止，网络状态已完成清理".to_string()
        } else {
            "已断开核心".to_string()
        };
        boot_log("stop_core: state cleared");
    }

    fn stop_active_core_and_return_to_ready(&mut self) {
        if self.catalog_core {
            return;
        }
        self.stop_core(true);
        // A normal GUI launch keeps the internal control plane alive.  A
        // command-line launch remains a traditional one-shot invocation.
        if !self.startup_direct && self.settings.rpc_address.trim().is_empty() {
            self.start_catalog_core();
        }
    }

    fn request_switch(&mut self, outbound: &Outbound) {
        if let Some(rpc) = self.rpc.as_mut() {
            let command = CoreCommand::Switch {
                tag: outbound.tag.clone(),
                ranked_first: outbound.active,
            };
            if let Err(error) = rpc.request_command(command) {
                self.error = Some(format!("无法发送切换请求: {error}"));
            }
        } else {
            self.error = Some("核心尚未连接".to_string());
        }
    }

    fn request_log_level(&mut self) {
        if let Some(rpc) = self.rpc.as_mut() {
            if let Err(error) = rpc.request_command(CoreCommand::SetLogLevel {
                level: self.settings.log_level.clone(),
            }) {
                boot_log(&format!("set_log_level request failed: {error:#}"));
            }
        }
        self.save_settings();
    }

    fn top_bar(&mut self, ui: &mut egui::Ui) {
        ui.horizontal(|ui| {
            ui.heading(RichText::new("PPP PRIVATE NETWORK™ 2").color(ACCENT));
            ui.add_space(16.0);
            let (label, color) = match self.snapshot.as_ref().map(|s| s.phase.as_str()) {
                Some("connected") => ("已连接", GOOD),
                Some("connecting") | Some("reconnecting") => ("连接中", WARN),
                Some("failed") => ("失败", BAD),
                _ if self.catalog_core => ("已就绪", ACCENT),
                _ if self.launching => ("启动中", WARN),
                _ => ("未启动", MUTED),
            };
            ui.colored_label(color, label);
            if self.needs_admin() {
                if is_process_elevated() {
                    ui.colored_label(GOOD, "管理员");
                } else if uac_badge_button(ui).clicked() {
                    self.relaunch_as_administrator();
                }
            }
            ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                if ui.button("退出").clicked() {
                    boot_log("exit requested via button");
                    self.stop_core(true);
                    let _ = self.settings.save();
                    ui.ctx().send_viewport_cmd(egui::ViewportCommand::Close);
                }
                if self.has_owned_core() && !self.catalog_core {
                    if ui.button("停止核心").clicked() {
                        self.stop_active_core_and_return_to_ready();
                    }
                } else if self.rpc.is_some() && !self.catalog_core {
                    if ui.button("断开").clicked() {
                        self.stop_core(false);
                    }
                }
                if ui.button("设置").clicked() {
                    self.view = View::Settings;
                }
            });
        });
    }

    fn navigation(&mut self, ui: &mut egui::Ui) {
        ui.add_space(8.0);
        ui.label(RichText::new("控制面板 · F1-F5").strong().color(MUTED));
        ui.add_space(8.0);
        for (view, label) in [
            (View::Overview, "总览"),
            (View::Network, "网络"),
            (View::Servers, "服务器"),
            (View::Routes, "分流"),
            (View::Settings, "启动设置"),
        ] {
            let selected = self.view == view;
            let button = egui::Button::new(if selected {
                RichText::new(label).color(Color32::WHITE).strong()
            } else {
                RichText::new(label).color(MUTED)
            })
            .min_size(Vec2::new(150.0, 34.0));
            let response = ui.add(button);
            if response.clicked() {
                response.surrender_focus();
                self.view = view;
            }
            ui.add_space(4.0);
        }
        ui.add_space(10.0);
        ui.label(
            RichText::new("Tab/Shift+Tab 聚焦 · Enter/Space 执行")
                .small()
                .color(MUTED),
        );
    }

    fn content(&mut self, ui: &mut egui::Ui) {
        let frame = self.frame_counter;
        let verbose = self.verbose_frame_log();
        if verbose {
            boot_log(&format!(
                "frame={frame} content enter view={:?} error={} available={:?}",
                self.view,
                self.error.is_some(),
                ui.available_size(),
            ));
        }
        if let Some(error) = self.error.clone() {
            egui::Frame::group(ui.style()).show(ui, |ui| {
                ui.colored_label(BAD, error);
            });
            ui.add_space(10.0);
        }
        match self.view {
            View::Overview => {
                if verbose {
                    boot_log(&format!("frame={frame} content page=overview enter"));
                }
                let scroll = keyboard_scroll_request(ui.ctx(), true);
                egui::ScrollArea::vertical()
                    .id_salt("overview-scroll")
                    .scroll_bar_visibility(egui::scroll_area::ScrollBarVisibility::AlwaysVisible)
                    .show(ui, |ui| {
                        apply_keyboard_scroll(ui, scroll);
                        self.draw_overview(ui);
                    });
                if verbose {
                    boot_log(&format!("frame={frame} content page=overview exit"));
                }
            }
            View::Network => {
                if verbose {
                    boot_log(&format!("frame={frame} content page=network enter"));
                }
                self.draw_network(ui);
                if verbose {
                    boot_log(&format!("frame={frame} content page=network exit"));
                }
            }
            View::Servers => {
                if verbose {
                    boot_log(&format!("frame={frame} content page=servers enter"));
                }
                self.draw_servers(ui);
                if verbose {
                    boot_log(&format!("frame={frame} content page=servers exit"));
                }
            }
            View::Routes => {
                if verbose {
                    boot_log(&format!("frame={frame} content page=routes enter"));
                }
                self.draw_routes(ui);
                if verbose {
                    boot_log(&format!("frame={frame} content page=routes exit"));
                }
            }
            View::Settings => {
                if verbose {
                    boot_log(&format!("frame={frame} content page=settings enter"));
                }
                self.draw_settings(ui);
                if verbose {
                    boot_log(&format!("frame={frame} content page=settings exit"));
                }
            }
        }
        if verbose {
            boot_log(&format!("frame={frame} content exit"));
        }
    }

    fn draw_overview(&mut self, ui: &mut egui::Ui) {
        ui.heading("总览");
        ui.add_space(12.0);
        self.draw_connection_controls(ui);
        ui.add_space(16.0);
        if self.catalog_core {
            self.draw_catalog_latency(ui);
            return;
        }
        let Some(snapshot) = &self.snapshot else {
            self.empty_state(
                ui,
                "还没有运行中的核心",
                "可以从左侧任意进入服务器、网络、分流或启动设置。",
            );
            return;
        };
        ui.columns(3, |columns| {
            metric_card(
                &mut columns[0],
                "连接状态",
                phase_text(snapshot),
                phase_color(snapshot),
            );
            metric_card(&mut columns[1], "主出口", active_name(snapshot), ACCENT);
            metric_card(
                &mut columns[2],
                "运行时间",
                format_duration(snapshot.duration_ms),
                MUTED,
            );
        });
        ui.add_space(8.0);
        ui.columns(3, |columns| {
            metric_card(
                &mut columns[0],
                "下载速率",
                format_rate(
                    self.traffic
                        .latest()
                        .map(|p| p.rx_bytes_per_sec)
                        .unwrap_or(0),
                ),
                GOOD,
            );
            metric_card(
                &mut columns[1],
                "上传速率",
                format_rate(
                    self.traffic
                        .latest()
                        .map(|p| p.tx_bytes_per_sec)
                        .unwrap_or(0),
                ),
                ACCENT,
            );
            metric_card(
                &mut columns[2],
                "MUX",
                format!(
                    "{} · {} links",
                    snapshot.effective_mux_mode, snapshot.mux_active_links
                ),
                WARN,
            );
        });
        ui.add_space(16.0);
        wide_info_section(ui, "连接信息", |ui| {
            info_grid(
                ui,
                &[
                    ("Server", snapshot.vpn_server.as_str()),
                    ("GUID", snapshot.guid.as_str()),
                    ("Transport", snapshot.transport.as_str()),
                    ("Bypass Mode", snapshot.bypass_mode.as_str()),
                    ("Http Proxy", snapshot.http_proxy.as_str()),
                    ("Socks Proxy", snapshot.socks_proxy.as_str()),
                    ("Hosting", snapshot.role.as_str()),
                    (
                        "Total",
                        &format!(
                            "↓ {}   ↑ {}",
                            format_bytes(snapshot.traffic.in_bytes),
                            format_bytes(snapshot.traffic.out_bytes)
                        ),
                    ),
                ],
            );
        });
        ui.add_space(12.0);
        draw_traffic_graphs(ui, &self.traffic);
    }

    /// Control-plane overview: show per-server latency from the direct TCP
    /// probe loop without starting any core or VPN link.
    fn draw_catalog_latency(&mut self, ui: &mut egui::Ui) {
        ui.heading("服务器延迟");
        ui.add_space(8.0);
        ui.label(
            RichText::new(
                "延迟由界面独立探测（TCP 连接），不启动 VPN；在“服务器”页选择并启动核心。",
            )
            .color(MUTED),
        );
        ui.add_space(12.0);
        if self.local_servers.is_empty() {
            self.empty_state(
                ui,
                "未找到服务器配置",
                "请确认启动目录下存在 ./config/*.json，或到启动设置修改服务器目录。",
            );
            return;
        }
        let mut start_clicked = None;
        egui::ScrollArea::vertical()
            .id_salt("overview-latency-scroll")
            .scroll_bar_visibility(egui::scroll_area::ScrollBarVisibility::AlwaysVisible)
            .show(ui, |ui| {
                for (index, profile) in self.local_servers.iter().enumerate() {
                    let (probe_label, probe_color) = self.catalog_probe_status(index, profile);
                    let selected = self.selected_local_server == Some(index);
                    egui::Frame::group(ui.style())
                        .stroke(Stroke::new(
                            if selected { 1.5_f32 } else { 1.0_f32 },
                            if selected {
                                ACCENT
                            } else {
                                Color32::from_rgb(48, 54, 66)
                            },
                        ))
                        .show(ui, |ui| {
                            ui.horizontal(|ui| {
                                ui.label(RichText::new(&profile.name).strong());
                                ui.colored_label(probe_color, probe_label);
                                ui.with_layout(
                                    egui::Layout::right_to_left(egui::Align::Center),
                                    |ui| {
                                        if ui.button("选择并启动").clicked() {
                                            start_clicked = Some(index);
                                        }
                                    },
                                );
                            });
                            ui.label(
                                RichText::new(&profile.server)
                                    .small()
                                    .color(MUTED)
                                    .family(FontFamily::Monospace),
                            );
                        });
                    ui.add_space(6.0);
                }
            });
        if let Some(index) = start_clicked {
            self.select_local_server(index);
            self.start_embedded_to(View::Overview);
        }
    }

    fn draw_connection_controls(&mut self, ui: &mut egui::Ui) {
        let tun_allowed = normalized_launch_mode(&self.settings.mode) == "client";
        if !tun_allowed {
            // Proxy and server modes are never allowed to create a TUN. Keep
            // the setting normalized even when the control-plane core is up.
            self.settings.tun_enabled = false;
        }
        card(ui, "连接控制", |ui| {
            ui.horizontal(|ui| {
                ui.add_enabled_ui(tun_allowed, |ui| {
                    ui.checkbox(&mut self.settings.tun_enabled, "TUN VPN 模式");
                });
                ui.checkbox(&mut self.settings.system_proxy_enabled, "系统代理");
            });
            ui.label(
                RichText::new(
                    "TUN 和系统代理可以自由开关；修改启动模式后，用“应用设置并重启核心”生效。",
                )
                .small()
                .color(MUTED),
            );
            if !tun_allowed {
                ui.label(
                    RichText::new("当前启动模式不支持 TUN，TUN 开关会被忽略。")
                        .small()
                        .color(WARN),
                );
            }
            if tun_allowed && !self.settings.tun_enabled {
                ui.label(
                    RichText::new(
                        "关闭 TUN 时使用无监听 proxy 控制模式，仅用于保持核心连接和服务器选择。",
                    )
                    .small()
                    .color(WARN),
                );
            }
            ui.add_space(6.0);
            ui.horizontal(|ui| {
                if self.has_owned_core() && !self.catalog_core {
                    if ui.button("停止 VPN 核心").clicked() {
                        self.stop_active_core_and_return_to_ready();
                    }
                    if ui.button("应用设置并重启核心").clicked() {
                        self.restart_core();
                    }
                } else if self.catalog_core {
                    ui.label(
                        RichText::new("服务器配置已准备；进入服务器页选择后启动 VPN。")
                            .small()
                            .color(MUTED),
                    );
                } else if self.rpc.is_some() {
                    ui.label(
                        RichText::new("已连接外部核心；启动开关不会修改外部进程。")
                            .small()
                            .color(WARN),
                    );
                } else if ui.button("打开启动设置").clicked() {
                    self.view = View::Settings;
                }
                if self.rpc.is_none() && !self.has_owned_core() {
                    if ui.button("查看服务器").clicked() {
                        self.view = View::Servers;
                    }
                }
            });
        });
    }

    fn draw_network(&mut self, ui: &mut egui::Ui) {
        let Some(snapshot) = &self.snapshot else {
            self.empty_state(
                ui,
                "没有网络快照",
                "启动核心后这里显示 TUN、NIC、代理和 MUX 状态。",
            );
            return;
        };
        ui.heading("网络");
        ui.add_space(12.0);
        let scroll = keyboard_scroll_request(ui.ctx(), true);
        egui::ScrollArea::vertical()
            .id_salt("network-scroll")
            .scroll_bar_visibility(egui::scroll_area::ScrollBarVisibility::AlwaysVisible)
            .show(ui, |ui| {
                apply_keyboard_scroll(ui, scroll);
                if snapshot.network.mode == "proxy-only" {
                    card(ui, "TUNNEL", |ui| {
                        info_grid_owned(ui, tunnel_rows(snapshot));
                    });
                    ui.add_space(12.0);
                }
                if let Some(tun) = &snapshot.network.tun {
                    interface_card(ui, "TUN", tun, true, snapshot);
                }
                if let Some(nic) = &snapshot.network.nic {
                    interface_card(ui, "NIC", nic, false, snapshot);
                }
                if snapshot.network.tun.is_none() && snapshot.network.nic.is_none() {
                    ui.label(RichText::new("waiting for network information...").color(MUTED));
                }
            });
    }

    fn draw_servers(&mut self, ui: &mut egui::Ui) {
        if self.catalog_core || self.snapshot.is_none() {
            self.draw_local_servers(ui);
            return;
        }
        let snapshot = self.snapshot.as_ref().expect("snapshot checked above");
        let outbounds: Vec<Outbound> = snapshot
            .outbounds
            .iter()
            .filter(|o| o.server_menu || o.tag == "main")
            .cloned()
            .collect();
        ui.heading(format!("服务器 ({})", outbounds.len()));
        ui.label(
            RichText::new("点击“切换”立即提交，不再弹确认框；主服务器点击“Rank #1”切换入口。")
                .color(MUTED),
        );
        ui.add_space(10.0);
        let mut keyboard_move = 0_i32;
        let mut keyboard_activate = false;
        ui.input(|input| {
            if input.key_pressed(egui::Key::ArrowDown) {
                keyboard_move = 1;
            } else if input.key_pressed(egui::Key::ArrowUp) {
                keyboard_move = -1;
            }
            keyboard_activate =
                input.key_pressed(egui::Key::Enter) || input.key_pressed(egui::Key::Space);
        });
        if !outbounds.is_empty() {
            if keyboard_move != 0 {
                let current = self
                    .runtime_server_selection
                    .min(outbounds.len().saturating_sub(1));
                self.runtime_server_selection = if keyboard_move > 0 {
                    (current + 1) % outbounds.len()
                } else if current == 0 {
                    outbounds.len() - 1
                } else {
                    current - 1
                };
            }
            if keyboard_activate {
                if let Some(outbound) = outbounds.get(self.runtime_server_selection) {
                    self.request_switch(outbound);
                }
            }
        }
        let mut clicked_selection = None;
        let keyboard_scroll_selected = keyboard_move != 0 || keyboard_activate;
        let page_scroll = keyboard_scroll_request(ui.ctx(), false);
        egui::ScrollArea::vertical()
            .id_salt("runtime-servers-scroll")
            .scroll_bar_visibility(egui::scroll_area::ScrollBarVisibility::AlwaysVisible)
            .show(ui, |ui| {
                apply_keyboard_scroll(ui, page_scroll);
                for (index, outbound) in outbounds.iter().enumerate() {
                    let selected = self.runtime_server_selection == index;
                    let selected_color = if selected || outbound.active {
                        ACCENT
                    } else {
                        MUTED
                    };
                    let mut action_rect = None;
                    let frame_response = egui::Frame::group(ui.style())
                        .stroke(Stroke::new(
                            if selected || outbound.active {
                                1.5_f32
                            } else {
                                1.0_f32
                            },
                            selected_color,
                        ))
                        .show(ui, |ui| {
                            ui.horizontal(|ui| {
                                let marker = if selected {
                                    "> "
                                } else if outbound.active {
                                    "* "
                                } else {
                                    "  "
                                };
                                ui.add_sized(
                                    [230.0, 24.0],
                                    egui::Label::new(
                                        RichText::new(format!("{marker}{}", outbound.display_name))
                                            .strong(),
                                    ),
                                );
                                ui.colored_label(state_color(outbound.state), usage_text(outbound));
                                ui.with_layout(
                                    egui::Layout::right_to_left(egui::Align::Center),
                                    |ui| {
                                        let label =
                                            if outbound.active { "Rank #1" } else { "切换" };
                                        let response = ui.button(label);
                                        action_rect = Some(response.rect);
                                        if response.has_focus() {
                                            response.scroll_to_me(Some(egui::Align::Center));
                                        }
                                        if response.clicked() {
                                            self.request_switch(outbound);
                                        }
                                    },
                                );
                            });
                            ui.label(
                                RichText::new(entry_text(outbound))
                                    .color(MUTED)
                                    .family(FontFamily::Monospace),
                            );
                        });
                    if selected && keyboard_scroll_selected {
                        ui.scroll_to_rect(frame_response.response.rect, Some(egui::Align::Center));
                    }
                    if selectable_card_clicked(
                        ui,
                        frame_response.response.rect,
                        action_rect,
                        ui.make_persistent_id(("runtime-server-card", index)),
                    ) {
                        clicked_selection = Some(index);
                    }
                    ui.add_space(8.0);
                }
            });
        if let Some(index) = clicked_selection {
            self.runtime_server_selection = index;
        }
    }

    fn draw_local_servers(&mut self, ui: &mut egui::Ui) {
        let frame = self.frame_counter;
        let verbose = self.verbose_frame_log();
        if verbose {
            boot_log(&format!(
                "frame={frame} local_servers enter count={} selected={:?} catalog_core={} launching={}",
                self.local_servers.len(),
                self.selected_local_server,
                self.catalog_core,
                self.launching,
            ));
        }
        ui.heading(format!("服务器 ({})", self.local_servers.len()));
        ui.label(
            RichText::new("已读取工作目录下的服务器 JSON；选择后启动实际 VPN 核心。").color(MUTED),
        );
        let mut keyboard_move = 0_i32;
        let mut keyboard_start = false;
        ui.input(|input| {
            if input.key_pressed(egui::Key::ArrowDown) {
                keyboard_move = 1;
            } else if input.key_pressed(egui::Key::ArrowUp) {
                keyboard_move = -1;
            }
            keyboard_start =
                input.key_pressed(egui::Key::Enter) || input.key_pressed(egui::Key::Space);
        });
        if !self.local_servers.is_empty() && keyboard_move != 0 {
            let next = match self.selected_local_server {
                None if keyboard_move > 0 => 0,
                None => self.local_servers.len() - 1,
                Some(index) => {
                    let current = index.min(self.local_servers.len() - 1);
                    if keyboard_move > 0 {
                        (current + 1) % self.local_servers.len()
                    } else if current == 0 {
                        self.local_servers.len() - 1
                    } else {
                        current - 1
                    }
                }
            };
            self.select_local_server(next);
        }
        if verbose {
            boot_log(&format!(
                "frame={frame} local_servers controls keyboard_move={keyboard_move} keyboard_start={keyboard_start} selected={:?}",
                self.selected_local_server,
            ));
        }
        if verbose {
            boot_log(&format!("frame={frame} local_servers before_header_space"));
        }
        ui.add_space(10.0);
        if verbose {
            boot_log(&format!("frame={frame} local_servers after_header_space"));
        }
        ui.horizontal(|ui| {
            if verbose {
                boot_log(&format!("frame={frame} local_servers header enter"));
            }
            if ui.button("刷新服务器配置").clicked() {
                self.refresh_server_catalog();
            }
            let can_start = self.selected_local_server.is_some()
                && !self.launching
                && (!self.has_owned_core() || self.catalog_core);
            if ui
                .add_enabled(can_start, egui::Button::new("选择并启动"))
                .clicked()
            {
                self.start_embedded_to(View::Servers);
            }
            if let Some(index) = self.selected_local_server {
                if let Some(profile) = self.local_servers.get(index) {
                    ui.label(RichText::new(format!("已选：{}", profile.name)).color(ACCENT));
                }
            }
            if verbose {
                boot_log(&format!("frame={frame} local_servers header exit"));
            }
        });
        if verbose {
            boot_log(&format!("frame={frame} local_servers after_header"));
        }
        ui.add_space(10.0);
        if verbose {
            boot_log(&format!("frame={frame} local_servers before_empty_check"));
        }
        if self.local_servers.is_empty() {
            self.empty_state(
                ui,
                "未找到服务器配置",
                "请确认启动目录下存在 ./config/*.json，或到启动设置修改服务器目录。",
            );
            return;
        }

        let mut clicked = None;
        // Enter/Space on a server row selects that row.  Starting is kept on
        // the dedicated button (or Ctrl+Enter) so keyboard selection cannot
        // accidentally launch a server while moving through the list.
        let mut start_clicked = None;
        let keyboard_scroll_selected = keyboard_move != 0 || keyboard_start;
        let page_scroll = keyboard_scroll_request(ui.ctx(), false);
        if verbose {
            boot_log(&format!(
                "frame={frame} local_servers before_scroll_show page_scroll={page_scroll:?}"
            ));
        }
        egui::ScrollArea::vertical()
            .id_salt("local-servers-scroll")
            .show(ui, |ui| {
                if verbose {
                    boot_log(&format!("frame={frame} local_servers scroll enter"));
                }
                apply_keyboard_scroll(ui, page_scroll);
                for (index, profile) in self.local_servers.iter().enumerate() {
                    if verbose {
                        boot_log(&format!(
                            "frame={frame} local_servers card={index} name={} entries={} begin",
                            profile.name,
                            profile.entries.len(),
                        ));
                    }
                    let selected = self.selected_local_server == Some(index);
                    let stroke_color = if selected {
                        ACCENT
                    } else {
                        Color32::from_rgb(48, 54, 66)
                    };
                    let mut action_rect = None;
                    let frame_response = egui::Frame::group(ui.style())
                        .stroke(Stroke::new(
                            if selected { 1.5_f32 } else { 1.0_f32 },
                            stroke_color,
                        ))
                        .show(ui, |ui| {
                            ui.horizontal(|ui| {
                                let marker = if selected { ">" } else { " " };
                                let label_response = ui.selectable_label(
                                    selected,
                                    RichText::new(format!("{marker} {}", profile.name)).strong(),
                                );
                                if label_response.has_focus() {
                                    label_response.scroll_to_me(Some(egui::Align::Center));
                                }
                                let focused_activate = label_response.has_focus()
                                    && ui.input(|input| {
                                        input.key_pressed(egui::Key::Enter)
                                            || input.key_pressed(egui::Key::Space)
                                    });
                                if label_response.clicked() || focused_activate {
                                    clicked = Some(index);
                                }
                                let (probe_label, probe_color) =
                                    self.catalog_probe_status(index, profile);
                                ui.colored_label(probe_color, probe_label);
                                ui.with_layout(
                                    egui::Layout::right_to_left(egui::Align::Center),
                                    |ui| {
                                        let response = ui
                                            .add_enabled(selected, egui::Button::new("选择并启动"));
                                        action_rect = Some(response.rect);
                                        if response.has_focus() {
                                            response.scroll_to_me(Some(egui::Align::Center));
                                        }
                                        if response.clicked() {
                                            start_clicked = Some(index);
                                        }
                                    },
                                );
                            });
                            ui.label(
                                RichText::new(&profile.server)
                                    .color(MUTED)
                                    .family(FontFamily::Monospace),
                            );
                            ui.horizontal_wrapped(|ui| {
                                ui.label(format!("Entry: {}", profile.entries.len()));
                            });
                            if profile.entries.len() > 1 {
                                ui.label(
                                    RichText::new(format!("入口：{}", profile.entries.join(" · ")))
                                        .small()
                                        .color(MUTED),
                                );
                            }
                        });
                    if selected && keyboard_scroll_selected {
                        ui.scroll_to_rect(frame_response.response.rect, Some(egui::Align::Center));
                    }
                    if selectable_card_clicked(
                        ui,
                        frame_response.response.rect,
                        action_rect,
                        ui.make_persistent_id(("local-server-card", index)),
                    ) {
                        clicked = Some(index);
                    }
                    ui.add_space(8.0);
                    if verbose {
                        boot_log(&format!("frame={frame} local_servers card={index} end"));
                    }
                }
                if verbose {
                    boot_log(&format!("frame={frame} local_servers scroll exit"));
                }
            });
        if verbose {
            boot_log(&format!("frame={frame} local_servers after_scroll_show"));
        }
        if verbose {
            boot_log(&format!("frame={frame} local_servers after_scroll clicked={clicked:?} start_clicked={start_clicked:?}"));
        }
        if let Some(index) = clicked.or(start_clicked) {
            self.select_local_server(index);
        }
        if start_clicked.is_some() {
            self.start_embedded_to(View::Servers);
        }
        if verbose {
            boot_log(&format!("frame={frame} local_servers exit"));
        }
    }

    fn catalog_probe_status(
        &self,
        profile_index: usize,
        profile: &LocalServerProfile,
    ) -> (String, Color32) {
        // Prefer the core's authoritative probe data once a real core is
        // running and reports checked outbound rows.  While the control-plane
        // core has no outbound rows (or before any core starts), fall back to
        // the desktop client's own TCP probes so latency is always visible.
        if let Some(snapshot) = &self.snapshot {
            let name = profile.name.trim();
            let profile_endpoint = endpoint_match_key(&profile.server);
            let outbound = snapshot.outbounds.iter().find(|outbound| {
                let tag_name = outbound
                    .tag
                    .strip_prefix("server:")
                    .unwrap_or(&outbound.tag);
                outbound.display_name.eq_ignore_ascii_case(name)
                    || tag_name.eq_ignore_ascii_case(name)
                    || endpoint_match_key(&outbound.server) == profile_endpoint
            });
            // The C++ core may merge a server row into a GEO/main row and
            // change its presentation tag.  Both sides still enumerate
            // server-menu entries in server-directory order, so use that
            // stable order as the final mapping.
            let outbound = outbound.or_else(|| {
                snapshot
                    .outbounds
                    .iter()
                    .filter(|outbound| {
                        outbound.server_menu
                            || outbound.tag.to_ascii_lowercase().starts_with("server:")
                    })
                    .nth(profile_index)
            });
            if let Some(outbound) = outbound {
                if outbound.probe_enabled && outbound.probe_checked {
                    if outbound.probe_reachable && outbound.probe_rtt_ms >= 0 {
                        return (format!("{} ms", outbound.probe_rtt_ms), GOOD);
                    }
                    return ("不可达".to_string(), BAD);
                }
            }
        }
        self.rust_probe_status(profile)
    }

    /// Direct TCP probe result from the desktop client's own probe loop.
    fn rust_probe_status(&self, profile: &LocalServerProfile) -> (String, Color32) {
        let Ok(table) = self.probe_table.lock() else {
            return ("等待探测".to_string(), MUTED);
        };
        match table.state(&profile.name) {
            Some(ProbeState::Ok(rtt)) => (format!("{rtt} ms"), GOOD),
            Some(ProbeState::Unreachable) => ("不可达".to_string(), BAD),
            Some(ProbeState::Probing) => ("探测中".to_string(), WARN),
            Some(ProbeState::Pending) | None => ("等待探测".to_string(), MUTED),
        }
    }

    fn draw_routes(&mut self, ui: &mut egui::Ui) {
        ui.heading("分流");
        let can_restart = self.has_owned_core() && !self.launching;
        ui.horizontal(|ui| {
            ui.label(
                RichText::new("选择分流引擎并编辑对应文件；保存后可应用设置并重启核心。")
                    .color(MUTED),
            );
            ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                if ui.button("保存分流设置").clicked() {
                    self.save_settings();
                }
                if ui
                    .add_enabled(can_restart, egui::Button::new("应用分流并重启核心"))
                    .clicked()
                {
                    self.restart_core();
                }
                if !can_restart {
                    ui.label(RichText::new("核心未由本窗口启动").small().color(MUTED));
                }
            });
        });
        tui_plain_rule(ui);
        let page_scroll = keyboard_scroll_request(ui.ctx(), false);
        egui::ScrollArea::vertical()
            .id_salt("routes-scroll")
            .scroll_bar_visibility(egui::scroll_area::ScrollBarVisibility::AlwaysVisible)
            .show(ui, |ui| {
                apply_keyboard_scroll(ui, page_scroll);
                self.draw_routes_body(ui);
            });
    }

    fn draw_routes_body(&mut self, ui: &mut egui::Ui) {
        ui.add_space(8.0);
        let selected_mode = match self
            .settings
            .bypass_mode
            .trim()
            .to_ascii_lowercase()
            .as_str()
        {
            "geo" => "geo",
            "no" => "no",
            _ => "ip",
        };
        let mut next_mode = None;
        let card_width = ((ui.available_width() - 16.0) / 3.0).max(170.0);
        ui.horizontal(|ui| {
            for (mode, title, detail) in [
                ("ip", "IP 分流", "ip.txt / ipv6.txt"),
                ("geo", "GEO 分流", "geo-rules.yaml"),
                ("no", "关闭用户分流", "仅保留必要路由"),
            ] {
                let selected = selected_mode == mode;
                let fill = if selected {
                    Color32::from_rgb(27, 58, 91)
                } else {
                    Color32::from_rgb(19, 21, 24)
                };
                let label = RichText::new(format!("{}\n{}", title, detail))
                    .color(if selected { Color32::WHITE } else { MUTED })
                    .strong();
                if ui
                    .add_sized(
                        [card_width, 62.0],
                        egui::Button::new(label)
                            .fill(fill)
                            .stroke(Stroke::new(1.0_f32, if selected { ACCENT } else { MUTED })),
                    )
                    .clicked()
                {
                    next_mode = Some(mode);
                }
            }
        });
        if let Some(mode) = next_mode {
            self.settings.bypass_mode = mode.to_string();
            self.status = format!("已选择 {} 分流，保存或应用后生效", route_mode_title(mode));
            self.error = None;
        }
        ui.add_space(12.0);

        let base = working_directory(&self.settings);
        match selected_mode {
            "geo" => {
                card(ui, "GEO 分流文件", |ui| {
                    route_file_field(
                        ui,
                        "GEO 规则文件",
                        &mut self.settings.geo_rules_file,
                        "./geo-rules.yaml",
                        &base,
                    );
                    route_file_field(
                        ui,
                        "GeoSite 数据",
                        &mut self.settings.geosite_file,
                        "./geosite.dat",
                        &base,
                    );
                    route_file_field(
                        ui,
                        "GeoIP 数据",
                        &mut self.settings.geoip_file,
                        "./geoip.dat",
                        &base,
                    );
                });
            }
            "no" => {
                card(ui, "关闭用户分流", |ui| {
                    ui.label(
                        RichText::new(
                            "当前模式不读取 IP/GEO 用户规则；普通流量进入当前 VPN 出口，服务器必要路由仍然保留。",
                        )
                        .color(WARN),
                    );
                });
            }
            _ => {
                card(ui, "IP 分流文件", |ui| {
                    route_file_field(
                        ui,
                        "IPv4 分流文件",
                        &mut self.settings.bypass_file,
                        "./ip.txt",
                        &base,
                    );
                    route_file_field(
                        ui,
                        "IPv6 分流文件",
                        &mut self.settings.bypass6_file,
                        "./ipv6.txt",
                        &base,
                    );
                    route_file_field(
                        ui,
                        "DNS 规则文件",
                        &mut self.settings.dns_rules_file,
                        "./dns-rules.txt",
                        &base,
                    );
                });
            }
        }

        ui.add_space(12.0);
        if let Some(snapshot) = &self.snapshot {
            card(ui, "运行时分流摘要", |ui| {
                let route = &snapshot.routes;
                let mode = if snapshot.bypass_mode.is_empty() {
                    selected_mode
                } else {
                    snapshot.bypass_mode.as_str()
                };
                info_grid(
                    ui,
                    &[
                        ("当前模式", mode),
                        ("Bypass Gateway", route.bypass_gateway.as_str()),
                        ("GEO 规则", &snapshot.geo.rule_count.to_string()),
                        ("DNS 规则", &route.dns_rule_count.to_string()),
                    ],
                );
            });
            if snapshot.bypass_mode == "geo" {
                ui.add_space(12.0);
                card(ui, "Split Rules", |ui| {
                    if snapshot.geo.split_rules.is_empty() {
                        ui.label(RichText::new("none").color(MUTED));
                    } else {
                        for rule in &snapshot.geo.split_rules {
                            let display = if rule.display.is_empty() {
                                rule.outbound.as_str()
                            } else {
                                rule.display.as_str()
                            };
                            ui.label(
                                RichText::new(format!(
                                    "{} -> {} ({})",
                                    rule.matcher, rule.outbound, display
                                ))
                                .family(FontFamily::Monospace),
                            );
                        }
                    }
                });
            }
        } else {
            card(ui, "分流预览", |ui| {
                ui.label(
                    RichText::new(
                        "核心尚未启动；当前显示的是待应用设置。启动核心后这里会显示实际加载的规则数量。",
                    )
                    .color(MUTED),
                );
            });
        }
    }

    fn draw_settings(&mut self, ui: &mut egui::Ui) {
        ui.heading("启动设置");
        ui.horizontal(|ui| {
            if ui.button("保存设置").clicked() {
                self.save_settings();
            }
            ui.label(RichText::new("保存到本地软件配置文件").small().color(MUTED));
        });
        tui_plain_rule(ui);
        let page_scroll = keyboard_scroll_request(ui.ctx(), false);
        egui::ScrollArea::vertical()
            .id_salt("startup-settings-scroll")
            .scroll_bar_visibility(egui::scroll_area::ScrollBarVisibility::AlwaysVisible)
            .show(ui, |ui| {
                apply_keyboard_scroll(ui, page_scroll);
                self.draw_settings_body(ui);
            });
    }

    fn proxy_port_hint(&self, http: bool) -> String {
        let configured = if http {
            self.settings.proxy_http_port.trim()
        } else {
            self.settings.proxy_socks_port.trim()
        };
        if !configured.is_empty() {
            return format!("当前手动覆盖为 {configured}；清空后恢复跟随配置/Main");
        }

        let runtime = self.snapshot.as_ref().and_then(|snapshot| {
            let value = if http {
                &snapshot.http_proxy
            } else {
                &snapshot.socks_proxy
            };
            proxy_port_from_display(value)
        });
        let configured_file = self.config_proxy_port(http);
        match runtime.or(configured_file) {
            Some(port) => format!("留空使用当前配置/Main 默认端口 {port}；填写后覆盖"),
            None => "留空读取当前配置/Main；填写 0 可关闭监听".to_string(),
        }
    }

    fn config_proxy_port(&self, http: bool) -> Option<String> {
        let base = working_directory(&self.settings);
        let path = resolve_from_working_dir(&base, &self.settings.config_path);
        let contents = fs::read_to_string(path).ok()?;
        let contents = contents.strip_prefix('\u{feff}').unwrap_or(&contents);
        let root = serde_json::from_str::<Value>(contents).ok()?;
        let section = if http { "http-proxy" } else { "socks-proxy" };
        let port = root.get("client")?.get(section)?.get("port")?;
        if let Some(value) = port.as_i64() {
            return Some(value.to_string());
        }
        if let Some(value) = port.as_u64() {
            return Some(value.to_string());
        }
        port.as_str().map(ToOwned::to_owned)
    }

    fn draw_settings_body(&mut self, ui: &mut egui::Ui) {
        // Keep every path field in the portable form used by the command
        // interface. This also converts an old saved Windows path as soon as
        // the settings page is opened, instead of waiting for Save.
        self.settings.normalize_paths();
        let http_proxy_hint = self.proxy_port_hint(true);
        let socks_proxy_hint = self.proxy_port_hint(false);
        ui.label(
            RichText::new(
                "填写核心启动目录和参数。启动后核心仍以内嵌方式运行，日志写入 ppp-core.log；修改后可随时点击右上角保存设置。",
            )
            .color(MUTED),
        );
        ui.add_space(14.0);
        card(ui, "软件配置文件", |ui| {
            labeled_text(
                ui,
                "保存路径",
                &mut self.settings.settings_file,
                "./ppp-tui.json",
            );
            ui.label(
                RichText::new("启动目录、命令、服务器目录和分流设置会保存到这个本地 JSON 文件。")
                    .small()
                    .color(MUTED),
            );
        });
        ui.add_space(12.0);
        card(ui, "核心启动", |ui| {
            let mut core_path = self.settings.core_path.clone().unwrap_or_default();
            labeled_text(
                ui,
                "外部核心路径",
                &mut core_path,
                "可选；留空使用内置核心，例如 ./ppp.exe",
            );
            self.settings.core_path = if core_path.trim().is_empty() {
                None
            } else {
                Some(core_path)
            };
            ui.add_space(8.0);
            labeled_text(
                ui,
                "启动目录",
                &mut self.settings.working_dir,
                "核心相对路径配置以此目录为基准",
            );
            labeled_cli_text(
                ui,
                "核心日志文件",
                "--log-file",
                "C++ 核心的调试与运行日志",
                &mut self.settings.log_file,
                "例如 ./ppp_win.log；留空使用 ./ppp-core.log",
            );
            let mut log_level_changed = false;
            ui.horizontal(|ui| {
                ui.label(RichText::new("核心日志等级").color(MUTED));
                egui::ComboBox::from_id_salt("core-log-level")
                    .selected_text(self.settings.log_level.as_str())
                    .show_ui(ui, |ui| {
                        for level in ["error", "warn", "info", "debug", "none"] {
                            if ui
                                .selectable_value(
                                    &mut self.settings.log_level,
                                    level.to_string(),
                                    level,
                                )
                                .changed()
                            {
                                log_level_changed = true;
                            }
                        }
                    });
                ui.label(
                    RichText::new("默认 error；排查时开启 info/debug")
                        .small()
                        .color(MUTED),
                );
            });
            if log_level_changed {
                self.settings.log_level = normalize_log_level(&self.settings.log_level);
                self.request_log_level();
            }
            labeled_cli_text(
                ui,
                "TUI 日志文件",
                "--tui-log",
                "Rust 界面的启动、设置和错误日志；由开关控制是否写入",
                &mut self.settings.tui_log_file,
                "例如 ./ppp-tui.log；留空使用工作目录下的默认文件",
            );
            if ui
                .checkbox(&mut self.settings.tui_log_enabled, "启用 TUI 日志")
                .changed()
            {
                configure_tui_logging(&self.settings);
            }
            ui.label(
                RichText::new("关闭后不写入启动、设置和错误日志；保存后立即生效")
                    .small()
                    .color(MUTED),
            );
        });
        ui.add_space(12.0);
        card(ui, "启动命令接口", |ui| {
            ui.label(
                RichText::new(
                    "模式、配置、TUN、MUX、分流、系统代理和日志由上方设置接管；高级参数只填写未覆盖的额外 CLI 参数。",
                )
                .small()
                .color(MUTED),
            );
            ui.add_space(8.0);
            ui.horizontal(|ui| {
                ui.label(RichText::new("启动模式 {--mode}").color(MUTED));
                let combo_id = ui.make_persistent_id("startup-mode");
                let combo_response = egui::ComboBox::from_id_salt("startup-mode")
                    .selected_text(match normalized_launch_mode(&self.settings.mode) {
                        "proxy" => "Proxy（无 TUN、无监听端口）",
                        "server" => "服务端",
                        _ => "客户端",
                    })
                    .show_ui(ui, |ui| {
                        ui.selectable_value(
                            &mut self.settings.mode,
                            "client".to_string(),
                            "客户端（可启用 TUN）",
                        );
                        ui.selectable_value(
                            &mut self.settings.mode,
                            "proxy".to_string(),
                            "Proxy（无 TUN、无监听端口）",
                        );
                        ui.selectable_value(
                            &mut self.settings.mode,
                            "server".to_string(),
                            "服务端",
                        );
                    });
                if combo_response.response.has_focus() {
                    combo_response
                        .response
                        .scroll_to_me(Some(egui::Align::Center));
                    let (activate, direction) = ui.input(|input| {
                        let direction = if input.key_pressed(egui::Key::ArrowDown) {
                            1_i32
                        } else if input.key_pressed(egui::Key::ArrowUp) {
                            -1_i32
                        } else {
                            0
                        };
                        (
                            input.key_pressed(egui::Key::Enter)
                                || input.key_pressed(egui::Key::Space),
                            direction,
                        )
                    });
                    if direction != 0 {
                        let modes = ["client", "proxy", "server"];
                        let current = modes
                            .iter()
                            .position(|mode| *mode == normalized_launch_mode(&self.settings.mode))
                            .unwrap_or(0);
                        let next = if direction > 0 {
                            (current + 1) % modes.len()
                        } else if current == 0 {
                            modes.len() - 1
                        } else {
                            current - 1
                        };
                        self.settings.mode = modes[next].to_string();
                        ui.memory_mut(|memory| memory.open_popup(combo_id.with("popup")));
                    }
                    if activate && !combo_response.response.clicked() {
                        ui.memory_mut(|memory| memory.toggle_popup(combo_id.with("popup")));
                    }
                }
                if self.needs_admin() {
                    if is_process_elevated() {
                        ui.colored_label(GOOD, "管理员运行");
                    } else {
                        if uac_badge_button(ui).clicked() {
                            self.relaunch_as_administrator();
                        }
                        ui.colored_label(WARN, "客户端 TUN 需要管理员权限");
                    }
                } else {
                    ui.colored_label(GOOD, "当前模式通常无需管理员权限");
                }
            });
            ui.label(
                RichText::new(match normalized_launch_mode(&self.settings.mode) {
                    "proxy" => {
                        "Proxy 仅加载服务器和传输配置，不创建 TUN，也不监听 HTTP/SOCKS 端口。"
                    }
                    "server" => "服务端模式监听配置中的服务端入口，不接管本机 TUN。",
                    _ => "客户端模式连接选中的服务器；启用 TUN 后会修改网卡、DNS 和路由。",
                })
                .small()
                .color(MUTED),
            );
            labeled_cli_text(
                ui,
                "配置文件",
                "--config",
                "核心使用的主配置文件",
                &mut self.settings.config_path,
                "./config/HKBN.json",
            );
            labeled_cli_text(
                ui,
                "服务器目录",
                "--server-dir",
                "读取服务器 JSON 的目录",
                &mut self.settings.server_dir,
                "./config",
            );
            ui.horizontal(|ui| {
                toggle_cli_option(
                    ui,
                    &mut self.settings.rt,
                    "实时模式",
                    "--rt",
                    "控制核心实时调度模式",
                );
                ui.label(
                    RichText::new("默认开启；关闭可能降低 CPU 占用，但会改变调度时序")
                        .small()
                        .color(MUTED),
                );
            });
            labeled_cli_text(
                ui,
                "DNS 服务器",
                "--dns",
                "核心使用的 DNS 地址列表",
                &mut self.settings.dns,
                "例如 1.1.1.1,8.8.8.8；留空使用核心默认值",
            );
            labeled_cli_text(
                ui,
                "核心自动重启",
                "--auto-restart",
                "核心运行指定秒数后重启；0/留空关闭",
                &mut self.settings.auto_restart,
                "例如 3600",
            );
            if normalized_launch_mode(&self.settings.mode) == "server" {
                labeled_cli_text(
                    ui,
                    "防火墙规则",
                    "--firewall-rules",
                    "服务端防火墙规则文件",
                    &mut self.settings.firewall_rules,
                    "例如 ./firewall-rules.txt；留空使用核心默认值",
                );
            }
            ui.label(
                RichText::new("TUN VPN 开启时才使用下面的 TUN 参数；关闭 TUN 后核心以无监听 proxy 控制模式启动。")
                    .small()
                    .color(MUTED),
            );
            labeled_cli_text(
                ui,
                "TUN IP",
                "--tun-ip",
                "虚拟网卡分配给本机的 IPv4 地址",
                &mut self.settings.tun_ip,
                "留空使用核心默认值，例如 192.168.12.32",
            );
            labeled_cli_text(
                ui,
                "TUN 网关",
                "--tun-gw",
                "虚拟网卡的网关地址，数据包从这里进入 TUN",
                &mut self.settings.tun_gw,
                "留空使用核心默认值，例如 192.168.12.1",
            );
            labeled_cli_text(
                ui,
                "TUN 掩码",
                "--tun-mask",
                "虚拟网卡 IPv4 网段前缀长度",
                &mut self.settings.tun_mask,
                "例如 24",
            );
            labeled_cli_text(
                ui,
                "物理网卡",
                "--nic",
                "指定承载直连/绕过流量的物理网卡",
                &mut self.settings.nic,
                "留空自动选择，例如 Ethernet 或 eth0",
            );
            labeled_cli_text(
                ui,
                "物理网关",
                "--ngw",
                "强制指定物理网络默认网关",
                &mut self.settings.ngw,
                "留空自动探测，例如 192.168.1.1",
            );
            labeled_cli_text(
                ui,
                "TUN 网卡名称",
                "--tun",
                "虚拟网卡/适配器名称",
                &mut self.settings.tun,
                "留空使用平台默认值",
            );
            if cfg!(target_os = "windows") {
                labeled_cli_text(
                    ui,
                    "TUN 驱动",
                    "--tun-driver",
                    "Windows 虚拟网卡驱动模式",
                    &mut self.settings.tun_driver,
                    "auto、wintun 或 tap",
                );
            }
            ui.horizontal(|ui| {
                ui.label(RichText::new("TCP/IP CC").color(MUTED));
                egui::ComboBox::from_id_salt("tcp-ip-cc")
                    .selected_text(
                        match normalize_tcp_ip_cc(&self.settings.tcp_ip_cc).as_str() {
                            "lwip" => "lwIP",
                            "ctcp" => "ctcp",
                            _ => "auto",
                        },
                    )
                    .show_ui(ui, |ui| {
                        ui.selectable_value(
                            &mut self.settings.tcp_ip_cc,
                            "auto".to_string(),
                            "auto（按平台/驱动）",
                        );
                        ui.selectable_value(
                            &mut self.settings.tcp_ip_cc,
                            "lwip".to_string(),
                            "lwIP（内置协议栈）",
                        );
                        ui.selectable_value(
                            &mut self.settings.tcp_ip_cc,
                            "ctcp".to_string(),
                            "ctcp（非 lwIP 路径）",
                        );
                    });
                self.settings.tcp_ip_cc = normalize_tcp_ip_cc(&self.settings.tcp_ip_cc);
            });
            ui.label(
                RichText::new(
                    "auto：Windows 的 Wintun 默认 ctcp、TAP 默认 lwIP；Linux/macOS 默认 ctcp。仅客户端 TUN 模式生效。",
                )
                .small()
                .color(MUTED),
            );
            labeled_cli_text(
                ui,
                "MUX 通道",
                "--tun-mux",
                "复用的并行传输通道数量",
                &mut self.settings.tun_mux,
                "例如 0",
            );
            labeled_cli_text(
                ui,
                "MUX 加速",
                "--tun-mux-acceleration",
                "MUX 数据处理加速等级",
                &mut self.settings.tun_mux_acceleration,
                "例如 0",
            );
            labeled_cli_text(
                ui,
                "链路重连",
                "--link-restart",
                "链路失败后的自动重连次数",
                &mut self.settings.link_restart,
                "例如 3",
            );
            egui::Grid::new("startup-toggle-grid")
                .num_columns(2)
                .spacing([18.0, 8.0])
                .show(ui, |ui| {
                    toggle_cli_option(
                        ui,
                        &mut self.settings.tun_host,
                        "TUN Host",
                        "--tun-host",
                        "允许核心接管主机网络",
                    );
                    toggle_cli_option(
                        ui,
                        &mut self.settings.tun_vnet,
                        "TUN VNet",
                        "--tun-vnet",
                        "启用虚拟网卡数据面",
                    );
                    ui.end_row();
                    toggle_cli_option(
                        ui,
                        &mut self.settings.tun_static,
                        "TUN Static",
                        "--tun-static",
                        "使用静态 TUN 地址配置",
                    );
                    toggle_cli_option(
                        ui,
                        &mut self.settings.tun_flash,
                        "TUN Flash",
                        "--tun-flash",
                        "快速刷新 TUN 网络状态",
                    );
                    ui.end_row();
                    toggle_cli_option(
                        ui,
                        &mut self.settings.block_quic,
                        "Block QUIC",
                        "--block-quic",
                        "阻止 QUIC，改走 TCP 分流",
                    );
                    ui.end_row();
                });
            if cfg!(any(target_os = "linux", target_os = "macos")) {
                labeled_cli_text(
                    ui,
                    "TUN SSMT",
                    "--tun-ssmt",
                    "TUN 传输线程/模式参数",
                    &mut self.settings.tun_ssmt,
                    "Linux 例如 4/mq；macOS 例如 4；留空使用默认值",
                );
            }
            if cfg!(target_os = "windows") {
                labeled_cli_text(
                    ui,
                    "DHCP 租约时间",
                    "--tun-lease-time-in-seconds",
                    "Windows TUN DHCP 租约秒数",
                    &mut self.settings.tun_lease_time,
                    "例如 7200；留空使用核心默认值",
                );
            }
            egui::Grid::new("startup-platform-toggle-grid")
                .num_columns(2)
                .spacing([18.0, 8.0])
                .show(ui, |ui| {
                    if cfg!(any(target_os = "linux", target_os = "macos")) {
                        toggle_cli_option(
                            ui,
                            &mut self.settings.tun_promisc,
                            "TUN Promisc",
                            "--tun-promisc",
                            "虚拟以太网混杂模式",
                        );
                    }
                    if cfg!(target_os = "linux") {
                        toggle_cli_option(
                            ui,
                            &mut self.settings.tun_route,
                            "TUN Route",
                            "--tun-route",
                            "启用 Linux 路由兼容模式",
                        );
                    }
                    ui.end_row();
                    if cfg!(target_os = "linux") {
                        toggle_cli_option(
                            ui,
                            &mut self.settings.tun_protect,
                            "TUN Protect",
                            "--tun-protect",
                            "保护物理网络，避免 VPN 路由回环",
                        );
                    }
                    ui.end_row();
                });
            labeled_cli_text(
                ui,
                "HTTP 代理端口",
                "--proxy-http-port",
                "本地 HTTP 代理监听端口；留空跟随当前配置/Main，0 表示关闭",
                &mut self.settings.proxy_http_port,
                &http_proxy_hint,
            );
            labeled_cli_text(
                ui,
                "SOCKS 端口",
                "--proxy-socks-port",
                "本地 SOCKS 代理监听端口；留空跟随当前配置/Main，0 表示关闭",
                &mut self.settings.proxy_socks_port,
                &socks_proxy_hint,
            );
            ui.add_space(8.0);
            ui.label(RichText::new("分流出口接口").strong());
            ui.label(
                RichText::new("用于 IP 分流；网关参数也可用于 Geo/无分流以外的底层路由场景。")
                    .small()
                    .color(MUTED),
            );
            if cfg!(target_os = "linux") {
                labeled_cli_text(
                    ui,
                    "绕过 IPv4 网卡",
                    "--bypass-nic",
                    "Linux 上 IP 分流列表使用的物理网卡",
                    &mut self.settings.bypass_nic,
                    "留空自动选择",
                );
            }
            labeled_cli_text(
                ui,
                "绕过 IPv4 网关",
                "--bypass-ngw",
                "IPv4 分流下一跳网关",
                &mut self.settings.bypass_ngw,
                "留空自动探测，例如 192.168.1.1",
            );
            if cfg!(target_os = "linux") {
                labeled_cli_text(
                    ui,
                    "绕过 IPv6 网卡",
                    "--bypass-nic6",
                    "Linux 上 IPv6 分流列表使用的物理网卡",
                    &mut self.settings.bypass_nic6,
                    "留空自动选择",
                );
            }
            labeled_cli_text(
                ui,
                "绕过 IPv6 网关",
                "--bypass-ngw6",
                "IPv6 分流下一跳网关",
                &mut self.settings.bypass_ngw6,
                "留空使用核心默认值，例如 fe80::1",
            );
            ui.add_space(10.0);
            ui.label(RichText::new("高级启动参数").strong());
            ui.label(
                RichText::new(
                    "可粘贴 README 中的完整参数，然后点击“导入到上方设置”；程序名和 TUI 内部 RPC 参数会自动移除。",
                )
                .small()
                .color(MUTED),
            );
            let command_response = ui.add(
                TextEdit::multiline(&mut self.settings.command)
                    .desired_rows(6)
                    .desired_width(f32::INFINITY)
                    .hint_text("例如：--mode=client --config=./config/HKBN.json --tun-mux=0"),
            );
            scroll_focused_control(ui, &command_response);
            ui.add_space(8.0);
            if ui.button("从高级命令导入到上方设置").clicked() {
                self.import_command_fields();
            }
            ui.add_space(8.0);
            let preview = format_command_preview(&self.prepared_core_args());
            ui.label(RichText::new("启动命令预览").strong());
            egui::Frame::group(ui.style()).show(ui, |ui| {
                egui::ScrollArea::vertical()
                    .id_salt("startup-command-preview-scroll")
                    .hscroll(true)
                    .min_scrolled_height(126.0)
                    .max_height(156.0)
                    .auto_shrink([false, false])
                    .scroll_bar_visibility(egui::scroll_area::ScrollBarVisibility::AlwaysVisible)
                    .show(ui, |ui| {
                        for line in preview.lines() {
                            ui.add(
                                egui::Label::new(RichText::new(line).family(FontFamily::Monospace))
                                    .wrap_mode(TextWrapMode::Extend)
                                    .selectable(true),
                            );
                        }
                    });
            });
            ui.add_space(8.0);
            ui.horizontal(|ui| {
                let launch_label = if self.needs_admin() {
                    "启动核心（需要 UAC）"
                } else {
                    "启动核心"
                };
                if ui
                    .add_enabled(
                        !self.launching && !self.has_owned_core(),
                        egui::Button::new(launch_label),
                    )
                    .clicked()
                {
                    self.start_embedded();
                }
                if self.launching {
                    ui.spinner();
                    ui.label("正在等待 RPC 端点…");
                }
            });
        });
        card(ui, "连接已有核心", |ui| {
            labeled_text(
                ui,
                "RPC 地址",
                &mut self.settings.rpc_address,
                "例如 127.0.0.1:39100",
            );
            labeled_text(
                ui,
                "RPC Token",
                &mut self.settings.rpc_token,
                "核心启动时的 --rpc-token",
            );
            if ui.button("连接已有核心").clicked() {
                self.attach_existing();
            }
        });
    }

    fn empty_state(&mut self, ui: &mut egui::Ui, title: &str, detail: &str) {
        ui.vertical_centered(|ui| {
            ui.add_space(80.0);
            ui.heading(title);
            ui.label(RichText::new(detail).color(MUTED));
            if ui.button("打开启动设置").clicked() {
                self.view = View::Settings;
            }
        });
    }
}

impl App for DesktopApp {
    fn update(&mut self, ctx: &egui::Context, _frame: &mut Frame) {
        // The settings page edits this field in place. Refresh the shared
        // logger before any per-frame diagnostics so clearing the path takes
        // effect without requiring a restart.
        configure_tui_logging(&self.settings);
        self.frame_counter = self.frame_counter.saturating_add(1);
        let frame = self.frame_counter;
        let verbose = self.verbose_frame_log();
        if verbose {
            boot_log(&format!(
                "frame={frame} begin view={:?} first_frame_rendered={} catalog_core={} launcher={} rpc={} launching={} local_servers={} snapshot={}",
                self.view,
                self.first_frame_rendered,
                self.catalog_core,
                self.has_owned_core(),
                self.rpc.is_some(),
                self.launching,
                self.local_servers.len(),
                self.snapshot.is_some(),
            ));
        }
        // Let eframe finish and show the first native frame before any core
        // or RPC work.  Subsequent RPC connects are asynchronous as well.
        let first_frame = !self.first_frame_rendered;
        if !first_frame {
            if self.startup_core_pending {
                self.startup_core_pending = false;
                if self.startup_direct {
                    boot_log("startup: launching requested direct core after first frame");
                    self.start_embedded_to(View::Overview);
                } else {
                    boot_log("startup: launching catalog/probe control plane after first frame");
                    self.start_catalog_core();
                }
            }
            self.poll_core(ctx);
        }
        if verbose {
            boot_log(&format!("frame={frame} stage=before_keyboard_shortcuts"));
        }
        self.handle_keyboard_shortcuts(ctx);
        if verbose {
            boot_log(&format!("frame={frame} stage=after_keyboard_shortcuts"));
        }
        if verbose {
            boot_log(&format!("frame={frame} stage=before_top_bar"));
        }
        egui::TopBottomPanel::top("top_bar")
            .frame(egui::Frame::side_top_panel(ctx.style().as_ref()).inner_margin(12.0))
            .show(ctx, |ui| self.top_bar(ui));
        if verbose {
            boot_log(&format!("frame={frame} stage=after_top_bar"));
        }
        if verbose {
            boot_log(&format!("frame={frame} stage=before_navigation"));
        }
        egui::SidePanel::left("navigation")
            .resizable(false)
            .exact_width(180.0)
            .frame(egui::Frame::side_top_panel(ctx.style().as_ref()).inner_margin(14.0))
            .show(ctx, |ui| self.navigation(ui));
        if verbose {
            boot_log(&format!("frame={frame} stage=after_navigation"));
        }
        if verbose {
            boot_log(&format!("frame={frame} stage=before_content"));
        }
        egui::CentralPanel::default()
            .frame(egui::Frame::central_panel(ctx.style().as_ref()).inner_margin(22.0))
            .show(ctx, |ui| {
                if first_frame {
                    // Keep the first native frame deliberately minimal.  The
                    // full page contains scroll areas, cards and graphs; on
                    // some Windows WGPU paths drawing all of that while the
                    // native window is being created can terminate without a
                    // Rust panic or eframe error.  Render the real page on
                    // the next frame after the window is visible.
                    ui.heading("PPP PRIVATE NETWORK™ 2");
                    ui.add_space(12.0);
                    ui.label("正在打开界面…");
                } else {
                    self.content(ui);
                }
            });
        if verbose {
            boot_log(&format!("frame={frame} stage=after_content"));
        }
        if first_frame {
            self.first_frame_rendered = true;
            boot_log(&format!(
                "frame={frame} first_frame_rendered; sending viewport visible/focus"
            ));
            ctx.send_viewport_cmd(egui::ViewportCommand::Visible(true));
            ctx.send_viewport_cmd(egui::ViewportCommand::Focus);
            ctx.request_repaint();
        }
        if verbose {
            boot_log(&format!("frame={frame} done"));
        }
    }

    fn on_exit(&mut self, _gl: Option<&eframe::glow::Context>) {
        let _ = self.settings.save();
        self.stop_core(true);
    }
}

fn install_cjk_font(ctx: &egui::Context) {
    let mut fonts = egui::FontDefinitions::default();
    fonts.font_data.insert(
        "ppp-cjk".to_owned(),
        egui::FontData::from_static(include_bytes!(env!("PPP_TUI_CJK_FONT_PATH"))),
    );
    for family in [egui::FontFamily::Proportional, egui::FontFamily::Monospace] {
        if let Some(fonts_for_family) = fonts.families.get_mut(&family) {
            fonts_for_family.push("ppp-cjk".to_owned());
        }
    }
    ctx.set_fonts(fonts);
}

fn install_tui_style(ctx: &egui::Context) {
    ctx.style_mut(|style| {
        let background = Color32::from_rgb(15, 16, 18);
        let panel = Color32::from_rgb(19, 21, 24);
        let field = Color32::from_rgb(10, 11, 13);
        let border = Color32::from_rgb(86, 94, 106);
        let hover = Color32::from_rgb(38, 45, 55);

        style.spacing.item_spacing = Vec2::new(8.0, 6.0);
        style.spacing.button_padding = Vec2::new(10.0, 6.0);
        style.visuals.panel_fill = background;
        style.visuals.window_fill = panel;
        style.visuals.window_stroke = Stroke::new(1.0_f32, border);
        style.visuals.window_rounding = Rounding::same(0.0);
        style.visuals.menu_rounding = Rounding::same(0.0);
        style.visuals.faint_bg_color = panel;
        style.visuals.extreme_bg_color = field;
        style.visuals.code_bg_color = field;
        style.visuals.button_frame = true;
        style.visuals.striped = true;

        style.visuals.widgets.noninteractive.rounding = Rounding::same(0.0);
        style.visuals.widgets.noninteractive.bg_stroke = Stroke::new(1.0_f32, border);
        style.visuals.widgets.noninteractive.weak_bg_fill = panel;
        style.visuals.widgets.inactive.rounding = Rounding::same(0.0);
        style.visuals.widgets.inactive.bg_fill = panel;
        style.visuals.widgets.inactive.weak_bg_fill = panel;
        style.visuals.widgets.inactive.bg_stroke = Stroke::new(1.0_f32, border);
        style.visuals.widgets.hovered.rounding = Rounding::same(0.0);
        style.visuals.widgets.hovered.bg_fill = hover;
        style.visuals.widgets.hovered.weak_bg_fill = hover;
        style.visuals.widgets.hovered.bg_stroke = Stroke::new(1.0_f32, ACCENT);
        style.visuals.widgets.active.rounding = Rounding::same(0.0);
        style.visuals.widgets.active.bg_fill = Color32::from_rgb(33, 58, 84);
        style.visuals.widgets.active.weak_bg_fill = Color32::from_rgb(33, 58, 84);
        style.visuals.widgets.active.bg_stroke = Stroke::new(1.0_f32, ACCENT);
        style.visuals.widgets.open.rounding = Rounding::same(0.0);

        for (text_style, size) in [
            (TextStyle::Body, 15.0),
            (TextStyle::Button, 15.0),
            (TextStyle::Small, 13.0),
            (TextStyle::Heading, 20.0),
            (TextStyle::Monospace, 15.0),
        ] {
            style
                .text_styles
                .insert(text_style, FontId::new(size, FontFamily::Monospace));
        }
    });
}

fn card(ui: &mut egui::Ui, title: &str, content: impl FnOnce(&mut egui::Ui)) {
    tui_top_rule(ui, title);
    ui.add_space(6.0);
    content(ui);
    ui.add_space(6.0);
    tui_bottom_rule(ui);
    ui.add_space(8.0);
}

fn wide_info_section(ui: &mut egui::Ui, title: &str, content: impl FnOnce(&mut egui::Ui)) {
    tui_top_rule(ui, title);
    ui.add_space(6.0);
    content(ui);
    ui.add_space(6.0);
    tui_bottom_rule(ui);
}

fn tui_top_rule(ui: &mut egui::Ui, title: &str) {
    ui.label(
        RichText::new(title)
            .strong()
            .color(ACCENT)
            .family(FontFamily::Monospace),
    );
    tui_plain_rule(ui);
}

fn tui_bottom_rule(ui: &mut egui::Ui) {
    tui_plain_rule(ui);
}

fn tui_plain_rule(ui: &mut egui::Ui) {
    let width = ui.available_width().max(0.0);
    let count = ((width / 9.0).floor() as usize).max(1);
    ui.add_sized(
        [width, 20.0],
        egui::Label::new(
            RichText::new("-".repeat(count))
                .color(MUTED)
                .family(FontFamily::Monospace),
        ),
    );
}

fn metric_card(ui: &mut egui::Ui, title: &str, value: String, color: Color32) {
    egui::Frame::group(ui.style()).show(ui, |ui| {
        ui.set_min_width(ui.available_width());
        ui.label(RichText::new(title).small().color(MUTED));
        ui.label(RichText::new(value).size(19.0).color(color).strong());
    });
}

fn info_grid(ui: &mut egui::Ui, rows: &[(&str, &str)]) {
    egui::Grid::new(ui.next_auto_id())
        .num_columns(2)
        .striped(true)
        .spacing([24.0, 6.0])
        .show(ui, |ui| {
            for (key, value) in rows {
                ui.add_sized(
                    [160.0, 20.0],
                    egui::Label::new(RichText::new(*key).color(MUTED)),
                );
                ui.label(*value);
                ui.end_row();
            }
        });
}

fn info_grid_owned(ui: &mut egui::Ui, rows: Vec<(String, String)>) {
    egui::Grid::new(ui.next_auto_id())
        .num_columns(2)
        .striped(true)
        .spacing([18.0, 5.0])
        .show(ui, |ui| {
            for (key, value) in rows {
                ui.add_sized(
                    [160.0, 20.0],
                    egui::Label::new(RichText::new(key).color(MUTED)),
                );
                ui.label(value);
                ui.end_row();
            }
        });
}

fn draw_traffic_graphs(ui: &mut egui::Ui, history: &TrafficHistory) {
    traffic_graph(ui, "▼ rx", history, true, GOOD);
    ui.add_space(8.0);
    traffic_graph(ui, "▲ tx", history, false, ACCENT);
}

fn traffic_graph(
    ui: &mut egui::Ui,
    title: &str,
    history: &TrafficHistory,
    rx: bool,
    color: Color32,
) {
    let background = Color32::from_rgb(9, 10, 12);
    let width = (ui.available_width() - 2.0).max(1.0);
    let (rect, _) = ui.allocate_exact_size(Vec2::new(width, 148.0), Sense::hover());
    let painter = ui.painter_at(rect);
    painter.rect_filled(rect, Rounding::same(0.0), background);
    // Draw the border inside the allocated rectangle. Using a framed child
    // with the full available width could push the last pixel outside the
    // scroll viewport, which made the TX card appear open on the right.
    painter.rect_stroke(
        rect.shrink(0.5),
        Rounding::same(0.0),
        Stroke::new(1.0_f32, color),
    );

    let latest = history
        .latest()
        .map(|point| {
            if rx {
                point.rx_bytes_per_sec
            } else {
                point.tx_bytes_per_sec
            }
        })
        .unwrap_or(0);
    painter.text(
        Pos2::new(rect.left() + 8.0, rect.top() + 6.0),
        Align2::LEFT_TOP,
        format!("{title} ({})", format_rate(latest)),
        FontId::monospace(14.0),
        color,
    );

    let plot = Rect::from_min_max(
        Pos2::new(rect.left() + 8.0, rect.top() + 27.0),
        Pos2::new(rect.right() - 8.0, rect.bottom() - 8.0),
    );
    let values: Vec<u64> = history
        .samples()
        .iter()
        .map(|point| {
            if rx {
                point.rx_bytes_per_sec
            } else {
                point.tx_bytes_per_sec
            }
        })
        .collect();
    let max_value = values.iter().copied().max().unwrap_or(1).max(1) as f32;
    if values.is_empty() {
        painter.text(
            plot.center(),
            Align2::CENTER_CENTER,
            "waiting for traffic...",
            FontId::monospace(13.0),
            MUTED,
        );
        return;
    }

    let bar_width = (plot.width() / values.len() as f32).max(1.0);
    for (index, value) in values.iter().enumerate() {
        let height =
            (plot.height() * (*value as f32 / max_value)).max(if *value > 0 { 2.0 } else { 0.0 });
        let left = plot.left() + index as f32 * bar_width;
        let bar = Rect::from_min_max(
            Pos2::new(left, plot.bottom() - height),
            Pos2::new((left + bar_width - 1.0).min(plot.right()), plot.bottom()),
        );
        painter.rect_filled(bar, Rounding::same(0.0), color);
    }
}

fn labeled_text(ui: &mut egui::Ui, label: &str, value: &mut String, hint: &str) {
    let width = ui.available_width().min(720.0);
    ui.label(RichText::new(label).color(MUTED));
    let response = ui.add_sized([width, 28.0], TextEdit::singleline(value).hint_text(hint));
    scroll_focused_control(ui, &response);
    ui.add_space(6.0);
}

fn toggle_cli_option(
    ui: &mut egui::Ui,
    value: &mut bool,
    label: &str,
    cli_name: &str,
    description: &str,
) {
    ui.allocate_ui(Vec2::new(220.0, 48.0), |ui| {
        let response = ui.checkbox(value, format!("{label} {{{cli_name}}}"));
        scroll_focused_control(ui, &response);
        ui.label(RichText::new(description).small().color(MUTED));
    });
}

fn proxy_port_from_display(value: &str) -> Option<String> {
    let authority = value.split('/').next()?.trim();
    if authority.is_empty() || authority.eq_ignore_ascii_case("off") {
        return None;
    }
    authority
        .rsplit_once(':')?
        .1
        .parse::<u16>()
        .ok()
        .map(|port| port.to_string())
}

/// A small in-app shield button. It keeps the existing badge appearance while
/// allowing the user to request a Windows `runas` relaunch when client TUN
/// needs elevation.
fn uac_badge_button(ui: &mut egui::Ui) -> egui::Response {
    let (rect, response) = ui.allocate_exact_size(Vec2::new(54.0, 24.0), Sense::click());
    let painter = ui.painter_at(rect);
    if response.hovered() {
        painter.rect_filled(rect, Rounding::same(0.0), Color32::from_rgb(48, 40, 22));
    }
    let shield = vec![
        Pos2::new(rect.left() + 4.0, rect.top() + 3.0),
        Pos2::new(rect.left() + 16.0, rect.top() + 3.0),
        Pos2::new(rect.left() + 16.0, rect.center().y + 5.0),
        Pos2::new(rect.left() + 10.0, rect.bottom() - 3.0),
        Pos2::new(rect.left() + 4.0, rect.center().y + 5.0),
    ];
    painter.add(egui::Shape::convex_polygon(
        shield,
        Color32::from_rgb(91, 72, 27),
        Stroke::new(1.0_f32, WARN),
    ));
    painter.line_segment(
        [
            Pos2::new(rect.left() + 10.0, rect.top() + 5.0),
            Pos2::new(rect.left() + 10.0, rect.bottom() - 6.0),
        ],
        Stroke::new(1.0_f32, WARN),
    );
    painter.text(
        Pos2::new(rect.left() + 22.0, rect.center().y),
        Align2::LEFT_CENTER,
        "UAC",
        FontId::monospace(12.0),
        WARN,
    );
    response.on_hover_text("请求以管理员身份重新启动 TUI")
}

fn labeled_cli_text(
    ui: &mut egui::Ui,
    label: &str,
    cli_name: &str,
    description: &str,
    value: &mut String,
    hint: &str,
) {
    let width = ui.available_width().min(720.0);
    ui.label(RichText::new(format!("{label} {{{cli_name}}}  ·  {description}")).color(MUTED));
    let response = ui.add_sized([width, 28.0], TextEdit::singleline(value).hint_text(hint));
    scroll_focused_control(ui, &response);
    ui.add_space(6.0);
}

fn route_file_field(ui: &mut egui::Ui, label: &str, value: &mut String, hint: &str, base: &Path) {
    ui.horizontal(|ui| {
        ui.add_sized(
            [160.0, 24.0],
            egui::Label::new(RichText::new(label).color(MUTED)),
        );
        let width = (ui.available_width() - 4.0).max(180.0);
        let response = ui.add(
            TextEdit::singleline(value)
                .desired_width(width)
                .hint_text(hint),
        );
        scroll_focused_control(ui, &response);
    });
    let path = resolve_from_working_dir(base, value);
    let exists = path.is_file();
    let display_path = display_path_string(&path);
    let (status, color) = if exists {
        (format!("已找到 · {display_path}"), GOOD)
    } else {
        (format!("未找到 · {display_path}"), WARN)
    };
    ui.horizontal(|ui| {
        ui.add_space(160.0);
        ui.colored_label(color, status);
    });
    ui.add_space(4.0);
}

/// Keep keyboard-focused controls visible in the page's parent ScrollArea.
///
/// Startup settings contain cards, grids, and a nested command preview. The
/// explicit rectangle request makes focus traversal scroll the outer page,
/// not only the immediate child container.
fn scroll_focused_control(ui: &mut egui::Ui, response: &egui::Response) {
    if response.has_focus() {
        ui.scroll_to_rect(response.rect.expand(4.0), Some(egui::Align::Center));
        response.scroll_to_me(Some(egui::Align::Center));
    }
}

fn keyboard_scroll_request(ctx: &egui::Context, include_line_keys: bool) -> Option<KeyboardScroll> {
    // Read this before entering ctx.input. Calling another Context input
    // accessor from inside the input lock can deadlock the egui UI thread;
    // that appeared as a window stuck on the loading placeholder with no
    // Rust panic or error.
    let widget_has_focus = ctx.wants_keyboard_input();
    ctx.input(|input| {
        if !widget_has_focus && input.key_pressed(egui::Key::Home) {
            Some(KeyboardScroll::Home)
        } else if !widget_has_focus && input.key_pressed(egui::Key::End) {
            Some(KeyboardScroll::End)
        } else if input.key_pressed(egui::Key::PageUp) {
            Some(KeyboardScroll::PageUp)
        } else if input.key_pressed(egui::Key::PageDown) {
            Some(KeyboardScroll::PageDown)
        } else if !widget_has_focus && include_line_keys && input.key_pressed(egui::Key::ArrowUp) {
            Some(KeyboardScroll::LineUp)
        } else if !widget_has_focus && include_line_keys && input.key_pressed(egui::Key::ArrowDown)
        {
            Some(KeyboardScroll::LineDown)
        } else {
            None
        }
    })
}

fn apply_keyboard_scroll(ui: &mut egui::Ui, request: Option<KeyboardScroll>) {
    let Some(request) = request else {
        return;
    };
    let page = (ui.clip_rect().height() * 0.85).max(120.0);
    let delta = match request {
        // Positive Y moves the content down, revealing the beginning; a
        // negative value moves the content up, revealing later information.
        KeyboardScroll::LineUp => Vec2::new(0.0, 48.0),
        KeyboardScroll::LineDown => Vec2::new(0.0, -48.0),
        KeyboardScroll::PageUp => Vec2::new(0.0, page),
        KeyboardScroll::PageDown => Vec2::new(0.0, -page),
        KeyboardScroll::Home => Vec2::new(0.0, 1_000_000.0),
        KeyboardScroll::End => Vec2::new(0.0, -1_000_000.0),
    };
    ui.scroll_with_delta(delta);
}

fn normalize_display_path(path: &Path) -> PathBuf {
    let candidate = path.canonicalize().unwrap_or_else(|_| path.to_path_buf());
    let text = candidate.to_string_lossy();
    let text = text.strip_prefix("\\\\?\\").unwrap_or(text.as_ref());
    let candidate = PathBuf::from(text);
    let mut normalized = PathBuf::new();
    for component in candidate.components() {
        match component {
            Component::CurDir => {}
            Component::ParentDir => {
                normalized.pop();
            }
            other => normalized.push(other.as_os_str()),
        }
    }
    normalized
}

fn selectable_card_clicked(
    ui: &mut egui::Ui,
    card_rect: Rect,
    action_rect: Option<Rect>,
    id: egui::Id,
) -> bool {
    let Some(action_rect) = action_rect else {
        return ui.interact(card_rect, id, Sense::click()).clicked();
    };

    let left_width = (action_rect.left() - card_rect.left() - 4.0).max(0.0);
    let left_area = Rect::from_min_size(card_rect.min, Vec2::new(left_width, card_rect.height()));
    let mut clicked = left_width > 0.0
        && ui
            .interact(left_area, id.with("left"), Sense::click())
            .clicked();

    let lower_top = (action_rect.bottom() + 4.0).min(card_rect.bottom());
    if lower_top < card_rect.bottom() {
        let lower_area = Rect::from_min_max(Pos2::new(card_rect.left(), lower_top), card_rect.max);
        clicked |= ui
            .interact(lower_area, id.with("lower"), Sense::click())
            .clicked();
    }
    clicked
}

fn route_mode_title(mode: &str) -> &'static str {
    match mode {
        "geo" => "GEO",
        "no" => "关闭用户",
        _ => "IP",
    }
}

fn interface_card(
    ui: &mut egui::Ui,
    title: &str,
    interface: &NetworkInterface,
    tun: bool,
    snapshot: &Snapshot,
) {
    card(ui, title, |ui| {
        info_grid_owned(ui, interface_rows(interface, tun, &snapshot.network));
    });
    ui.add_space(12.0);
}

fn tunnel_rows(snapshot: &Snapshot) -> Vec<(String, String)> {
    let network = &snapshot.network;
    vec![
        ("Mode".to_string(), value_or_dash(&network.mode).to_string()),
        (
            "Adapter".to_string(),
            value_or_dash(&network.adapter).to_string(),
        ),
        (
            "Logical IPv4".to_string(),
            value_or_dash(&network.logical_ipv4).to_string(),
        ),
        (
            "Logical IPv6".to_string(),
            value_or_dash(&network.logical_ipv6).to_string(),
        ),
        (
            "Tunnel DNS".to_string(),
            value_or_dash(&network.tunnel_dns).to_string(),
        ),
        (
            "Link State".to_string(),
            value_or_dash(&network.link_state).to_string(),
        ),
        (
            "Mux State".to_string(),
            value_or_dash(&network.mux_state).to_string(),
        ),
        (
            "TCP/IP Transport".to_string(),
            value_or_dash(&network.tcp_ip_transport).to_string(),
        ),
        (
            "DNS Transport".to_string(),
            value_or_dash(&network.dns_transport).to_string(),
        ),
    ]
}

fn interface_rows(
    interface: &NetworkInterface,
    tun: bool,
    network: &Network,
) -> Vec<(String, String)> {
    let name = if interface.description.is_empty() {
        interface.name.clone()
    } else {
        format!("{}[{}]", interface.name, interface.description)
    };
    let interface_text = format!(
        "{} {} {}",
        value_or_dash(&interface.ipv4),
        value_or_dash(&interface.gateway),
        value_or_dash(&interface.subnet_mask)
    );
    let mut rows = vec![
        ("Name".to_string(), name),
        ("Index".to_string(), interface.index.to_string()),
    ];
    if !interface.id.is_empty() {
        rows.push(("Id".to_string(), interface.id.clone()));
    }
    rows.push(("Interface".to_string(), interface_text));

    if tun {
        let ipv6 = [
            if !interface.ipv6_address.is_empty() {
                format!("{}/64", interface.ipv6_address)
            } else if !interface.ipv6.is_empty() {
                interface.ipv6.clone()
            } else {
                String::new()
            },
            interface.ipv6_gateway.clone(),
            interface.ipv6_subnet_mask.clone(),
        ]
        .into_iter()
        .filter(|value| !value.is_empty())
        .collect::<Vec<_>>();
        if !ipv6.is_empty() {
            rows.push(("Interface IPv6".to_string(), ipv6.join(" ")));
        }
        rows.extend([
            (
                "Aggligator".to_string(),
                value_or_dash(&network.aggligator).to_string(),
            ),
            (
                "Proxy Interlayer".to_string(),
                value_or_dash(&network.proxy_interlayer).to_string(),
            ),
            (
                "TCP/IP CC".to_string(),
                value_or_dash(&network.tcp_ip_cc).to_string(),
            ),
            (
                "Block QUIC".to_string(),
                value_or_dash(&network.block_quic).to_string(),
            ),
            (
                "Mux State".to_string(),
                value_or_dash(&network.mux_state).to_string(),
            ),
            (
                "Link State".to_string(),
                value_or_dash(&network.link_state).to_string(),
            ),
        ]);
    } else if !interface.ipv6_gateway.is_empty() {
        rows.push(("Interface IPv6".to_string(), interface.ipv6_gateway.clone()));
    }

    for (index, dns) in interface.dns.iter().enumerate() {
        rows.push((format!("DNS Server {}", index + 1), dns.clone()));
    }
    rows
}

fn value_or_dash(value: &str) -> &str {
    if value.is_empty() {
        "-"
    } else {
        value
    }
}

fn phase_text(snapshot: &Snapshot) -> String {
    match snapshot.phase.as_str() {
        "connected" => "已连接".to_string(),
        "connecting" => "连接中".to_string(),
        "reconnecting" => "重连中".to_string(),
        "failed" => "失败".to_string(),
        other if other.is_empty() => "未启动".to_string(),
        other => other.to_string(),
    }
}

fn phase_color(snapshot: &Snapshot) -> Color32 {
    match snapshot.phase.as_str() {
        "connected" => GOOD,
        "connecting" | "reconnecting" => WARN,
        "failed" => BAD,
        _ => MUTED,
    }
}

fn active_name(snapshot: &Snapshot) -> String {
    snapshot
        .outbounds
        .iter()
        .find(|outbound| outbound.active)
        .map(|outbound| outbound.display_name.clone())
        .unwrap_or_else(|| "-".to_string())
}

fn state_color(state: i32) -> Color32 {
    match state {
        1 => GOOD,
        0 | 2 => WARN,
        _ => MUTED,
    }
}

fn usage_text(outbound: &Outbound) -> String {
    let mut usage = String::new();
    if outbound.active {
        usage.push_str(" main");
    }
    if outbound.route_used {
        usage.push_str(" split");
    }
    match outbound.state {
        1 => usage.push_str(" established"),
        0 => usage.push_str(" connecting"),
        2 => usage.push_str(" reconnecting"),
        _ => {}
    }
    if outbound.probe_checked {
        if outbound.probe_reachable && outbound.probe_rtt_ms >= 0 {
            usage.push_str(&format!(" ({}ms)", outbound.probe_rtt_ms));
        } else {
            usage.push_str(" (unreachable)");
        }
    } else if !outbound.probe_enabled {
        usage.push_str(" (probe off)");
    }
    usage
}

fn entry_text(outbound: &Outbound) -> String {
    let configured = endpoint_text(&outbound.server);
    if !outbound.multiple_entries {
        return if outbound.current_entry.is_empty() {
            configured
        } else {
            outbound.current_entry.clone()
        };
    }

    let connected = !outbound.current_entry.is_empty();
    let current = if connected {
        outbound.current_entry.clone()
    } else if !outbound.ranked_first_entry.is_empty() {
        outbound.ranked_first_entry.clone()
    } else if !outbound.probe_entry.is_empty() {
        outbound.probe_entry.clone()
    } else {
        configured
    };
    if !connected {
        return if current.is_empty() {
            "(not connected)".to_string()
        } else {
            current
        };
    }
    if outbound.ranked_first_entry.is_empty() {
        format!("{}  [ranking collecting]", current)
    } else if outbound.ranked_first_entry == outbound.current_entry {
        format!("{}  [#1]", current)
    } else {
        format!("{}  -> #1 {}", current, outbound.ranked_first_entry)
    }
}

fn endpoint_text(uri: &str) -> String {
    let trimmed = uri.trim();
    let authority_start = trimmed.find("://").map(|index| index + 3).unwrap_or(0);
    let authority = &trimmed[authority_start..];
    authority
        .find('/')
        .map(|index| authority[..index].to_string())
        .unwrap_or_else(|| authority.to_string())
}

fn endpoint_match_key(uri: &str) -> String {
    endpoint_text(uri).trim().to_ascii_lowercase()
}

fn format_duration(duration_ms: u64) -> String {
    let total_seconds = duration_ms / 1000;
    format!(
        "{:02}:{:02}:{:02}",
        total_seconds / 3600,
        (total_seconds / 60) % 60,
        total_seconds % 60
    )
}

fn split_command_line(input: &str) -> Vec<String> {
    let mut args = Vec::new();
    let mut current = String::new();
    let mut quoted = false;
    for ch in input.chars() {
        if ch == '"' {
            quoted = !quoted;
        } else if ch.is_whitespace() && !quoted {
            if !current.is_empty() {
                args.push(std::mem::take(&mut current));
            }
        } else {
            current.push(ch);
        }
    }
    if !current.is_empty() {
        args.push(current);
    }
    args
}

fn resolve_settings_path(value: &str) -> PathBuf {
    let path = PathBuf::from(value.trim());
    if path.is_absolute() {
        path
    } else {
        std::env::current_dir()
            .unwrap_or_else(|_| PathBuf::from("."))
            .join(path)
    }
}

fn normalize_user_path(value: &str) -> String {
    let value = value.trim().strip_prefix("\\\\?\\").unwrap_or(value.trim());
    value.replace('\\', "/")
}

fn normalize_path_field(value: &mut String) {
    let normalized = normalize_user_path(value);
    *value = normalized;
}

fn path_to_forward_slashes(path: &Path) -> String {
    normalize_user_path(&path.to_string_lossy())
}

fn display_path_string(path: &Path) -> String {
    path_to_forward_slashes(&normalize_display_path(path))
}

fn normalized_launch_mode(value: &str) -> &'static str {
    match value.trim().to_ascii_lowercase().as_str() {
        "proxy" | "proxy-only" | "proxy_only" => "proxy",
        "server" => "server",
        _ => "client",
    }
}

fn working_directory(settings: &StartupSettings) -> PathBuf {
    let value = settings.working_dir.trim();
    let path = if value.is_empty() {
        PathBuf::from(".")
    } else {
        PathBuf::from(value)
    };
    if path.is_absolute() {
        path
    } else {
        std::env::current_dir()
            .unwrap_or_else(|_| PathBuf::from("."))
            .join(path)
    }
}

fn resolve_from_working_dir(base: &Path, value: &str) -> PathBuf {
    let path = PathBuf::from(value.trim());
    if path.is_absolute() {
        path
    } else {
        base.join(path)
    }
}

fn core_path_string(base: &Path, path: &Path) -> String {
    if let Ok(relative) = path.strip_prefix(base) {
        let text = path_to_forward_slashes(relative);
        if text.is_empty() {
            ".".to_string()
        } else {
            format!("./{text}")
        }
    } else {
        path_to_forward_slashes(path)
    }
}

fn comparable_path(path: &Path) -> String {
    fs::canonicalize(path)
        .unwrap_or_else(|_| path.to_path_buf())
        .to_string_lossy()
        .to_ascii_lowercase()
}

fn load_server_catalog(settings: &StartupSettings) -> (Vec<LocalServerProfile>, Option<String>) {
    let working_dir = working_directory(settings);
    let directory = resolve_from_working_dir(&working_dir, &settings.server_dir);
    let read_dir = match fs::read_dir(&directory) {
        Ok(read_dir) => read_dir,
        Err(error) => {
            return (
                Vec::new(),
                Some(format!(
                    "服务器目录无法读取：{} ({error})",
                    display_path_string(&directory)
                )),
            );
        }
    };

    let mut files: Vec<PathBuf> = read_dir
        .filter_map(Result::ok)
        .map(|entry| entry.path())
        .filter(|path| {
            path.is_file()
                && path
                    .extension()
                    .and_then(|extension| extension.to_str())
                    .is_some_and(|extension| extension.eq_ignore_ascii_case("json"))
        })
        .collect();
    files.sort_by_key(|path| {
        path.file_name()
            .map(|name| name.to_string_lossy().to_ascii_lowercase())
            .unwrap_or_default()
    });

    let mut profiles = Vec::new();
    let mut errors = Vec::new();
    for path in files {
        let file_name = path
            .file_name()
            .map(|name| name.to_string_lossy().into_owned())
            .unwrap_or_else(|| display_path_string(&path));
        let contents = match fs::read_to_string(&path) {
            Ok(contents) => contents,
            Err(error) => {
                errors.push(format!("{file_name}: {error}"));
                continue;
            }
        };
        // JSON files exported by the Windows tooling may start with an UTF-8
        // BOM. serde_json intentionally does not treat that marker as JSON
        // whitespace, so remove it before parsing the server catalog.
        let contents = contents.strip_prefix('\u{feff}').unwrap_or(&contents);
        let root = match serde_json::from_str::<Value>(contents) {
            Ok(root) => root,
            Err(error) => {
                errors.push(format!("{file_name}: JSON 无法解析 ({error})"));
                continue;
            }
        };
        let Some(client) = root.get("client").and_then(Value::as_object) else {
            errors.push(format!("{file_name}: 缺少 client 配置段"));
            continue;
        };

        let mut entries = Vec::new();
        if let Some(server) = client.get("server").and_then(Value::as_str) {
            let server = server.trim();
            if !server.is_empty() {
                entries.push(server.to_string());
            }
        }
        if let Some(extra_entries) = client.get("servers").and_then(Value::as_array) {
            for entry in extra_entries.iter().filter_map(Value::as_str) {
                let entry = entry.trim();
                if !entry.is_empty() && !entries.iter().any(|item| item == entry) {
                    entries.push(entry.to_string());
                }
            }
        }
        if entries.is_empty() {
            errors.push(format!(
                "{file_name}: 没有 client.server 或 client.servers 入口"
            ));
            continue;
        }

        let name = path
            .file_stem()
            .map(|stem| stem.to_string_lossy().into_owned())
            .unwrap_or_else(|| file_name.trim_end_matches(".json").to_string());
        profiles.push(LocalServerProfile {
            name,
            path,
            server: entries[0].clone(),
            entries,
        });
    }

    let warning = errors.first().map(|first| {
        if errors.len() == 1 {
            format!("服务器目录中有配置无法读取：{first}")
        } else {
            format!("服务器目录中有 {} 个配置无法读取：{first}", errors.len())
        }
    });
    (profiles, warning)
}

fn find_selected_local_server(
    settings: &StartupSettings,
    profiles: &[LocalServerProfile],
) -> Option<usize> {
    let working_dir = working_directory(settings);
    let configured_path = resolve_from_working_dir(&working_dir, &settings.config_path);
    let configured_key = comparable_path(&configured_path);
    profiles
        .iter()
        .position(|profile| comparable_path(&profile.path) == configured_key)
}

fn command_value(args: &[String], name: &str) -> Option<String> {
    let prefix = format!("{name}=");
    let mut index = 0;
    while index < args.len() {
        if let Some(value) = args[index].strip_prefix(&prefix) {
            return Some(value.to_string());
        }
        if args[index] == name {
            if let Some(value) = args.get(index + 1) {
                if !value.starts_with("--") {
                    return Some(value.clone());
                }
            }
        }
        index += 1;
    }
    None
}

fn import_text(args: &[String], name: &str, destination: &mut String) {
    if let Some(value) = command_value(args, name) {
        *destination = value;
    }
}

fn command_bool(args: &[String], name: &str, default: bool) -> bool {
    let Some(value) = command_value(args, name) else {
        return default;
    };
    match value.trim().to_ascii_lowercase().as_str() {
        "yes" | "y" | "true" | "1" | "on" => true,
        "no" | "n" | "false" | "0" | "off" => false,
        _ => default,
    }
}

fn normalize_core_args(mut args: Vec<String>) -> Vec<String> {
    while matches!(
        args.first()
            .map(|value| value.to_ascii_lowercase())
            .as_deref(),
        Some("start") | Some("/wait") | Some("/b") | Some("/min") | Some("/max")
    ) {
        args.remove(0);
    }

    if args.len() > 1 && !is_core_executable_arg(&args[0]) && is_core_executable_arg(&args[1]) {
        // `start "window title" ppp.exe ...` leaves the title as the first
        // token after the shell keyword.
        args.remove(0);
    }
    if args
        .first()
        .is_some_and(|value| is_core_executable_arg(value))
    {
        args.remove(0);
    }
    args
}

fn is_core_executable_arg(value: &str) -> bool {
    // Command strings can be authored on Windows and then parsed by the
    // Linux/macOS client. PathBuf only treats the host platform's separator
    // as special, so recognize both separators explicitly.
    let name = value
        .rsplit(|character| character == '/' || character == '\\')
        .next()
        .unwrap_or(value);
    matches!(
        name.to_ascii_lowercase().as_str(),
        "ppp-tui.exe" | "ppp-tui" | "ppp.exe" | "ppp" | "ppp-core.exe" | "ppp-core"
    )
}

fn remove_command_argument(args: &mut Vec<String>, name: &str) {
    let prefix = format!("{name}=");
    let mut filtered = Vec::with_capacity(args.len());
    let mut index = 0;
    while index < args.len() {
        if args[index] == name {
            index += 1;
            if index < args.len() && !args[index].starts_with("--") {
                index += 1;
            }
        } else if args[index].starts_with(&prefix) {
            index += 1;
        } else {
            filtered.push(std::mem::take(&mut args[index]));
            index += 1;
        }
    }
    *args = filtered;
}

fn set_command_argument(args: &mut Vec<String>, name: &str, value: &str) {
    remove_command_argument(args, name);
    args.push(format!("{name}={value}"));
}

fn set_optional_command_argument(args: &mut Vec<String>, name: &str, value: &str) {
    remove_command_argument(args, name);
    if !value.trim().is_empty() {
        args.push(format!("{name}={value}"));
    }
}

fn set_bool_if_non_default(args: &mut Vec<String>, name: &str, value: bool, default: bool) {
    remove_command_argument(args, name);
    if value != default {
        set_command_argument(args, name, if value { "yes" } else { "no" });
    }
}

fn set_optional_if_not_default(args: &mut Vec<String>, name: &str, value: &str, default: &str) {
    remove_command_argument(args, name);
    let value = value.trim();
    if !value.is_empty() && value != default {
        args.push(format!("{name}={value}"));
    }
}

fn join_command_args(args: &[String]) -> String {
    args.iter()
        .map(|arg| format_command_arg(arg))
        .collect::<Vec<_>>()
        .join(" ")
}

fn format_command_preview(args: &[String]) -> String {
    args.iter()
        .map(|arg| format!("  {}", format_command_arg(arg)))
        .collect::<Vec<_>>()
        .join("\n")
}

fn format_command_arg(arg: &str) -> String {
    if arg.chars().any(char::is_whitespace) {
        format!("\"{}\"", arg.replace('"', "\\\""))
    } else {
        arg.to_string()
    }
}

#[cfg(windows)]
fn is_process_elevated() -> bool {
    #[link(name = "shell32")]
    unsafe extern "system" {
        fn IsUserAnAdmin() -> i32;
    }

    // SAFETY: IsUserAnAdmin has no arguments and returns a Win32 BOOL.
    unsafe { IsUserAnAdmin() != 0 }
}

#[cfg(not(windows))]
fn is_process_elevated() -> bool {
    false
}

#[cfg(windows)]
fn relaunch_elevated() -> anyhow::Result<()> {
    use std::ffi::{c_void, OsStr};
    use std::os::windows::ffi::OsStrExt;

    #[link(name = "shell32")]
    unsafe extern "system" {
        fn ShellExecuteW(
            hwnd: *mut c_void,
            operation: *const u16,
            file: *const u16,
            parameters: *const u16,
            directory: *const u16,
            show_command: i32,
        ) -> isize;
    }

    fn wide(value: &OsStr) -> Vec<u16> {
        value.encode_wide().chain(std::iter::once(0)).collect()
    }

    let executable_path = std::env::current_exe()?;
    let executable = wide(executable_path.as_os_str());
    let operation = wide(OsStr::new("runas"));
    let parameters = std::env::args_os()
        .skip(1)
        .map(|arg| {
            let value = arg.to_string_lossy().replace('"', "\\\"");
            format!("\"{value}\"")
        })
        .collect::<Vec<_>>()
        .join(" ");
    let parameters = wide(OsStr::new(&parameters));
    let directory_path = std::env::current_dir()?;
    let directory = wide(directory_path.as_os_str());

    // SAFETY: all pointers refer to NUL-terminated UTF-16 buffers that live
    // until ShellExecuteW returns.
    let result = unsafe {
        ShellExecuteW(
            std::ptr::null_mut(),
            operation.as_ptr(),
            executable.as_ptr(),
            parameters.as_ptr(),
            directory.as_ptr(),
            1,
        )
    };
    if result <= 32 {
        anyhow::bail!("ShellExecuteW(runas) returned {result}");
    }
    Ok(())
}

#[cfg(not(windows))]
fn relaunch_elevated() -> anyhow::Result<()> {
    anyhow::bail!("管理员重启按钮仅支持 Windows")
}

pub fn run(settings: StartupSettings) -> eframe::Result<()> {
    configure_tui_logging(&settings);
    boot_log("run() entered");
    // 单实例保护：同一时间只允许一个 GUI 窗口。回环 TCP 端口锁
    // （跨平台、零依赖）；重复启动时提示并退出，避免多个实例抢占
    // 核心的全局实例锁（"Repeat runs are not allowed."）。
    let mut listener = None;
    for attempt in 1..=20 {
        match std::net::TcpListener::bind(("127.0.0.1", 18991)) {
            Ok(value) => {
                listener = Some(value);
                boot_log(&format!("single-instance lock acquired attempt={attempt}"));
                break;
            }
            Err(error) => {
                boot_log(&format!(
                    "single-instance lock busy attempt={attempt}/20 error={error}"
                ));
                if attempt < 20 {
                    std::thread::sleep(Duration::from_millis(100));
                }
            }
        }
    }
    if let Some(listener) = listener {
        // 保持端口占用直到进程退出：listener 永不 drop。
        let _: &'static std::net::TcpListener = Box::leak(Box::new(listener));
    } else {
        {
            let message = "另一个 ppp-tui 窗口已在运行。\n请先关闭它，或检查任务管理器中的 ppp-tui.exe / ppp-core.exe 残留进程。";
            boot_log("single-instance lock failed after retries; treating as existing instance");
            write_diagnostic_log("[single-instance] 另一个 ppp-tui 实例已在运行，本次启动退出\n");
            #[cfg(windows)]
            {
                let script = format!(
                    "Add-Type -AssemblyName System.Windows.Forms; [System.Windows.Forms.MessageBox]::Show('{}','ppp-tui','OK','Warning')",
                    message.replace('\'', "''")
                );
                let _ = std::process::Command::new("powershell")
                    .args(["-NoProfile", "-WindowStyle", "Hidden", "-Command", &script])
                    .spawn();
            }
            #[cfg(not(windows))]
            {
                eprintln!("ppp-tui: {message}");
            }
            return Ok(());
        }
    }

    boot_log("building NativeOptions");
    let mut viewport = egui::ViewportBuilder::default()
        .with_title("PPP PRIVATE NETWORK™ 2")
        .with_inner_size([1160.0, 780.0])
        .with_min_inner_size([900.0, 620.0])
        // The Windows drag-and-drop bridge initializes OLE as STA.  Some
        // environments initialize the GUI thread as MTA first, which makes
        // winit panic with RPC_E_CHANGED_MODE while creating the window.
        // TUI does not currently consume file-drop events, so avoid that
        // optional bridge and keep startup compatible with both COM modes.
        .with_drag_and_drop(false)
        .with_visible(true);
    if let Some(icon) = bundled_window_icon() {
        viewport = viewport.with_icon(icon);
    } else {
        boot_log("failed to decode bundled window icon; using eframe default");
    }
    let options = eframe::NativeOptions {
        viewport,
        renderer: eframe::Renderer::Glow,
        ..Default::default()
    };
    boot_log("calling eframe::run_native");
    let result = eframe::run_native(
        "PPP PRIVATE NETWORK™ 2",
        options,
        Box::new(move |cc| Ok(Box::new(DesktopApp::new(cc, settings)))),
    );
    match &result {
        Ok(()) => boot_log("run_native returned successfully"),
        Err(error) => boot_log(&format!("run_native returned error: {error:#}")),
    }
    result
}

/// Configure the shared TUI log destination. The explicit boolean controls
/// whether startup diagnostics and panic reporting are persisted; an empty
/// enabled path falls back to the default file in the working directory.
pub(crate) fn configure_tui_logging(settings: &StartupSettings) {
    let path = tui_log_path_for_settings(settings);
    let cell = TUI_LOG_PATH.get_or_init(|| Mutex::new(None));
    *cell.lock().unwrap_or_else(|poison| poison.into_inner()) = path;
}

fn tui_log_path_for_settings(settings: &StartupSettings) -> Option<PathBuf> {
    if !settings.tui_log_enabled {
        return None;
    }
    let base = working_directory(settings);
    let path = if settings.tui_log_file.trim().is_empty() {
        "./ppp-tui.log"
    } else {
        settings.tui_log_file.trim()
    };
    Some(resolve_from_working_dir(&base, path))
}

fn default_tui_log_enabled() -> bool {
    true
}

/// Startup progress log uses the same configured path as runtime TUI logs.
fn boot_log(message: &str) {
    let timestamp = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|duration| duration.as_secs())
        .unwrap_or_default();
    write_diagnostic_log(&format!("[{timestamp}] {message}\n"));
}

fn configured_tui_log_path() -> Option<PathBuf> {
    TUI_LOG_PATH
        .get_or_init(|| Mutex::new(None))
        .lock()
        .unwrap_or_else(|poison| poison.into_inner())
        .clone()
}

fn bundled_window_icon() -> Option<egui::IconData> {
    const ICON_ICO: &[u8] = include_bytes!(concat!(env!("CARGO_MANIFEST_DIR"), "/../icon.ico"));
    let decoded = image::load_from_memory_with_format(ICON_ICO, image::ImageFormat::Ico)
        .ok()
        .or_else(|| {
            let embedded_png = embedded_ico_payload(ICON_ICO)?;
            image::load_from_memory_with_format(embedded_png, image::ImageFormat::Png).ok()
        })?;
    let width = decoded.width();
    let height = decoded.height();
    let rgba = decoded.to_rgba8();
    Some(egui::IconData {
        rgba: rgba.into_raw(),
        width,
        height,
    })
}

fn embedded_ico_payload(bytes: &[u8]) -> Option<&[u8]> {
    if bytes.len() < 22 || bytes.get(0..4) != Some(&[0, 0, 1, 0]) {
        return None;
    }
    let image_size = u32::from_le_bytes(bytes[14..18].try_into().ok()?) as usize;
    let image_offset = u32::from_le_bytes(bytes[18..22].try_into().ok()?) as usize;
    let image_end = image_offset.checked_add(image_size)?;
    let payload = bytes.get(image_offset..image_end)?;
    payload.starts_with(b"\x89PNG\r\n\x1a\n").then_some(payload)
}

fn append_tui_log(message: &str) {
    let Some(path) = configured_tui_log_path() else {
        return;
    };
    if let Some(parent) = path.parent() {
        let _ = fs::create_dir_all(parent);
    }
    let Ok(mut file) = fs::OpenOptions::new().create(true).append(true).open(path) else {
        return;
    };
    let _ = file.write_all(message.as_bytes());
    let _ = file.flush();
}

/// Write a startup diagnostic using the configured TUI path. Unlike the old
/// fallback search, this never creates a hidden default log when logging is
/// disabled in the settings page.
pub(crate) fn write_diagnostic_log(message: &str) {
    append_tui_log(message);
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn traditional_start_command_is_normalized_without_losing_windows_paths() {
        let raw = r#"start ppp.exe --mode=client --config=./config/rfcJP.json --server-dir=C:\openppp2\config --tun-gw=192.168.12.1"#;
        let args = normalize_core_args(split_command_line(raw));
        assert_eq!(args[0], "--mode=client");
        assert_eq!(
            command_value(&args, "--config").as_deref(),
            Some("./config/rfcJP.json")
        );
        assert_eq!(
            command_value(&args, "--server-dir").as_deref(),
            Some(r"C:\openppp2\config")
        );
    }

    #[test]
    fn command_title_form_is_supported() {
        let args = normalize_core_args(split_command_line(
            r#"start "PPP client" .\ppp.exe --mode=proxy"#,
        ));
        assert_eq!(args, vec!["--mode=proxy"]);
    }

    #[test]
    fn user_paths_are_portable_forward_slash_paths() {
        assert_eq!(
            normalize_user_path(r".\config\HKBN.json"),
            "./config/HKBN.json"
        );
        assert_eq!(
            normalize_user_path(r"\\?\C:\Users\test\ppp-core.log"),
            "C:/Users/test/ppp-core.log"
        );
    }

    #[test]
    fn launch_mode_normalization_matches_core_modes() {
        assert_eq!(normalized_launch_mode("proxy-only"), "proxy");
        assert_eq!(normalized_launch_mode("server"), "server");
        assert_eq!(normalized_launch_mode("client"), "client");
    }

    #[test]
    fn proxy_display_ports_are_read_from_runtime_values() {
        assert_eq!(
            proxy_port_from_display("192.168.68.249:8080/http"),
            Some("8080".to_string())
        );
        assert_eq!(proxy_port_from_display("off"), None);
    }

    #[test]
    fn disabled_tui_logging_ignores_configured_path() {
        let mut settings = StartupSettings::default();
        settings.tui_log_enabled = false;
        settings.tui_log_file = "custom.log".to_string();
        assert!(tui_log_path_for_settings(&settings).is_none());
    }

    #[test]
    fn enabled_tui_logging_uses_default_when_path_is_empty() {
        let mut settings = StartupSettings::default();
        settings.tui_log_file.clear();
        let path = tui_log_path_for_settings(&settings).expect("logging is enabled");
        assert_eq!(
            path.file_name().and_then(|name| name.to_str()),
            Some("ppp-tui.log")
        );
    }

    #[test]
    fn client_core_log_level_is_forwarded_after_owned_arguments_are_removed() {
        let mut args = vec!["--mode=client".to_string(), "--log-level=error".to_string()];
        remove_command_argument(&mut args, "--log-level");
        set_command_argument(&mut args, "--log-level", "debug");
        assert_eq!(
            command_value(&args, "--log-level").as_deref(),
            Some("debug")
        );
    }

    #[test]
    fn bundled_window_icon_is_decodable() {
        let icon = bundled_window_icon().expect("repository icon.ico should decode");
        assert_eq!(icon.width, icon.height);
        assert!(icon.width >= 16);
        assert_eq!(icon.rgba.len(), (icon.width * icon.height * 4) as usize);
    }
}
