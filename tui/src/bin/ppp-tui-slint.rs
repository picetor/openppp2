#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use std::cell::RefCell;
use std::net::{TcpListener, TcpStream};
use std::rc::Rc;
use std::sync::mpsc::{channel, Receiver, TryRecvError};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use anyhow::{Context, Result};
use ppp_tui::core::command::{remove_command_argument, set_command_argument};
use ppp_tui::core::probe::{spawn_probe_loop, ProbeState, ProbeTable};
use ppp_tui::core::server_catalog::{
    find_selected_local_server, load_server_catalog, LocalServerProfile,
};
use ppp_tui::core::settings::{
    display_path_string, normalized_launch_mode, relative_path_string, resolve_settings_path,
    validate_core_log_path, working_directory, StartupSettings,
};
use ppp_tui::core::traffic::{format_bytes, format_rate, TrafficHistory};
use ppp_tui::rpc::schema::{Network, NetworkInterface, Outbound, Snapshot};
use ppp_tui::rpc::{CoreClient, CoreCommand, Response, RpcClient};
use ppp_tui::terminal::prepared_core_args;
use slint::{ComponentHandle, ModelRc, VecModel};

slint::include_modules!();

fn info_model(rows: Vec<(&str, String)>) -> ModelRc<InfoItem> {
    ModelRc::from(Rc::new(VecModel::from(
        rows.into_iter()
            .filter(|(_, value)| !value.trim().is_empty())
            .map(|(label, value)| InfoItem {
                label: label.into(),
                value: value.into(),
            })
            .collect::<Vec<_>>(),
    )))
}

fn int_model(values: Vec<i32>) -> ModelRc<i32> {
    ModelRc::from(Rc::new(VecModel::from(values)))
}

fn interface_model(
    interface: Option<&NetworkInterface>,
    network: &Network,
    mtu: Option<i32>,
) -> ModelRc<InfoItem> {
    let Some(interface) = interface else {
        return info_model(Vec::new());
    };
    let mut rows = vec![
        ("名称", interface.name.clone()),
        ("描述", interface.description.clone()),
        ("索引", interface.index.to_string()),
        ("ID", interface.id.clone()),
        ("IPv4", interface.ipv4.clone()),
        ("IPv4 网关", interface.gateway.clone()),
        ("IPv4 掩码", interface.subnet_mask.clone()),
        ("IPv6", interface.ipv6.clone()),
        ("IPv6 地址", interface.ipv6_address.clone()),
        ("IPv6 网关", interface.ipv6_gateway.clone()),
        ("IPv6 掩码", interface.ipv6_subnet_mask.clone()),
        ("DNS", interface.dns.join(" / ")),
    ];
    if let Some(mtu) = mtu {
        rows.push(("MTU", mtu.to_string()));
    }
    if mtu.is_some() {
        rows.extend([
            ("Aggligator", network.aggligator.clone()),
            ("Proxy Interlayer", network.proxy_interlayer.clone()),
            ("TCP/IP 栈", network.tcp_ip_cc.clone()),
            ("UDP/443", network.block_quic.clone()),
            ("MUX", network.mux_state.clone()),
            ("链路", network.link_state.clone()),
        ]);
    }
    info_model(rows)
}

fn outbound_entry(outbound: &Outbound) -> String {
    let current = if !outbound.current_entry.is_empty() {
        outbound.current_entry.as_str()
    } else if !outbound.ranked_first_entry.is_empty() {
        outbound.ranked_first_entry.as_str()
    } else if !outbound.probe_entry.is_empty() {
        outbound.probe_entry.as_str()
    } else {
        outbound.server.as_str()
    };
    if !outbound.multiple_entries || outbound.current_entry.is_empty() {
        return current.to_string();
    }
    if outbound.ranked_first_entry.is_empty() {
        format!("{} · 排名采集中", current)
    } else if outbound.ranked_first_entry == outbound.current_entry {
        format!("{} · #1", current)
    } else {
        format!("{} → #1 {}", current, outbound.ranked_first_entry)
    }
}

struct Backend {
    settings: StartupSettings,
    profiles: Vec<LocalServerProfile>,
    selected_profile: Option<usize>,
    probes: Arc<Mutex<ProbeTable>>,
    server_rows: Vec<(String, String, String)>,
    server_rows_initialized: bool,
    core_port_guard: Option<TcpListener>,
    core: Option<CoreClient>,
    launch_rx: Option<Receiver<Result<CoreClient, String>>>,
    stop_after_launch: bool,
    connect_rx: Option<Receiver<Result<TcpStream, String>>>,
    last_connect: Instant,
    snapshot: Option<Snapshot>,
    traffic: TrafficHistory,
    last_snapshot_request: Instant,
    restart_attempts: u32,
    status: String,
    error: Option<String>,
}

impl Backend {
    fn new(mut settings: StartupSettings) -> Self {
        // Keep the settings file stable if the working directory is changed.
        settings.settings_file = resolve_settings_path(&settings.settings_file)
            .to_string_lossy()
            .into_owned();
        let (profiles, warning) = load_server_catalog(&settings);
        let selected_profile = find_selected_local_server(&settings, &profiles);
        let probes = Arc::new(Mutex::new(ProbeTable::default()));
        let backend = Self {
            settings,
            profiles,
            selected_profile,
            probes,
            server_rows: Vec::new(),
            server_rows_initialized: false,
            core_port_guard: None,
            core: None,
            launch_rx: None,
            stop_after_launch: false,
            connect_rx: None,
            last_connect: Instant::now() - Duration::from_secs(5),
            snapshot: None,
            traffic: TrafficHistory::new(),
            last_snapshot_request: Instant::now() - Duration::from_secs(5),
            restart_attempts: 0,
            status: "未启动".to_string(),
            error: warning,
        };
        backend.start_probes();
        backend
    }

    fn start_probes(&self) {
        let targets = self
            .profiles
            .iter()
            .map(|profile| (profile.name.clone(), profile.server.clone()))
            .collect();
        spawn_probe_loop(Arc::new(targets), Arc::clone(&self.probes));
    }

    fn init_ui(&mut self, ui: &OpenPPP2Design) {
        ui.set_preview_mode(false);
        ui.set_admin_label(
            if is_process_elevated() {
                "管理员"
            } else if normalized_launch_mode(&self.settings.mode) == "client"
                && self.settings.tun_enabled
            {
                "需要 UAC"
            } else {
                ""
            }
            .into(),
        );
        ui.set_core_running(false);
        ui.set_core_launching(false);
        ui.set_status_label(self.status.clone().into());
        ui.set_notice(
            self.error
                .clone()
                .unwrap_or_else(|| "请选择服务器并启动核心".to_string())
                .into(),
        );
        ui.set_working_dir(self.settings.working_dir.clone().into());
        ui.set_settings_file(self.settings.settings_file.clone().into());
        ui.set_mode_index(match normalized_launch_mode(&self.settings.mode) {
            "server" => 2,
            "proxy" => 1,
            _ => 0,
        });
        ui.set_tun_enabled(self.settings.tun_enabled);
        ui.set_config_path(self.settings.config_path.clone().into());
        ui.set_server_dir(self.settings.server_dir.clone().into());
        ui.set_core_log_file(self.settings.log_file.clone().into());
        ui.set_tui_log_file(self.settings.tui_log_file.clone().into());
        ui.set_tui_log_enabled(self.settings.tui_log_enabled);
        ui.set_log_level(self.settings.log_level.clone().into());
        ui.set_tun_ip(self.settings.tun_ip.clone().into());
        ui.set_tun_gw(self.settings.tun_gw.clone().into());
        ui.set_tun_mask(self.settings.tun_mask.clone().into());
        ui.set_nic(self.settings.nic.clone().into());
        ui.set_ngw(self.settings.ngw.clone().into());
        ui.set_tun_name(self.settings.tun.clone().into());
        ui.set_tun_driver(self.settings.tun_driver.clone().into());
        ui.set_tcp_ip_cc(self.settings.tcp_ip_cc.clone().into());
        ui.set_tun_mux(self.settings.tun_mux.clone().into());
        ui.set_tun_mux_acceleration(self.settings.tun_mux_acceleration.clone().into());
        ui.set_link_restart(self.settings.link_restart.clone().into());
        ui.set_tun_lease_time(self.settings.tun_lease_time.clone().into());
        ui.set_tun_ssmt(self.settings.tun_ssmt.clone().into());
        ui.set_dns(self.settings.dns.clone().into());
        ui.set_auto_restart(self.settings.auto_restart.clone().into());
        ui.set_firewall_rules(self.settings.firewall_rules.clone().into());
        ui.set_bypass_ngw(self.settings.bypass_ngw.clone().into());
        ui.set_bypass_ngw6(self.settings.bypass_ngw6.clone().into());
        ui.set_bypass_nic(self.settings.bypass_nic.clone().into());
        ui.set_bypass_nic6(self.settings.bypass_nic6.clone().into());
        ui.set_proxy_http_port(self.settings.proxy_http_port.clone().into());
        ui.set_proxy_socks_port(self.settings.proxy_socks_port.clone().into());
        ui.set_rpc_listen(self.settings.rpc_listen.clone().into());
        ui.set_runtime_api(if self.settings.rpc_listen.trim().is_empty() {
            "运行端点：关闭".into()
        } else {
            format!("运行端点：{}", self.settings.rpc_listen).into()
        });
        ui.set_rpc_address(self.settings.rpc_address.clone().into());
        ui.set_rpc_token(self.settings.rpc_token.clone().into());
        ui.set_advanced_command(self.settings.command.clone().into());
        ui.set_geo_rules_file(self.settings.geo_rules_file.clone().into());
        ui.set_geosite_file(self.settings.geosite_file.clone().into());
        ui.set_geoip_file(self.settings.geoip_file.clone().into());
        ui.set_bypass_file(self.settings.bypass_file.clone().into());
        ui.set_bypass6_file(self.settings.bypass6_file.clone().into());
        ui.set_dns_rules_file(self.settings.dns_rules_file.clone().into());
        ui.set_tun_static(self.settings.tun_static);
        ui.set_tun_host(self.settings.tun_host);
        ui.set_tun_vnet(self.settings.tun_vnet);
        ui.set_tun_flash(self.settings.tun_flash);
        ui.set_tun_promisc(self.settings.tun_promisc);
        ui.set_tun_route(self.settings.tun_route);
        ui.set_tun_protect(self.settings.tun_protect);
        ui.set_block_quic(self.settings.block_quic);
        ui.set_rt(self.settings.rt);
        ui.set_system_proxy_enabled(self.settings.system_proxy_enabled);
        ui.set_route_mode(match self.settings.bypass_mode.as_str() {
            "geo" => 1,
            "no" => 2,
            _ => 0,
        });
        ui.set_selected_server(self.selected_profile.map_or(-1, |index| index as i32));
        ui.set_download_rate("0 B/s".into());
        ui.set_upload_rate("0 B/s".into());
        ui.set_received_total("累计接收  0 B".into());
        ui.set_sent_total("累计发送  0 B".into());
        ui.set_connected_duration("00:00:00".into());
        ui.set_server_name("未连接".into());
        ui.set_server_detail("选择服务器后启动核心".into());
        ui.set_server_latency("—".into());
        self.clear_runtime(ui);
        self.publish_command_preview(ui);
        self.refresh_server_rows(ui);
    }

    fn read_settings(&mut self, ui: &OpenPPP2Design) {
        self.settings.working_dir = ui.get_working_dir().to_string();
        self.settings.settings_file = ui.get_settings_file().to_string();
        self.settings.settings_file = resolve_settings_path(&self.settings.settings_file)
            .to_string_lossy()
            .into_owned();
        self.settings.mode = match ui.get_mode_index() {
            2 => "server",
            1 => "proxy",
            _ => "client",
        }
        .to_string();
        self.settings.tun_enabled = ui.get_tun_enabled();
        self.settings.config_path = ui.get_config_path().to_string();
        self.settings.server_dir = ui.get_server_dir().to_string();
        self.settings.log_file = ui.get_core_log_file().to_string();
        self.settings.tui_log_file = ui.get_tui_log_file().to_string();
        self.settings.tui_log_enabled = ui.get_tui_log_enabled();
        self.settings.log_level = ui.get_log_level().to_string();
        self.settings.tun_ip = ui.get_tun_ip().to_string();
        self.settings.tun_gw = ui.get_tun_gw().to_string();
        self.settings.tun_mask = ui.get_tun_mask().to_string();
        self.settings.nic = ui.get_nic().to_string();
        self.settings.ngw = ui.get_ngw().to_string();
        self.settings.tun = ui.get_tun_name().to_string();
        self.settings.tun_driver = ui.get_tun_driver().to_string();
        self.settings.tcp_ip_cc = ui.get_tcp_ip_cc().to_string();
        self.settings.tun_mux = ui.get_tun_mux().to_string();
        self.settings.tun_mux_acceleration = ui.get_tun_mux_acceleration().to_string();
        self.settings.link_restart = ui.get_link_restart().to_string();
        self.settings.tun_lease_time = ui.get_tun_lease_time().to_string();
        self.settings.tun_ssmt = ui.get_tun_ssmt().to_string();
        self.settings.dns = ui.get_dns().to_string();
        self.settings.auto_restart = ui.get_auto_restart().to_string();
        self.settings.firewall_rules = ui.get_firewall_rules().to_string();
        self.settings.bypass_ngw = ui.get_bypass_ngw().to_string();
        self.settings.bypass_ngw6 = ui.get_bypass_ngw6().to_string();
        self.settings.bypass_nic = ui.get_bypass_nic().to_string();
        self.settings.bypass_nic6 = ui.get_bypass_nic6().to_string();
        self.settings.proxy_http_port = ui.get_proxy_http_port().to_string();
        self.settings.proxy_socks_port = ui.get_proxy_socks_port().to_string();
        self.settings.rpc_listen = ui.get_rpc_listen().to_string();
        self.settings.rpc_address = ui.get_rpc_address().to_string();
        self.settings.rpc_token = ui.get_rpc_token().to_string();
        self.settings.command = ui.get_advanced_command().to_string();
        self.settings.geo_rules_file = ui.get_geo_rules_file().to_string();
        self.settings.geosite_file = ui.get_geosite_file().to_string();
        self.settings.geoip_file = ui.get_geoip_file().to_string();
        self.settings.bypass_file = ui.get_bypass_file().to_string();
        self.settings.bypass6_file = ui.get_bypass6_file().to_string();
        self.settings.dns_rules_file = ui.get_dns_rules_file().to_string();
        self.settings.tun_static = ui.get_tun_static();
        self.settings.tun_host = ui.get_tun_host();
        self.settings.tun_vnet = ui.get_tun_vnet();
        self.settings.tun_flash = ui.get_tun_flash();
        self.settings.tun_promisc = ui.get_tun_promisc();
        self.settings.tun_route = ui.get_tun_route();
        self.settings.tun_protect = ui.get_tun_protect();
        self.settings.block_quic = ui.get_block_quic();
        self.settings.rt = ui.get_rt();
        self.settings.system_proxy_enabled = ui.get_system_proxy_enabled();
        self.settings.bypass_mode = match ui.get_route_mode() {
            1 => "geo",
            2 => "no",
            _ => "ip",
        }
        .to_string();
        self.settings.normalize_paths();
    }

    fn save(&mut self, ui: &OpenPPP2Design) -> bool {
        self.read_settings(ui);
        match self.settings.save() {
            Ok(()) => {
                self.error = None;
                self.publish_command_preview(ui);
                ui.set_notice(
                    format!(
                        "设置已保存到 {}；启动参数重启核心后生效",
                        self.settings.settings_file
                    )
                    .into(),
                );
                self.reload_profiles(ui);
                true
            }
            Err(error) => {
                self.error = Some(format!("保存设置失败：{error:#}"));
                self.publish_status(ui);
                false
            }
        }
    }

    fn publish_command_preview(&self, ui: &OpenPPP2Design) {
        let mut preview = prepared_core_args(&self.settings)
            .iter()
            .map(|argument| argument.to_string())
            .collect::<Vec<_>>()
            .join("\n");
        if !self.settings.rpc_token.is_empty() {
            preview = preview.replace(&self.settings.rpc_token, "••••");
        }
        ui.set_command_preview(preview.into());
    }

    fn reload_profiles(&mut self, ui: &OpenPPP2Design) {
        let (profiles, warning) = load_server_catalog(&self.settings);
        self.profiles = profiles;
        self.selected_profile = find_selected_local_server(&self.settings, &self.profiles);
        self.probes = Arc::new(Mutex::new(ProbeTable::default()));
        self.start_probes();
        self.server_rows.clear();
        self.server_rows_initialized = false;
        ui.set_selected_server(self.selected_profile.map_or(-1, |index| index as i32));
        self.refresh_server_rows(ui);
        if let Some(warning) = warning {
            ui.set_notice(warning.into());
        }
    }

    fn select_server(&mut self, ui: &OpenPPP2Design, index: usize) {
        let Some(profile) = self.profiles.get(index) else {
            return;
        };
        self.settings.config_path =
            relative_path_string(&working_directory(&self.settings), &profile.path);
        self.selected_profile = Some(index);
        ui.set_config_path(self.settings.config_path.clone().into());
        ui.set_selected_server(index as i32);
        ui.set_notice(
            if self.core.is_some() {
                format!("已选择 {}；停止并重新启动后生效", profile.name)
            } else {
                format!("已选择 {}；点击启动连接", profile.name)
            }
            .into(),
        );
    }

    fn start(&mut self, ui: &OpenPPP2Design) {
        if self.core.is_some() || self.launch_rx.is_some() {
            return;
        }
        self.stop_after_launch = false;
        if !self.save(ui) {
            return;
        }
        if !self.settings.rpc_address.trim().is_empty() {
            self.core = Some(CoreClient::rpc(
                self.settings.rpc_address.clone(),
                self.settings.rpc_token.clone(),
            ));
            self.status = "正在连接已有核心".to_string();
            ui.set_core_launching(true);
            self.publish_status(ui);
            return;
        }
        let working_dir = working_directory(&self.settings);
        if !working_dir.is_dir() {
            self.error = Some(format!("启动目录不存在：{}", working_dir.display()));
            self.publish_status(ui);
            return;
        }
        let mut args = prepared_core_args(&self.settings);
        match validate_core_log_path(&self.settings) {
            Ok(Some(path)) => {
                set_command_argument(&mut args, "--log-file", &display_path_string(&path))
            }
            Ok(None) => remove_command_argument(&mut args, "--log-file"),
            Err(error) => {
                self.error = Some(format!("核心日志路径无效：{error:#}"));
                self.publish_status(ui);
                return;
            }
        }
        // The egui desktop holds this port for its lifetime. Hold it while
        // the Slint-owned core runs so either desktop can be viewed while
        // only one of them owns a local core.
        if self.core_port_guard.is_none() {
            match bind_loopback_with_retry(18991, Duration::from_secs(3)) {
                Ok(guard) => self.core_port_guard = Some(guard),
                Err(_) => {
                    self.error = Some(
                        "现有 ppp-tui 桌面窗口正在运行；请先关闭它，再启动 Slint 核心".to_string(),
                    );
                    self.publish_status(ui);
                    return;
                }
            }
        }
        if normalized_launch_mode(&self.settings.mode) == "client"
            && self.settings.tun_enabled
            && !is_process_elevated()
        {
            match relaunch_elevated() {
                Ok(()) => std::process::exit(0),
                Err(error) => {
                    self.core_port_guard = None;
                    self.error = Some(format!("无法请求 UAC 提升：{error:#}"));
                    self.publish_status(ui);
                    return;
                }
            }
        }
        if let Err(error) = std::env::set_current_dir(&working_dir) {
            self.core_port_guard = None;
            self.error = Some(format!("无法进入启动目录：{error}"));
            self.publish_status(ui);
            return;
        }
        set_command_argument(&mut args, "--headless", "yes");
        let (tx, rx) = channel();
        self.launch_rx = Some(rx);
        self.status = "正在启动核心".to_string();
        self.error = None;
        ui.set_core_launching(true);
        self.publish_status(ui);
        std::thread::spawn(move || {
            let result = CoreClient::in_process(&args).map_err(|error| format!("{error:#}"));
            if let Err(std::sync::mpsc::SendError(Ok(core))) = tx.send(result) {
                let _ = core.stop_owned();
            }
        });
    }

    fn stop(&mut self, ui: &OpenPPP2Design) {
        if self.launch_rx.is_some() {
            self.stop_after_launch = true;
            self.status = "正在等待核心启动完成后停止".to_string();
            self.publish_status(ui);
            return;
        }
        self.error = None;
        if let Some(core) = self.core.take() {
            if core.is_in_process() {
                if let Err(error) = core.stop_owned() {
                    self.error = Some(format!("停止核心失败：{error:#}"));
                }
            }
        }
        self.connect_rx = None;
        self.core_port_guard = None;
        self.snapshot = None;
        self.traffic.reset();
        self.restart_attempts = 0;
        self.status = "已停止".to_string();
        if self.error.is_none() {
            ui.set_notice("核心已停止".into());
        }
        ui.set_core_running(false);
        ui.set_core_launching(false);
        self.clear_runtime(ui);
        self.publish_status(ui);
    }

    fn restart(&mut self, ui: &OpenPPP2Design) {
        if self.launch_rx.is_some() {
            ui.set_notice("核心正在启动，请稍后再应用设置".into());
        } else if self.core.as_ref().is_some_and(|core| !core.is_in_process()) {
            ui.set_notice("当前连接的是外部核心；请在核心进程中重启".into());
        } else {
            if self.core.is_some() {
                self.stop(ui);
            }
            self.start(ui);
        }
    }

    fn apply_routes(&mut self, ui: &OpenPPP2Design) {
        if !self.save(ui) {
            return;
        }
        if self.core.as_ref().is_some_and(CoreClient::is_in_process) {
            self.stop(ui);
            self.start(ui);
            ui.set_notice("分流设置已保存，正在重启核心应用".into());
        } else {
            ui.set_notice("分流设置已保存；下次启动核心时生效".into());
        }
    }

    fn tick(&mut self, ui: &OpenPPP2Design) {
        self.finish_launch(ui);
        self.finish_connect(ui);
        self.begin_connect();
        let mut replies = Vec::new();
        if let Some(core) = self.core.as_mut() {
            if core.is_connected() {
                for _ in 0..64 {
                    match core.poll() {
                        Ok(Some(reply)) => replies.push(reply),
                        Ok(None) => break,
                        Err(error) => {
                            self.error = Some(format!("核心通道错误：{error:#}"));
                            core.disconnect();
                            break;
                        }
                    }
                }
                if core.is_authenticated() && !core.has_pending() {
                    if self.last_snapshot_request.elapsed() >= Duration::from_secs(1) {
                        if core.request_command(CoreCommand::GetSnapshot).is_ok() {
                            self.last_snapshot_request = Instant::now();
                        }
                    }
                }
            }
        }
        for reply in replies {
            self.handle_reply(ui, reply);
        }
        self.handle_exit(ui);
        self.refresh_server_rows(ui);
        self.publish_status(ui);
    }

    fn finish_launch(&mut self, ui: &OpenPPP2Design) {
        let Some(rx) = self.launch_rx.take() else {
            return;
        };
        match rx.try_recv() {
            Ok(Ok(core)) => {
                if self.stop_after_launch {
                    self.stop_after_launch = false;
                    if let Err(error) = core.stop_owned() {
                        self.error = Some(format!("停止核心失败：{error:#}"));
                    }
                    self.core_port_guard = None;
                    self.status = "已停止".to_string();
                    ui.set_core_running(false);
                    ui.set_core_launching(false);
                    self.publish_status(ui);
                    return;
                }
                core.register_emergency_stop();
                self.core = Some(core);
                self.status = "核心已启动，正在连接".to_string();
                self.error = None;
                ui.set_core_running(true);
                ui.set_core_launching(false);
            }
            Ok(Err(error)) => {
                self.stop_after_launch = false;
                self.core_port_guard = None;
                self.status = "核心启动失败".to_string();
                self.error = Some(error);
                ui.set_core_launching(false);
            }
            Err(TryRecvError::Empty) => self.launch_rx = Some(rx),
            Err(TryRecvError::Disconnected) => {
                self.stop_after_launch = false;
                self.core_port_guard = None;
                self.status = "启动线程已退出".to_string();
                self.error = Some("未收到核心启动结果".to_string());
                ui.set_core_launching(false);
            }
        }
    }

    fn begin_connect(&mut self) {
        let Some(core) = self.core.as_ref() else {
            return;
        };
        if core.is_in_process()
            || core.is_connected()
            || self.connect_rx.is_some()
            || self.last_connect.elapsed() < Duration::from_secs(2)
        {
            return;
        }
        let address = core.address().to_string();
        let (tx, rx) = channel();
        self.connect_rx = Some(rx);
        self.last_connect = Instant::now();
        std::thread::spawn(move || {
            let result = RpcClient::connect_socket(&address).map_err(|error| format!("{error:#}"));
            let _ = tx.send(result);
        });
    }

    fn finish_connect(&mut self, ui: &OpenPPP2Design) {
        let Some(rx) = self.connect_rx.take() else {
            return;
        };
        match rx.try_recv() {
            Ok(Ok(stream)) => {
                if let Some(core) = self.core.as_mut() {
                    if let Err(error) = core.attach_stream(stream) {
                        self.error = Some(format!("连接核心失败：{error:#}"));
                    } else {
                        self.status = "正在验证核心连接".to_string();
                        ui.set_core_running(true);
                        ui.set_core_launching(false);
                    }
                }
            }
            Ok(Err(error)) => self.error = Some(format!("RPC 连接失败：{error}")),
            Err(TryRecvError::Empty) => self.connect_rx = Some(rx),
            Err(TryRecvError::Disconnected) => self.error = Some("RPC 连接线程已退出".to_string()),
        }
    }

    fn handle_reply(&mut self, ui: &OpenPPP2Design, reply: Response) {
        match reply {
            Response::Result { method, .. } if method == "hello" => {
                self.status = "已连接核心".to_string();
                self.error = None;
                ui.set_core_running(true);
                ui.set_core_launching(false);
            }
            Response::Result { method, value, .. } if method == "get_snapshot" => {
                match serde_json::from_value::<Snapshot>(value) {
                    Ok(snapshot) => {
                        self.traffic.feed(
                            snapshot.traffic.in_bytes,
                            snapshot.traffic.out_bytes,
                            snapshot.monotonic_ms,
                        );
                        self.status = if snapshot.is_connected() {
                            "已连接".to_string()
                        } else if snapshot.is_transitioning() {
                            "连接中".to_string()
                        } else {
                            snapshot.phase.clone()
                        };
                        self.render_snapshot(ui, &snapshot);
                        self.snapshot = Some(snapshot);
                        self.error = None;
                        self.restart_attempts = 0;
                    }
                    Err(error) => self.error = Some(format!("快照解析失败：{error}")),
                }
            }
            Response::Result { method, .. } if method == "switch_server" => {
                ui.set_notice("服务器切换请求已提交".into());
            }
            Response::Result { method, .. } if method == "switch_rank1" => {
                ui.set_notice("首选入口切换请求已提交".into());
            }
            Response::Error { code, message, .. } => {
                self.error = Some(format!("核心错误 {code}：{message}"));
            }
            _ => {}
        }
    }

    fn handle_exit(&mut self, ui: &OpenPPP2Design) {
        let exited = self
            .core
            .as_ref()
            .is_some_and(|core| core.is_in_process() && !core.is_running());
        if !exited {
            return;
        }
        let planned = self
            .core
            .as_ref()
            .is_some_and(CoreClient::restart_requested);
        self.core = None;
        self.snapshot = None;
        self.traffic.reset();
        ui.set_core_running(false);
        ui.set_core_launching(false);
        self.clear_runtime(ui);
        if ppp_tui::core::restart::allow_restart_after_exit(planned, &mut self.restart_attempts) {
            self.status = if planned {
                "核心请求重启，正在重新启动".to_string()
            } else {
                format!("核心退出，自动重启 {}/3", self.restart_attempts)
            };
            self.start(ui);
        } else {
            self.core_port_guard = None;
            self.status = "核心多次退出，已停止自动重启".to_string();
            self.error = Some("请检查网络与配置后手动启动".to_string());
        }
    }

    fn render_snapshot(&self, ui: &OpenPPP2Design, snapshot: &Snapshot) {
        ui.set_has_snapshot(true);
        let outbounds: Vec<_> = snapshot
            .outbounds
            .iter()
            .filter(|item| snapshot.is_visible_server_outbound(item))
            .map(|item| OutboundItem {
                name: if item.display_name.is_empty() {
                    item.tag.clone()
                } else {
                    item.display_name.clone()
                }
                .into(),
                detail: format!(
                    "{} · {} · 重连 {}",
                    item.server,
                    outbound_entry(item),
                    item.reconnects
                )
                .into(),
                state: format!(
                    "{}{}{}{}",
                    if item.active { "主出口 " } else { "" },
                    if item.route_used { "分流 " } else { "" },
                    match item.state {
                        1 => "已连接",
                        0 => "连接中",
                        2 => "重连中",
                        _ => "未知",
                    },
                    if item.probe_checked {
                        if item.probe_reachable {
                            format!(" · {}ms", item.probe_rtt_ms)
                        } else {
                            " · 不可达".into()
                        }
                    } else {
                        "".into()
                    },
                )
                .into(),
                action: if item.active { "Rank #1" } else { "切换" }.into(),
                active: item.active,
            })
            .collect();
        ui.set_outbound_items(ModelRc::from(Rc::new(VecModel::from(outbounds))));
        let (rx, tx) = self.traffic.latest().map_or((0, 0), |point| {
            (point.rx_bytes_per_sec, point.tx_bytes_per_sec)
        });
        ui.set_download_rate(format_rate(rx).into());
        ui.set_upload_rate(format_rate(tx).into());
        ui.set_received_total(
            format!("累计接收  {}", format_bytes(snapshot.traffic.in_bytes)).into(),
        );
        ui.set_sent_total(format!("累计发送  {}", format_bytes(snapshot.traffic.out_bytes)).into());
        let seconds = snapshot.duration_ms / 1000;
        ui.set_connected_duration(
            format!(
                "{:02}:{:02}:{:02}",
                seconds / 3600,
                seconds / 60 % 60,
                seconds % 60
            )
            .into(),
        );
        let name = if snapshot.vpn_server.is_empty() {
            &snapshot.server
        } else {
            &snapshot.vpn_server
        };
        ui.set_server_name(if name.is_empty() { "未连接" } else { name }.into());
        ui.set_server_detail(snapshot.transport.clone().into());
        let rtt = snapshot
            .outbounds
            .iter()
            .find(|outbound| outbound.active && outbound.probe_reachable)
            .map(|outbound| format!("{} ms", outbound.probe_rtt_ms))
            .unwrap_or_else(|| "—".to_string());
        ui.set_server_latency(rtt.into());
        ui.set_transport_status(snapshot.connection.clone().into());
        ui.set_runtime_api(if snapshot.control_api.enabled {
            format!("运行端点：{}", snapshot.control_api.listen).into()
        } else {
            "运行端点：关闭".into()
        });
        ui.set_mux_summary(snapshot.mux_state.clone().into());
        ui.set_http_summary(snapshot.http_proxy.clone().into());
        ui.set_socks_summary(snapshot.socks_proxy.clone().into());
        let active = snapshot.outbounds.iter().find(|item| item.active);
        ui.set_active_outbound(
            active
                .map(|item| {
                    if item.display_name.is_empty() {
                        item.tag.as_str()
                    } else {
                        item.display_name.as_str()
                    }
                })
                .unwrap_or("—")
                .into(),
        );
        ui.set_mux_mode(
            format!(
                "{} · {} links",
                snapshot.effective_mux_mode, snapshot.mux_active_links
            )
            .into(),
        );
        ui.set_connection_items(info_model(vec![
            (
                "服务器",
                if snapshot.vpn_server.is_empty() {
                    snapshot.server.clone()
                } else {
                    snapshot.vpn_server.clone()
                },
            ),
            ("GUID", snapshot.guid.clone()),
            ("传输", snapshot.transport.clone()),
            ("旁路模式", snapshot.bypass_mode.clone()),
            ("HTTP 代理", snapshot.http_proxy.clone()),
            ("SOCKS 代理", snapshot.socks_proxy.clone()),
            ("运行环境", snapshot.role.clone()),
            (
                "累计流量",
                format!(
                    "↓ {} / ↑ {}",
                    format_bytes(snapshot.traffic.in_bytes),
                    format_bytes(snapshot.traffic.out_bytes)
                ),
            ),
        ]));
        ui.set_api_items(info_model(vec![
            (
                "状态",
                if snapshot.control_api.enabled {
                    "开启"
                } else {
                    "关闭"
                }
                .into(),
            ),
            ("监听地址", snapshot.control_api.listen.clone()),
            ("鉴权方式", snapshot.control_api.authentication.clone()),
            (
                "Token",
                if snapshot.control_api.token_configured {
                    "已配置"
                } else {
                    "未配置"
                }
                .into(),
            ),
            (
                "客户端",
                format!(
                    "{} / {}",
                    snapshot.control_api.clients, snapshot.control_api.max_clients
                ),
            ),
        ]));
        ui.set_tunnel_items(info_model(vec![
            ("模式", snapshot.network.mode.clone()),
            ("适配器", snapshot.network.adapter.clone()),
            ("逻辑 IPv4", snapshot.network.logical_ipv4.clone()),
            ("逻辑 IPv6", snapshot.network.logical_ipv6.clone()),
            ("隧道 DNS", snapshot.network.tunnel_dns.clone()),
            ("链路", snapshot.network.link_state.clone()),
            ("MUX", snapshot.network.mux_state.clone()),
            ("TCP/IP 传输", snapshot.network.tcp_ip_transport.clone()),
            ("DNS 传输", snapshot.network.dns_transport.clone()),
        ]));
        ui.set_mux_items(info_model(vec![
            ("请求模式", snapshot.requested_mux_mode.clone()),
            ("有效模式", snapshot.effective_mux_mode.clone()),
            ("接收顺序", snapshot.ordering_label().to_string()),
            ("活动链路", snapshot.mux_active_links.to_string()),
            ("回退原因", snapshot.mux_fallback_reason.clone()),
            ("运行环境", snapshot.hosting_environment.clone()),
        ]));
        ui.set_tun_items(interface_model(
            snapshot.network.tun.as_ref(),
            &snapshot.network,
            Some(snapshot.dataplane.wintun.interface_mtu),
        ));
        ui.set_nic_items(interface_model(
            snapshot.network.nic.as_ref(),
            &snapshot.network,
            None,
        ));
        ui.set_route_items(info_model(vec![
            ("当前模式", snapshot.bypass_mode.clone()),
            ("IPv4 旁路网关", snapshot.routes.bypass_gateway.clone()),
            ("IPv6 旁路网关", snapshot.routes.bypass_gateway_ipv6.clone()),
            ("GEO 规则", snapshot.geo.rule_count.to_string()),
            ("静态网络", snapshot.geo.static_networks.to_string()),
            ("DNS 规则", snapshot.routes.dns_rule_count.to_string()),
            ("直接 DNS", snapshot.geo.direct_dns.join(" / ")),
            ("IPv4 规则文件", snapshot.routes.bypass_ipv4_file.clone()),
            ("IPv6 规则文件", snapshot.routes.bypass_ipv6_file.clone()),
            ("GEO 规则文件", snapshot.routes.geo_rules_file.clone()),
            ("DNS 规则文件", snapshot.routes.dns_rules_file.clone()),
        ]));
        ui.set_split_items(info_model(
            snapshot
                .geo
                .split_rules
                .iter()
                .map(|rule| {
                    (
                        rule.matcher.as_str(),
                        if rule.display.is_empty() {
                            rule.outbound.clone()
                        } else {
                            format!("{} · {}", rule.outbound, rule.display)
                        },
                    )
                })
                .collect(),
        ));
        let samples = self.traffic.samples();
        let peak_rx = samples
            .iter()
            .map(|sample| sample.rx_bytes_per_sec)
            .max()
            .unwrap_or(1)
            .max(1);
        let peak_tx = samples
            .iter()
            .map(|sample| sample.tx_bytes_per_sec)
            .max()
            .unwrap_or(1)
            .max(1);
        ui.set_rx_bars(int_model(
            samples
                .iter()
                .map(|sample| {
                    ((sample.rx_bytes_per_sec as u128 * 100 / peak_rx as u128).max(2)) as i32
                })
                .collect(),
        ));
        ui.set_tx_bars(int_model(
            samples
                .iter()
                .map(|sample| {
                    ((sample.tx_bytes_per_sec as u128 * 100 / peak_tx as u128).max(2)) as i32
                })
                .collect(),
        ));
        ui.set_tcp_ip_stack(snapshot.network.tcp_ip_cc.clone().into());
        ui.set_udp_443_status(snapshot.network.block_quic.clone().into());
        if let Some(tun) = &snapshot.network.tun {
            ui.set_network_adapter(tun.name.clone().into());
            ui.set_network_ipv4(tun.ipv4.clone().into());
            ui.set_network_gateway(tun.gateway.clone().into());
            ui.set_network_dns(tun.dns.join("  /  ").into());
        } else {
            ui.set_network_adapter("—".into());
            ui.set_network_ipv4("—".into());
            ui.set_network_gateway("—".into());
            ui.set_network_dns("—".into());
        }
        if let Some(nic) = &snapshot.network.nic {
            ui.set_physical_adapter(nic.name.clone().into());
            ui.set_physical_ipv4(nic.ipv4.clone().into());
            ui.set_physical_gateway(nic.gateway.clone().into());
        } else {
            ui.set_physical_adapter("—".into());
            ui.set_physical_ipv4("—".into());
            ui.set_physical_gateway("—".into());
        }
        ui.set_network_mtu(snapshot.dataplane.wintun.interface_mtu.to_string().into());
    }

    fn clear_network(&self, ui: &OpenPPP2Design) {
        ui.set_network_adapter("—".into());
        ui.set_network_ipv4("—".into());
        ui.set_network_gateway("—".into());
        ui.set_network_dns("—".into());
        ui.set_network_mtu("—".into());
        ui.set_physical_adapter("—".into());
        ui.set_physical_ipv4("—".into());
        ui.set_physical_gateway("—".into());
        ui.set_transport_status("—".into());
        ui.set_tcp_ip_stack("—".into());
        ui.set_udp_443_status("—".into());
        ui.set_mux_summary("—".into());
        ui.set_http_summary("—".into());
    }

    fn clear_runtime(&self, ui: &OpenPPP2Design) {
        ui.set_has_snapshot(false);
        ui.set_active_outbound("—".into());
        ui.set_mux_mode("—".into());
        ui.set_socks_summary("—".into());
        for clear in [
            OpenPPP2Design::set_connection_items,
            OpenPPP2Design::set_api_items,
            OpenPPP2Design::set_tunnel_items,
            OpenPPP2Design::set_mux_items,
            OpenPPP2Design::set_tun_items,
            OpenPPP2Design::set_nic_items,
            OpenPPP2Design::set_route_items,
            OpenPPP2Design::set_split_items,
        ] {
            clear(ui, info_model(Vec::new()));
        }
        ui.set_rx_bars(int_model(Vec::new()));
        ui.set_tx_bars(int_model(Vec::new()));
        ui.set_outbound_items(ModelRc::from(Rc::new(VecModel::from(
            Vec::<OutboundItem>::new(),
        ))));
        ui.set_download_rate("0 B/s".into());
        ui.set_upload_rate("0 B/s".into());
        ui.set_received_total("累计接收  0 B".into());
        ui.set_sent_total("累计发送  0 B".into());
        ui.set_connected_duration("00:00:00".into());
        ui.set_server_name("未连接".into());
        ui.set_server_detail("选择服务器后启动核心".into());
        ui.set_server_latency("—".into());
        self.clear_network(ui);
    }

    fn refresh_server_rows(&mut self, ui: &OpenPPP2Design) {
        let guard = self.probes.lock().ok();
        let rows: Vec<_> = self
            .profiles
            .iter()
            .map(|profile| {
                let latency = guard
                    .as_ref()
                    .and_then(|table| table.state(&profile.name))
                    .map(|state| match state {
                        ProbeState::Ok(ms) => format!("{ms} ms"),
                        ProbeState::Unreachable => "不可达".to_string(),
                        ProbeState::Pending | ProbeState::Probing => "探测中".to_string(),
                    })
                    .unwrap_or_else(|| "探测中".to_string());
                (
                    profile.name.clone(),
                    format!(
                        "{} · {} 个入口{}",
                        profile.server,
                        profile.entries.len(),
                        if profile.entries.len() > 1 {
                            format!(" · {}", profile.entries.join(" / "))
                        } else {
                            String::new()
                        }
                    ),
                    latency,
                )
            })
            .collect();
        if self.server_rows_initialized && rows == self.server_rows {
            return;
        }
        let model: Vec<_> = rows
            .iter()
            .map(|(name, detail, latency)| ServerItem {
                name: name.clone().into(),
                detail: detail.clone().into(),
                latency: latency.clone().into(),
            })
            .collect();
        ui.set_server_items(ModelRc::from(Rc::new(VecModel::from(model))));
        self.server_rows = rows;
        self.server_rows_initialized = true;
    }

    fn publish_status(&self, ui: &OpenPPP2Design) {
        if ui.get_status_label().as_str() != self.status {
            ui.set_status_label(self.status.clone().into());
        }
        if let Some(error) = &self.error {
            if ui.get_notice().as_str() != error {
                ui.set_notice(error.clone().into());
            }
        }
    }

    fn switch_outbound(&mut self, ui: &OpenPPP2Design, index: usize) {
        let Some(outbound) = self.snapshot.as_ref().and_then(|snapshot| {
            snapshot
                .outbounds
                .iter()
                .filter(|item| snapshot.is_visible_server_outbound(item))
                .nth(index)
        }) else {
            return;
        };
        let command = CoreCommand::Switch {
            tag: outbound.tag.clone(),
            ranked_first: outbound.active,
        };
        match self.core.as_mut() {
            Some(core) => match core.request_command(command) {
                Ok(()) => ui.set_notice("切换请求已发送".into()),
                Err(error) => {
                    self.error = Some(format!("无法切换出口：{error:#}"));
                    self.publish_status(ui);
                }
            },
            None => ui.set_notice("核心尚未连接".into()),
        }
    }
}

#[cfg(windows)]
fn is_process_elevated() -> bool {
    #[link(name = "shell32")]
    unsafe extern "system" {
        fn IsUserAnAdmin() -> i32;
    }
    // SAFETY: This Win32 function takes no arguments and returns a BOOL.
    unsafe { IsUserAnAdmin() != 0 }
}

#[cfg(not(windows))]
fn is_process_elevated() -> bool {
    false
}

#[cfg(windows)]
fn relaunch_elevated() -> Result<()> {
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

    let executable = wide(std::env::current_exe()?.as_os_str());
    let operation = wide(OsStr::new("runas"));
    let mut parameters = std::env::args_os()
        .skip(1)
        .map(|arg| format!("\"{}\"", arg.to_string_lossy().replace('"', "\\\"")))
        .collect::<Vec<_>>()
        .join(" ");
    parameters.push_str(" --slint-auto-start");
    let parameters = wide(OsStr::new(&parameters));
    let directory = wide(std::env::current_dir()?.as_os_str());
    // SAFETY: Each pointer refers to a live NUL-terminated UTF-16 buffer.
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
    anyhow::ensure!(result > 32, "ShellExecuteW(runas) returned {result}");
    Ok(())
}

#[cfg(not(windows))]
fn relaunch_elevated() -> Result<()> {
    anyhow::bail!("管理员重启仅支持 Windows")
}

#[cfg(windows)]
struct OleApartment;

#[cfg(windows)]
impl OleApartment {
    fn initialize() -> Result<Self> {
        #[link(name = "ole32")]
        unsafe extern "system" {
            fn OleInitialize(reserved: *mut std::ffi::c_void) -> i32;
        }
        // SAFETY: OleInitialize accepts a null reserved pointer.
        let result = unsafe { OleInitialize(std::ptr::null_mut()) };
        anyhow::ensure!(result >= 0, "OleInitialize failed: 0x{:08x}", result as u32);
        Ok(Self)
    }
}

#[cfg(windows)]
impl Drop for OleApartment {
    fn drop(&mut self) {
        #[link(name = "ole32")]
        unsafe extern "system" {
            fn OleUninitialize();
        }
        // SAFETY: Paired with OleInitialize on the same UI thread.
        unsafe { OleUninitialize() };
    }
}

fn bind_loopback_with_retry(port: u16, timeout: Duration) -> std::io::Result<TcpListener> {
    let deadline = Instant::now() + timeout;
    loop {
        match TcpListener::bind(("127.0.0.1", port)) {
            Ok(listener) => return Ok(listener),
            Err(_) if Instant::now() < deadline => {
                std::thread::sleep(Duration::from_millis(50));
            }
            Err(error) => return Err(error),
        }
    }
}

fn main() -> Result<()> {
    let auto_start = std::env::args_os()
        .skip(1)
        .any(|arg| arg.to_string_lossy() == "--slint-auto-start");
    let _single_instance = bind_loopback_with_retry(
        18992,
        if auto_start {
            Duration::from_secs(5)
        } else {
            Duration::ZERO
        },
    )
    .context("Slint 桌面窗口已经运行")?;
    let settings = StartupSettings::from_cli();
    #[cfg(windows)]
    {
        // The embedded core initializes the process main thread as MTA before
        // Rust main runs. Winit's Windows window needs an STA thread for OLE.
        std::thread::spawn(move || run_ui(settings, auto_start))
            .join()
            .map_err(|_| anyhow::anyhow!("Slint 窗口线程异常退出"))?
    }
    #[cfg(not(windows))]
    {
        run_ui(settings, auto_start)
    }
}

fn run_ui(settings: StartupSettings, auto_start: bool) -> Result<()> {
    #[cfg(windows)]
    let _ole_apartment = OleApartment::initialize()?;
    let ui = OpenPPP2Design::new()?;
    let backend = Rc::new(RefCell::new(Backend::new(settings)));
    backend.borrow_mut().init_ui(&ui);
    if auto_start {
        if is_process_elevated() {
            let weak = ui.as_weak();
            let state = Rc::clone(&backend);
            slint::Timer::single_shot(Duration::from_millis(250), move || {
                if let Some(ui) = weak.upgrade() {
                    state.borrow_mut().start(&ui);
                }
            });
        } else {
            ui.set_notice("UAC 提升未完成；请以管理员身份启动后重试".into());
        }
    }

    let weak = ui.as_weak();
    let state = Rc::clone(&backend);
    ui.on_save_requested(move || {
        if let Some(ui) = weak.upgrade() {
            state.borrow_mut().save(&ui);
        }
    });
    let weak = ui.as_weak();
    let state = Rc::clone(&backend);
    ui.on_start_requested(move || {
        if let Some(ui) = weak.upgrade() {
            state.borrow_mut().start(&ui);
        }
    });
    let weak = ui.as_weak();
    let state = Rc::clone(&backend);
    ui.on_stop_requested(move || {
        if let Some(ui) = weak.upgrade() {
            state.borrow_mut().stop(&ui);
        }
    });
    let weak = ui.as_weak();
    let state = Rc::clone(&backend);
    ui.on_restart_requested(move || {
        if let Some(ui) = weak.upgrade() {
            state.borrow_mut().restart(&ui);
        }
    });
    let weak = ui.as_weak();
    let state = Rc::clone(&backend);
    ui.on_server_picked(move |index| {
        if let Some(ui) = weak.upgrade() {
            state.borrow_mut().select_server(&ui, index as usize);
        }
    });
    let weak = ui.as_weak();
    let state = Rc::clone(&backend);
    ui.on_outbound_picked(move |index| {
        if let Some(ui) = weak.upgrade() {
            state.borrow_mut().switch_outbound(&ui, index as usize);
        }
    });
    let weak = ui.as_weak();
    let state = Rc::clone(&backend);
    ui.on_routes_requested(move || {
        if let Some(ui) = weak.upgrade() {
            state.borrow_mut().apply_routes(&ui);
        }
    });
    let weak = ui.as_weak();
    let state = Rc::clone(&backend);
    ui.on_refresh_probes_requested(move || {
        if let Some(ui) = weak.upgrade() {
            let mut backend = state.borrow_mut();
            backend.probes = Arc::new(Mutex::new(ProbeTable::default()));
            backend.start_probes();
            backend.server_rows.clear();
            backend.refresh_server_rows(&ui);
            ui.set_notice("正在重新探测服务器延迟".into());
        }
    });
    let weak = ui.as_weak();
    let state = Rc::clone(&backend);
    ui.on_refresh_catalog_requested(move || {
        if let Some(ui) = weak.upgrade() {
            let mut backend = state.borrow_mut();
            backend.read_settings(&ui);
            backend.reload_profiles(&ui);
            ui.set_notice("服务器配置已刷新".into());
        }
    });
    let timer = slint::Timer::default();
    let weak = ui.as_weak();
    let state = Rc::clone(&backend);
    timer.start(
        slint::TimerMode::Repeated,
        Duration::from_millis(100),
        move || {
            if let Some(ui) = weak.upgrade() {
                state.borrow_mut().tick(&ui);
            }
        },
    );
    let result = ui.run();
    backend.borrow_mut().stop(&ui);
    result.context("Slint window failed")
}
