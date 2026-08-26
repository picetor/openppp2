//! Linux terminal front-end (ratatui/crossterm).
//!
//! Same shared business layer as the window front-end: the same RPC client,
//! core launcher, startup settings and server catalog.  Only presentation
//! and input differ (ASCII cards, character bar charts, keyboard focus).

pub mod events;
pub mod widgets;

use std::io;
use std::sync::mpsc::{channel, Receiver};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use anyhow::{Context, Result};
use crossterm::event::{self, Event};
use crossterm::terminal::{
    disable_raw_mode, enable_raw_mode, EnterAlternateScreen, LeaveAlternateScreen,
};
use crossterm::ExecutableCommand;
use ratatui::backend::CrosstermBackend;
use ratatui::layout::{Constraint, Direction, Layout, Rect};
use ratatui::style::{Modifier, Style};
use ratatui::text::{Line, Span};
use ratatui::widgets::{Block, Borders, Paragraph};
use ratatui::Terminal;

use crate::core::command::{
    normalize_core_args, remove_command_argument, set_bool_if_non_default, set_command_argument,
    set_optional_command_argument, set_optional_if_not_default, split_command_line,
};
use crate::core::launcher::Launcher;
use crate::core::probe::{spawn_probe_loop, ProbeState, ProbeTable};
use crate::core::server_catalog::{load_server_catalog, LocalServerProfile};
use crate::core::settings::{
    normalize_log_level, normalize_tcp_ip_cc, normalized_launch_mode, StartupSettings,
};
use crate::core::traffic::{format_bytes, format_rate, TrafficHistory};
use crate::rpc::schema::{Outbound, Snapshot};
use crate::rpc::{CoreCommand, Response, RpcClient};

use events::{Action, EventReader};
use widgets::{bar_chart_columns, plain_card, rule_line};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum View {
    Overview,
    Network,
    Servers,
    Routes,
    Settings,
}

pub const VIEW_TITLES: [&str; 5] = ["Overview", "Network", "Servers", "Routes", "Settings"];

pub struct TerminalApp {
    view: View,
    settings: StartupSettings,
    local_servers: Vec<LocalServerProfile>,
    selected_local_server: Option<usize>,
    /// Servers page: focused row index (independent selection + scroll).
    server_selection: usize,
    server_scroll: usize,
    /// Settings page: focused field row.
    settings_selection: usize,
    settings_scroll: usize,
    rpc: Option<RpcClient>,
    rpc_connect_rx: Option<Receiver<anyhow::Result<std::net::TcpStream>>>,
    launcher: Option<Launcher>,
    catalog_core: bool,
    launch_rx: Option<Receiver<Result<Launcher, String>>>,
    snapshot: Option<Snapshot>,
    traffic: TrafficHistory,
    last_refresh: Instant,
    last_connect_attempt: Instant,
    launching: bool,
    status: String,
    error: Option<String>,
    quit: bool,
    confirm_quit: bool,
    /// Direct TCP probe table so server latency is visible on startup before
    /// any core (or VPN link) is started.
    probe_table: Arc<Mutex<ProbeTable>>,
    /// Help overlay open.
    help_open: bool,
    /// Servers page: active filter text.
    filter: Option<String>,
    /// Servers page: currently typing a filter (raw chars go to `filter`).
    filter_active: bool,
    /// Settings page: field index being edited (raw chars go to
    /// `editing_text`); None when not editing.
    editing: Option<usize>,
    editing_text: String,
    /// Stop-core confirmation latch (press o twice).
    confirm_stop: bool,
    /// Number of automatic relaunches after an owned core exits unexpectedly.
    auto_restart_count: u32,
}

impl TerminalApp {
    pub fn new(settings: StartupSettings) -> Self {
        let (local_servers, catalog_error) = load_server_catalog(&settings);
        let selected_local_server = local_servers
            .iter()
            .position(|profile| {
                profile
                    .path
                    .to_string_lossy()
                    .to_ascii_lowercase()
                    .ends_with(&settings.config_path.to_ascii_lowercase())
                    || settings.config_path.contains(&profile.name)
            })
            .or_else(|| {
                local_servers
                    .iter()
                    .position(|profile| profile.name == settings.config_path)
            });
        let status = if local_servers.is_empty() {
            "no server configurations read yet".to_string()
        } else {
            format!("{} server configuration(s) read", local_servers.len())
        };
        // Probe the server catalog directly so latency is visible before any
        // core (or VPN link) is started; the control-plane core has no
        // outbound rows to probe with.
        let probe_table = Arc::new(Mutex::new(ProbeTable::default()));
        {
            let targets: Arc<Vec<(String, String)>> = Arc::new(
                local_servers
                    .iter()
                    .map(|profile| (profile.name.clone(), profile.server.clone()))
                    .collect(),
            );
            spawn_probe_loop(targets, Arc::clone(&probe_table));
        }
        let has_rpc = !settings.rpc_address.trim().is_empty();
        let launch_direct = settings.launch_direct && !has_rpc;
        let initial_view = if has_rpc || launch_direct {
            View::Overview
        } else {
            View::Servers
        };
        let rpc = has_rpc.then(|| {
            RpcClient::new(
                settings.rpc_address.trim().to_string(),
                settings.rpc_token.trim().to_string(),
            )
        });
        let status = if has_rpc {
            "connecting to existing core".to_string()
        } else if launch_direct {
            "starting core from command line".to_string()
        } else {
            status
        };
        let app = Self {
            view: initial_view,
            settings,
            local_servers,
            selected_local_server,
            server_selection: 0,
            server_scroll: 0,
            settings_selection: 0,
            settings_scroll: 0,
            rpc,
            rpc_connect_rx: None,
            launcher: None,
            catalog_core: false,
            launch_rx: None,
            snapshot: None,
            traffic: TrafficHistory::new(),
            last_refresh: Instant::now() - Duration::from_secs(2),
            last_connect_attempt: Instant::now() - Duration::from_secs(5),
            launching: false,
            status,
            error: catalog_error,
            quit: false,
            confirm_quit: false,
            probe_table,
            help_open: false,
            filter: None,
            filter_active: false,
            editing: None,
            editing_text: String::new(),
            confirm_stop: false,
            auto_restart_count: 0,
        };
        app
    }

    // ------------------------------------------------------------------
    // Core lifecycle (same semantics as the window front-end)
    // ------------------------------------------------------------------

    fn start_catalog_core(&mut self) {
        if self.launching || self.launcher.is_some() {
            return;
        }
        self.auto_restart_count = 0;
        let args = catalog_core_args(&self.settings);
        self.launch_core_with_args(args, View::Servers, true);
    }

    fn start_direct_core(&mut self) {
        if self.launching || self.launcher.is_some() {
            return;
        }
        self.auto_restart_count = 0;
        let args = self.core_args();
        self.launch_core_with_args(args, View::Overview, false);
    }

    fn launch_core_with_args(&mut self, args: Vec<String>, view: View, catalog_core: bool) {
        let working_dir = crate::core::settings::working_directory(&self.settings);
        let core_path = self.settings.core_path.clone().map(|path| {
            let path = std::path::PathBuf::from(path);
            if path.is_absolute() {
                path
            } else {
                working_dir.join(path)
            }
        });
        let (tx, rx) = channel();
        self.launch_rx = Some(rx);
        self.launching = true;
        self.catalog_core = catalog_core;
        self.status = "starting core...".to_string();
        self.view = view;
        std::thread::spawn(move || {
            let result = match core_path {
                Some(path) => Launcher::spawn_in(&path, &args, &working_dir),
                None => Launcher::spawn_embedded_in(&args, &working_dir),
            }
            .map_err(|error| format!("{error:#}"));
            let _ = tx.send(result);
        });
    }

    fn stop_core(&mut self) {
        // An attached CLI must only disconnect. An owned core gets the
        // shared graceful-shutdown path, which waits for the RPC request to
        // be accepted before Launcher waits for Dispose() to finish.
        let owns_core = self.launcher.is_some();
        if let Some(rpc) = self.rpc.as_mut() {
            rpc.disconnect();
        }
        if let Some(launcher) = self.launcher.as_mut() {
            self.status = "core stopping (restoring network)...".to_string();
            let _ = launcher.request_graceful_shutdown();
        }

        if let Some(mut launcher) = self.launcher.take() {
            launcher.stop();
        }
        self.launch_rx = None;
        self.launching = false;
        self.catalog_core = false;
        self.rpc = None;
        self.rpc_connect_rx = None;
        self.snapshot = None;
        self.traffic.reset();
        self.status = if owns_core {
            "core stopped".to_string()
        } else {
            "disconnected from core".to_string()
        };
        // The core is gone; a late console-close event must not try again.
        set_emergency_target(None);
    }

    /// TCP-probe latency label for a local server profile.
    fn probe_status_text(&self, profile: &LocalServerProfile) -> String {
        let Ok(table) = self.probe_table.lock() else {
            return "pending".to_string();
        };
        match table.state(&profile.name) {
            Some(ProbeState::Ok(rtt)) => format!("{rtt}ms"),
            Some(ProbeState::Unreachable) => "unreachable".to_string(),
            Some(ProbeState::Probing) => "probing".to_string(),
            Some(ProbeState::Pending) | None => "pending".to_string(),
        }
    }

    fn is_editing(&self) -> bool {
        self.editing.is_some()
    }

    fn is_filtering(&self) -> bool {
        self.filter_active
    }

    /// Enter edit mode on settings field `index` (text fields only).
    fn begin_editing(&mut self, index: usize) {
        if !matches!(index, 2..=51) {
            return;
        }
        let current = match index {
            2 => self.settings.mode.clone(),
            3 => self.settings.config_path.clone(),
            4 => self.settings.server_dir.clone(),
            5 => self.settings.bypass_mode.clone(),
            6 => self.settings.tun_ip.clone(),
            7 => self.settings.tun_gw.clone(),
            8 => self.settings.tun_mask.clone(),
            9 => self.settings.tun_mux.clone(),
            10 => self.settings.tun_mux_acceleration.clone(),
            11 => self.settings.link_restart.clone(),
            12 => self.settings.proxy_http_port.clone(),
            13 => self.settings.proxy_socks_port.clone(),
            14 => self.settings.bypass_file.clone(),
            15 => self.settings.bypass6_file.clone(),
            16 => self.settings.dns_rules_file.clone(),
            17 => self.settings.geo_rules_file.clone(),
            18 => self.settings.geosite_file.clone(),
            19 => self.settings.geoip_file.clone(),
            20 => self.settings.log_file.clone(),
            21 => self.settings.tui_log_file.clone(),
            22 => self.settings.settings_file.clone(),
            23 => self.settings.working_dir.clone(),
            24 => self.settings.rpc_address.clone(),
            25 => self.settings.rpc_token.clone(),
            26 => self.settings.core_path.clone().unwrap_or_default(),
            27 => self.settings.tun_host.to_string(),
            28 => self.settings.tun_vnet.to_string(),
            29 => self.settings.tun_static.to_string(),
            30 => self.settings.tun_flash.to_string(),
            31 => self.settings.block_quic.to_string(),
            33 => self.settings.log_level.clone(),
            34 => self.settings.tcp_ip_cc.clone(),
            36 => self.settings.dns.clone(),
            37 => self.settings.auto_restart.clone(),
            38 => self.settings.firewall_rules.clone(),
            39 => self.settings.nic.clone(),
            40 => self.settings.ngw.clone(),
            41 => self.settings.tun.clone(),
            42 => self.settings.tun_driver.clone(),
            43 => self.settings.tun_ssmt.clone(),
            44 => self.settings.tun_lease_time.clone(),
            45 => self.settings.bypass_nic.clone(),
            46 => self.settings.bypass_ngw.clone(),
            47 => self.settings.bypass_nic6.clone(),
            48 => self.settings.bypass_ngw6.clone(),
            _ => self.settings.command.clone(),
        };
        self.editing = Some(index);
        self.editing_text = current;
        self.status = format!("editing field {} (Esc cancel, Enter/Tab commit)", index + 1);
    }

    /// Commit the editing buffer back into the settings field.
    fn commit_editing(&mut self) {
        let Some(index) = self.editing.take() else {
            return;
        };
        let text = std::mem::take(&mut self.editing_text);
        match index {
            2 => self.settings.mode = text,
            3 => self.settings.config_path = text,
            4 => self.settings.server_dir = text,
            5 => self.settings.bypass_mode = text,
            6 => self.settings.tun_ip = text,
            7 => self.settings.tun_gw = text,
            8 => self.settings.tun_mask = text,
            9 => self.settings.tun_mux = text,
            10 => self.settings.tun_mux_acceleration = text,
            11 => self.settings.link_restart = text,
            12 => self.settings.proxy_http_port = text,
            13 => self.settings.proxy_socks_port = text,
            14 => self.settings.bypass_file = text,
            15 => self.settings.bypass6_file = text,
            16 => self.settings.dns_rules_file = text,
            17 => self.settings.geo_rules_file = text,
            18 => self.settings.geosite_file = text,
            19 => self.settings.geoip_file = text,
            20 => self.settings.log_file = text,
            21 => self.settings.tui_log_file = text,
            22 => self.settings.settings_file = text,
            23 => self.settings.working_dir = text,
            24 => self.settings.rpc_address = text,
            25 => self.settings.rpc_token = text,
            26 => {
                self.settings.core_path = if text.trim().is_empty() {
                    None
                } else {
                    Some(text)
                }
            }
            33 => self.settings.log_level = normalize_log_level(&text),
            34 => self.settings.tcp_ip_cc = normalize_tcp_ip_cc(&text),
            36 => self.settings.dns = text,
            37 => self.settings.auto_restart = text,
            38 => self.settings.firewall_rules = text,
            39 => self.settings.nic = text,
            40 => self.settings.ngw = text,
            41 => self.settings.tun = text,
            42 => self.settings.tun_driver = text,
            43 => self.settings.tun_ssmt = text,
            44 => self.settings.tun_lease_time = text,
            45 => self.settings.bypass_nic = text,
            46 => self.settings.bypass_ngw = text,
            47 => self.settings.bypass_nic6 = text,
            48 => self.settings.bypass_ngw6 = text,
            32 => self.settings.command = text,
            _ => {}
        }
        if index == 33 {
            if let Some(rpc) = self.rpc.as_mut() {
                let _ = rpc.request_command(CoreCommand::SetLogLevel {
                    level: self.settings.log_level.clone(),
                });
            }
        }
        self.status = "settings field updated".to_string();
        self.save_settings();
    }

    /// Handle a raw key while editing a settings field.
    fn handle_editing_key(&mut self, key: crossterm::event::KeyEvent) {
        use crossterm::event::{KeyCode, KeyModifiers};
        if key.kind == crossterm::event::KeyEventKind::Release {
            return;
        }
        let ctrl = key.modifiers.contains(KeyModifiers::CONTROL);
        match key.code {
            KeyCode::Esc => {
                self.editing = None;
                self.editing_text.clear();
                self.status = "edit cancelled".to_string();
            }
            KeyCode::Enter => {
                self.commit_editing();
            }
            KeyCode::Tab => {
                self.commit_editing();
                move_list(
                    settings_field_count(),
                    &mut self.settings_selection,
                    &mut self.settings_scroll,
                    1,
                );
            }
            KeyCode::Backspace => {
                self.editing_text.pop();
            }
            KeyCode::Char(c) if !ctrl => {
                self.editing_text.push(c);
            }
            _ => {}
        }
    }

    /// Handle a raw key while typing a servers-page filter.
    fn handle_filter_key(&mut self, key: crossterm::event::KeyEvent) {
        use crossterm::event::{KeyCode, KeyModifiers};
        if key.kind == crossterm::event::KeyEventKind::Release {
            return;
        }
        let ctrl = key.modifiers.contains(KeyModifiers::CONTROL);
        match key.code {
            KeyCode::Esc | KeyCode::Enter => {
                self.filter_active = false;
                if self.filter.as_deref().map(|f| f.is_empty()).unwrap_or(true) {
                    self.filter = None;
                }
            }
            KeyCode::Backspace => {
                if let Some(filter) = self.filter.as_mut() {
                    filter.pop();
                    if filter.is_empty() {
                        self.filter = None;
                    }
                }
            }
            KeyCode::Char(c) if !ctrl => {
                self.filter.get_or_insert_with(String::new).push(c);
            }
            _ => {}
        }
    }

    /// Global indices into local_servers that pass the active filter.
    fn visible_server_indices(&self) -> Vec<usize> {
        let filter = self.filter.as_deref().unwrap_or("").to_lowercase();
        self.local_servers
            .iter()
            .enumerate()
            .filter(|(_, profile)| {
                filter.is_empty()
                    || profile.name.to_lowercase().contains(&filter)
                    || profile.server.to_lowercase().contains(&filter)
            })
            .map(|(index, _)| index)
            .collect()
    }

    /// Outbounds shown by the running core on the Servers page.
    fn visible_runtime_outbounds(&self) -> Vec<usize> {
        let filter = self.filter.as_deref().unwrap_or("").to_lowercase();
        self.snapshot
            .as_ref()
            .map(|snapshot| {
                snapshot
                    .outbounds
                    .iter()
                    .enumerate()
                    .filter(|(_, outbound)| {
                        outbound.server_menu || outbound.tag.eq_ignore_ascii_case("main")
                    })
                    .filter(|(_, outbound)| {
                        filter.is_empty()
                            || outbound.tag.to_lowercase().contains(&filter)
                            || outbound.display_name.to_lowercase().contains(&filter)
                            || outbound.server.to_lowercase().contains(&filter)
                    })
                    .map(|(index, _)| index)
                    .collect()
            })
            .unwrap_or_default()
    }

    fn server_item_count(&self) -> usize {
        if self.snapshot.is_some() && !self.catalog_core {
            self.visible_runtime_outbounds().len()
        } else {
            self.visible_server_indices().len()
        }
    }

    fn selected_runtime_outbound(&self) -> Option<Outbound> {
        if self.catalog_core {
            return None;
        }
        let visible = self.visible_runtime_outbounds();
        let index = *visible.get(self.server_selection)?;
        self.snapshot.as_ref()?.outbounds.get(index).cloned()
    }

    fn request_switch(&mut self, outbound: &Outbound) {
        if let Some(rpc) = self.rpc.as_mut() {
            let command = CoreCommand::Switch {
                tag: outbound.tag.clone(),
                ranked_first: outbound.active,
            };
            match rpc.request_command(command) {
                Ok(()) => {
                    self.status = format!("switching to {}", outbound.display_name);
                }
                Err(error) => self.error = Some(format!("switch request failed: {error:#}")),
            }
        }
    }

    /// Build the full core argument list from the settings, mirroring the
    /// window client's prepared_core_args: bypass mode and its files, TUN
    /// options, proxy ports and the log file are all forwarded so the
    /// runtime summary matches the configured split mode.
    fn core_args(&self) -> Vec<String> {
        prepared_core_args(&self.settings)
    }

    fn restart_core(&mut self) {
        let rpc_address = self.settings.rpc_address.clone();
        let was_attached = !rpc_address.trim().is_empty();
        self.stop_core();
        if was_attached {
            self.rpc = Some(RpcClient::new(
                rpc_address.trim().to_string(),
                self.settings.rpc_token.trim().to_string(),
            ));
        } else {
            self.start_catalog_core();
        }
    }

    fn save_settings(&mut self) {
        match self.settings.save() {
            Ok(()) => self.status = format!("settings saved to {}", self.settings.settings_file),
            Err(error) => self.error = Some(format!("save settings failed: {error:#}")),
        }
    }

    // ------------------------------------------------------------------
    // RPC polling (batch frame consumption, 1s snapshot refresh)
    // ------------------------------------------------------------------

    fn poll_core(&mut self) {
        if let Some(rx) = &self.launch_rx {
            match rx.try_recv() {
                Ok(result) => {
                    self.launch_rx = None;
                    self.launching = false;
                    match result {
                        Ok(launcher) => {
                            self.status = format!("core started · RPC {}", launcher.endpoint);
                            let endpoint = launcher.endpoint.clone();
                            let token = launcher.token.clone();
                            let pid = launcher.pid();
                            self.rpc = Some(RpcClient::new(endpoint.clone(), token.clone()));
                            set_emergency_target(Some((endpoint, token, pid)));
                            self.launcher = Some(launcher);
                            self.error = None;
                        }
                        Err(error) => {
                            self.status = "core start failed".to_string();
                            self.error = Some(error);
                        }
                    }
                }
                Err(std::sync::mpsc::TryRecvError::Disconnected) => {
                    self.launch_rx = None;
                    self.launching = false;
                    self.status = "core launcher thread exited".to_string();
                }
                Err(std::sync::mpsc::TryRecvError::Empty) => {}
            }
        }

        // rpc.connect() performs a blocking TCP connect (up to 3s); running
        // it on the render loop freezes the overview refresh whenever the
        // core is unreachable.  Attempt it off the loop and attach the
        // stream asynchronously (same pattern as the window client).
        // 1) trigger the background connect when needed.
        if let Some(rpc) = self.rpc.as_mut() {
            if !rpc.is_connected() && self.last_connect_attempt.elapsed() >= Duration::from_secs(2)
            {
                self.last_connect_attempt = Instant::now();
                self.rpc_connect_rx = Some(rpc_connect_background(rpc.address().to_string()));
            }
        }
        // 2) attach the finished stream (separate scope to avoid a second
        //    mutable borrow of self.rpc while the poll borrow is active).
        if let Some(rx) = self.rpc_connect_rx.take() {
            match rx.try_recv() {
                Ok(Ok(stream)) => match self.rpc.as_mut() {
                    Some(rpc) => match rpc.attach_stream(stream) {
                        Ok(()) => self.status = "connecting to core".to_string(),
                        Err(error) => {
                            self.status = "RPC attach failed".to_string();
                            self.error = Some(format!("{error:#}"));
                        }
                    },
                    None => {}
                },
                Ok(Err(error)) => {
                    self.status = "RPC connect failed".to_string();
                    self.error = Some(format!("{error:#}"));
                }
                Err(std::sync::mpsc::TryRecvError::Empty) => {
                    self.rpc_connect_rx = Some(rx);
                }
                Err(std::sync::mpsc::TryRecvError::Disconnected) => {
                    self.status = "RPC connect thread exited".to_string();
                }
            }
        }
        // 3) poll frames.
        let mut frames = Vec::new();
        if let Some(rpc) = self.rpc.as_mut() {
            for _ in 0..64 {
                match rpc.poll() {
                    Ok(Some(response)) => frames.push(response),
                    Ok(None) => break,
                    Err(error) => {
                        self.status = "RPC channel error".to_string();
                        self.error = Some(format!("{error:#}"));
                        break;
                    }
                }
            }
        }

        for response in frames {
            self.handle_response(response);
        }

        // Keep the CLI usable when the owned core crashes or exits early,
        // matching the window client's bounded automatic relaunch behavior.
        let core_exited = self
            .launcher
            .as_mut()
            .and_then(|launcher| launcher.has_exited())
            .is_some();
        if core_exited && !self.launching {
            let view = self.view;
            let was_catalog = self.catalog_core;
            self.launcher.take();
            self.rpc = None;
            self.rpc_connect_rx = None;
            self.catalog_core = false;
            self.snapshot = None;
            self.traffic.reset();
            set_emergency_target(None);
            self.auto_restart_count = self.auto_restart_count.saturating_add(1);
            if self.auto_restart_count <= 3 {
                self.status = format!("core exited; restarting ({}/3)...", self.auto_restart_count);
                if was_catalog {
                    let args = catalog_core_args(&self.settings);
                    self.launch_core_with_args(args, View::Servers, true);
                } else {
                    let args = self.core_args();
                    self.launch_core_with_args(args, view, false);
                }
            } else {
                self.status = "core exited repeatedly; press p to start".to_string();
                self.error = Some("core stopped after 3 automatic restart attempts".to_string());
                self.auto_restart_count = 0;
            }
        }

        if let Some(rpc) = self.rpc.as_mut() {
            if rpc.is_authenticated()
                && !rpc.has_pending()
                && self.last_refresh.elapsed() >= Duration::from_secs(1)
            {
                let _ = rpc.request_command(CoreCommand::GetSnapshot);
                self.last_refresh = Instant::now();
            }
        }
    }

    fn handle_response(&mut self, response: Response) {
        match response {
            Response::Result { method, value, .. } => match method.as_str() {
                "hello" => {
                    self.error = None;
                    self.status = "connected to core".to_string();
                }
                "get_snapshot" | "get_outbounds" => {
                    if let Ok(snapshot) = serde_json::from_value::<Snapshot>(value) {
                        let phase = snapshot.phase.clone();
                        self.traffic.feed(
                            snapshot.traffic.in_bytes,
                            snapshot.traffic.out_bytes,
                            snapshot.monotonic_ms,
                        );
                        self.snapshot = Some(snapshot);
                        self.status = format!("phase={phase}");
                        self.error = None;
                    }
                }
                "get_logs" => {}
                "switch_server" | "switch_rank1" => {
                    let accepted = value
                        .get("accepted")
                        .and_then(|v| v.as_bool())
                        .unwrap_or(false);
                    self.status = if accepted {
                        format!("switch({method}) accepted")
                    } else {
                        "switch rejected by core".to_string()
                    };
                }
                _ => {}
            },
            Response::Event { kind, .. } => match kind.as_str() {
                "log" => {}
                _ => {}
            },
            Response::Error { code, message, .. } => {
                self.status = format!("RPC error {code}");
                self.error = Some(message);
                if code == 401 {
                    if let Some(rpc) = self.rpc.as_mut() {
                        rpc.disconnect();
                    }
                }
            }
        }
    }

    // ------------------------------------------------------------------
    // Actions
    // ------------------------------------------------------------------

    fn on_action(&mut self, action: Action) {
        match action {
            Action::Quit => {
                if self.confirm_quit {
                    self.quit = true;
                } else {
                    self.confirm_quit = true;
                    self.status = "press q/Ctrl+C again to quit".to_string();
                }
            }
            Action::SwitchView(view) => {
                self.view = view;
                self.confirm_quit = false;
            }
            Action::MoveSelection(delta) => match self.view {
                View::Servers => {
                    let count = self.server_item_count();
                    move_list(
                        count,
                        &mut self.server_selection,
                        &mut self.server_scroll,
                        delta,
                    );
                }
                View::Settings => {
                    move_list(
                        settings_field_count(),
                        &mut self.settings_selection,
                        &mut self.settings_scroll,
                        delta,
                    );
                }
                _ => {}
            },
            Action::PageScroll(delta) => match self.view {
                View::Servers => {
                    let count = self.server_item_count();
                    scroll_list(count, &mut self.server_scroll, delta);
                }
                View::Settings => {
                    scroll_list(settings_field_count(), &mut self.settings_scroll, delta);
                }
                _ => {}
            },
            Action::JumpEnd(end) => match self.view {
                View::Servers => {
                    let count = self.server_item_count();
                    self.server_selection = if end { count.saturating_sub(1) } else { 0 };
                    self.server_scroll = self.server_selection;
                }
                _ => {}
            },
            Action::Activate => match self.view {
                View::Servers => {
                    if self.snapshot.is_some() && !self.catalog_core {
                        if let Some(outbound) = self.selected_runtime_outbound() {
                            self.request_switch(&outbound);
                        }
                    } else {
                        self.activate_server_selection();
                    }
                }
                View::Settings => self.activate_settings_selection(),
                View::Overview => {
                    if self.launcher.is_none() && !self.launching {
                        if let Some(index) = self.selected_local_server {
                            self.activate_server_selection_at(index);
                        } else {
                            self.start_catalog_core();
                        }
                    }
                }
                _ => {}
            },
            Action::Toggle => match self.view {
                View::Overview => self.toggle_tun_enabled(),
                View::Settings => match self.settings_selection {
                    0 => {
                        self.settings.tun_enabled = !self.settings.tun_enabled;
                        self.save_settings();
                    }
                    1 => {
                        self.settings.system_proxy_enabled = !self.settings.system_proxy_enabled;
                        self.save_settings();
                    }
                    27 => {
                        self.settings.tun_host = !self.settings.tun_host;
                        self.save_settings();
                    }
                    28 => {
                        self.settings.tun_vnet = !self.settings.tun_vnet;
                        self.save_settings();
                    }
                    29 => {
                        self.settings.tun_static = !self.settings.tun_static;
                        self.save_settings();
                    }
                    30 => {
                        self.settings.tun_flash = !self.settings.tun_flash;
                        self.save_settings();
                    }
                    31 => {
                        self.settings.block_quic = !self.settings.block_quic;
                        self.save_settings();
                    }
                    35 => {
                        self.settings.rt = !self.settings.rt;
                        self.save_settings();
                    }
                    49 => {
                        self.settings.tun_promisc = !self.settings.tun_promisc;
                        self.save_settings();
                    }
                    50 => {
                        self.settings.tun_route = !self.settings.tun_route;
                        self.save_settings();
                    }
                    51 => {
                        self.settings.tun_protect = !self.settings.tun_protect;
                        self.save_settings();
                    }
                    _ => {}
                },
                _ => {}
            },
            Action::Save => {
                if self.view == View::Settings {
                    self.save_settings();
                }
            }
            Action::Restart => {
                if matches!(self.view, View::Settings | View::Routes) && self.launcher.is_some() {
                    self.restart_core();
                }
            }
            Action::StopCore => {
                if self.launcher.is_none() && !self.launching {
                    self.status = "core is not running".to_string();
                    self.confirm_stop = false;
                } else if self.confirm_stop {
                    self.confirm_stop = false;
                    self.stop_core();
                    self.status = "core stopped (p to start)".to_string();
                } else {
                    self.confirm_stop = true;
                    self.status = "press o again to stop the core".to_string();
                }
            }
            Action::StartCore => {
                if self.launcher.is_none() && !self.launching {
                    self.start_catalog_core();
                }
            }
            Action::BypassMode(mode) => {
                if self.view == View::Routes {
                    self.settings.bypass_mode = mode.to_string();
                    self.status = format!("bypass mode set to {mode}");
                }
            }
            Action::Help => {
                self.help_open = !self.help_open;
                self.confirm_quit = false;
            }
            Action::StartFilter => {
                if self.view == View::Servers {
                    self.filter = Some(String::new());
                    self.filter_active = true;
                }
            }
            Action::Cancel => {
                self.confirm_quit = false;
                self.confirm_stop = false;
                self.help_open = false;
            }
        }
    }

    fn activate_server_selection_at(&mut self, index: usize) {
        if self.local_servers.is_empty() {
            return;
        }
        let index = index.min(self.local_servers.len() - 1);
        let profile = &self.local_servers[index];
        self.selected_local_server = Some(index);
        let working_dir = crate::core::settings::working_directory(&self.settings);
        self.settings.config_path =
            crate::core::settings::core_path_string(&working_dir, &profile.path);
        self.settings.server_dir = profile
            .path
            .parent()
            .map(|p| p.to_string_lossy().to_string())
            .unwrap_or_default();
        if self.launcher.is_some() {
            self.status = format!("applying server {}...", profile.name);
            if self.catalog_core {
                self.stop_core();
                let args = self.core_args();
                self.launch_core_with_args(args, View::Overview, false);
            } else {
                self.restart_core();
            }
        } else if !self.launching {
            let args = self.core_args();
            self.launch_core_with_args(args, View::Overview, false);
        }
    }

    fn activate_server_selection(&mut self) {
        let visible = self.visible_server_indices();
        if visible.is_empty() {
            return;
        }
        let row = self.server_selection.min(visible.len() - 1);
        self.activate_server_selection_at(visible[row]);
    }

    fn activate_settings_selection(&mut self) {
        // Toggle boolean fields; Enter on a text field enters edit mode.
        match self.settings_selection {
            0 => {
                self.settings.tun_enabled = !self.settings.tun_enabled;
                self.save_settings();
            }
            1 => {
                self.settings.system_proxy_enabled = !self.settings.system_proxy_enabled;
                self.save_settings();
            }
            27 => {
                self.settings.tun_host = !self.settings.tun_host;
                self.save_settings();
            }
            28 => {
                self.settings.tun_vnet = !self.settings.tun_vnet;
                self.save_settings();
            }
            29 => {
                self.settings.tun_static = !self.settings.tun_static;
                self.save_settings();
            }
            30 => {
                self.settings.tun_flash = !self.settings.tun_flash;
                self.save_settings();
            }
            31 => {
                self.settings.block_quic = !self.settings.block_quic;
                self.save_settings();
            }
            35 => {
                self.settings.rt = !self.settings.rt;
                self.save_settings();
            }
            49 => {
                self.settings.tun_promisc = !self.settings.tun_promisc;
                self.save_settings();
            }
            50 => {
                self.settings.tun_route = !self.settings.tun_route;
                self.save_settings();
            }
            51 => {
                self.settings.tun_protect = !self.settings.tun_protect;
                self.save_settings();
            }
            index => self.begin_editing(index),
        }
    }

    fn toggle_tun_enabled(&mut self) {
        self.settings.tun_enabled = !self.settings.tun_enabled;
    }
}

/// Construct the core command line from the same settings contract used by
/// the window client. Preserve advanced command-line options, then replace
/// only arguments owned by the TUI so split-mode changes cannot leave stale
/// TUN/proxy values behind.
fn catalog_core_args(settings: &StartupSettings) -> Vec<String> {
    let mut args = normalize_core_args(split_command_line(&settings.command));
    for name in [
        "--headless",
        "--rpc-listen",
        "--rpc-token",
        "--rpc-max-clients",
        "--log-level",
        "--tui-log",
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
    set_optional_command_argument(&mut args, "--server-dir", &settings.server_dir);
    set_command_argument(&mut args, "--proxy-http-port", "0");
    set_command_argument(&mut args, "--proxy-socks-port", "0");
    set_command_argument(&mut args, "--catalog-only", "yes");
    set_bool_if_non_default(&mut args, "--rt", settings.rt, true);
    set_optional_if_not_default(&mut args, "--tun-mux", &settings.tun_mux, "");
    set_optional_if_not_default(
        &mut args,
        "--tun-mux-acceleration",
        &settings.tun_mux_acceleration,
        "",
    );
    set_optional_if_not_default(
        &mut args,
        "--log-file",
        &settings.log_file,
        "./ppp-core.log",
    );
    set_command_argument(&mut args, "--log-level", &settings.log_level);
    args
}

fn prepared_core_args(settings: &StartupSettings) -> Vec<String> {
    let mut args = normalize_core_args(split_command_line(&settings.command));
    for name in [
        "--headless",
        "--rpc-listen",
        "--rpc-token",
        "--rpc-max-clients",
        "--catalog-only",
        "--log-level",
        "--tui-log",
        "--lwip",
    ] {
        remove_command_argument(&mut args, name);
    }

    let configured_mode = normalized_launch_mode(&settings.mode);
    // The GUI treats client + TUN disabled as proxy-only. Keep that same
    // effective mode here while leaving the setting editable as "client".
    let mode = if configured_mode == "client" && !settings.tun_enabled {
        "proxy"
    } else {
        configured_mode
    };
    set_command_argument(&mut args, "--mode", mode);
    set_optional_command_argument(&mut args, "--config", &settings.config_path);
    set_optional_command_argument(&mut args, "--server-dir", &settings.server_dir);
    set_bool_if_non_default(&mut args, "--rt", settings.rt, true);
    set_optional_command_argument(&mut args, "--dns", &settings.dns);
    set_optional_command_argument(&mut args, "--auto-restart", &settings.auto_restart);
    if mode == "server" {
        set_optional_command_argument(&mut args, "--firewall-rules", &settings.firewall_rules);
    } else {
        remove_command_argument(&mut args, "--firewall-rules");
    }

    remove_command_argument(&mut args, "--set-http-proxy");
    if settings.system_proxy_enabled && matches!(mode, "client" | "proxy") {
        set_command_argument(&mut args, "--set-http-proxy", "yes");
    }

    let tun_active = mode == "client" && settings.tun_enabled;
    remove_command_argument(&mut args, "--lwip");
    if tun_active {
        match normalize_tcp_ip_cc(&settings.tcp_ip_cc).as_str() {
            "lwip" => set_command_argument(&mut args, "--lwip", "yes"),
            "ctcp" => set_command_argument(&mut args, "--lwip", "no"),
            _ => {}
        }
    }
    if tun_active {
        set_optional_command_argument(&mut args, "--nic", &settings.nic);
        set_optional_command_argument(&mut args, "--ngw", &settings.ngw);
        set_optional_command_argument(&mut args, "--tun", &settings.tun);
        set_optional_command_argument(&mut args, "--tun-ip", &settings.tun_ip);
        set_optional_command_argument(&mut args, "--tun-gw", &settings.tun_gw);
        set_optional_command_argument(&mut args, "--tun-mask", &settings.tun_mask);
        set_bool_if_non_default(&mut args, "--tun-host", settings.tun_host, true);
        set_bool_if_non_default(&mut args, "--tun-vnet", settings.tun_vnet, true);
        set_bool_if_non_default(&mut args, "--tun-static", settings.tun_static, false);
        set_bool_if_non_default(&mut args, "--tun-flash", settings.tun_flash, false);
        set_bool_if_non_default(&mut args, "--block-quic", settings.block_quic, false);
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
        set_optional_if_not_default(&mut args, "--tun-driver", &settings.tun_driver, "auto");
        set_optional_command_argument(
            &mut args,
            "--tun-lease-time-in-seconds",
            &settings.tun_lease_time,
        );
    } else {
        remove_command_argument(&mut args, "--tun-driver");
        remove_command_argument(&mut args, "--tun-lease-time-in-seconds");
    }
    if tun_active && cfg!(any(target_os = "linux", target_os = "macos")) {
        set_optional_command_argument(&mut args, "--tun-ssmt", &settings.tun_ssmt);
        set_bool_if_non_default(&mut args, "--tun-promisc", settings.tun_promisc, true);
    } else {
        remove_command_argument(&mut args, "--tun-ssmt");
        remove_command_argument(&mut args, "--tun-promisc");
    }
    if tun_active && cfg!(target_os = "linux") {
        set_bool_if_non_default(&mut args, "--tun-route", settings.tun_route, false);
        set_bool_if_non_default(&mut args, "--tun-protect", settings.tun_protect, true);
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
        set_optional_command_argument(&mut args, "--tun-mux", &settings.tun_mux);
        set_optional_command_argument(
            &mut args,
            "--tun-mux-acceleration",
            &settings.tun_mux_acceleration,
        );
        set_optional_command_argument(&mut args, "--link-restart", &settings.link_restart);
    }

    if tun_active {
        set_optional_command_argument(&mut args, "--proxy-http-port", &settings.proxy_http_port);
        set_optional_command_argument(&mut args, "--proxy-socks-port", &settings.proxy_socks_port);
    } else if mode == "proxy" {
        set_command_argument(&mut args, "--proxy-http-port", "0");
        set_command_argument(&mut args, "--proxy-socks-port", "0");
    } else {
        remove_command_argument(&mut args, "--proxy-http-port");
        remove_command_argument(&mut args, "--proxy-socks-port");
    }

    let bypass_mode = settings.bypass_mode.trim().to_ascii_lowercase();
    remove_command_argument(&mut args, "--bypass-mode");
    if !bypass_mode.is_empty() && bypass_mode != "ip" {
        set_command_argument(&mut args, "--bypass-mode", &bypass_mode);
    }
    if mode == "server" {
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
    } else {
        match bypass_mode.as_str() {
            "geo" => {
                remove_command_argument(&mut args, "--bypass");
                remove_command_argument(&mut args, "--bypass6");
                remove_command_argument(&mut args, "--dns-rules");
                set_optional_command_argument(&mut args, "--geo-rules", &settings.geo_rules_file);
                set_optional_command_argument(&mut args, "--geosite", &settings.geosite_file);
                set_optional_command_argument(&mut args, "--geoip", &settings.geoip_file);
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
                remove_command_argument(&mut args, "--geo-rules");
                remove_command_argument(&mut args, "--geosite");
                remove_command_argument(&mut args, "--geoip");
                set_optional_command_argument(&mut args, "--bypass", &settings.bypass_file);
                set_optional_command_argument(&mut args, "--bypass6", &settings.bypass6_file);
                set_optional_command_argument(&mut args, "--dns-rules", &settings.dns_rules_file);
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
        set_optional_command_argument(&mut args, "--bypass-ngw", &settings.bypass_ngw);
        set_optional_command_argument(&mut args, "--bypass-ngw6", &settings.bypass_ngw6);
        if cfg!(target_os = "linux") {
            set_optional_command_argument(&mut args, "--bypass-nic", &settings.bypass_nic);
            set_optional_command_argument(&mut args, "--bypass-nic6", &settings.bypass_nic6);
        } else {
            remove_command_argument(&mut args, "--bypass-nic");
            remove_command_argument(&mut args, "--bypass-nic6");
        }
    }
    set_optional_if_not_default(
        &mut args,
        "--log-file",
        &settings.log_file,
        "./ppp-core.log",
    );
    set_command_argument(&mut args, "--log-level", &settings.log_level);
    args
}

/// Perform the blocking TCP connect off the render loop.
fn rpc_connect_background(address: String) -> Receiver<anyhow::Result<std::net::TcpStream>> {
    let (tx, rx) = channel();
    std::thread::spawn(move || {
        let result =
            RpcClient::connect_socket(&address).map_err(|error| anyhow::anyhow!("{error:#}"));
        let _ = tx.send(result);
    });
    rx
}

/// Record the core this process currently owns so the Windows console-close
/// watchdog can reach it after this process is told to die.
fn set_emergency_target(target: Option<(String, String, u32)>) {
    #[cfg(windows)]
    ctrl::set_target(target);
    #[cfg(not(windows))]
    let _ = target;
}

fn move_list(count: usize, selection: &mut usize, scroll: &mut usize, delta: i64) {
    if count == 0 {
        return;
    }
    let len = count as i64;
    *selection = ((((*selection as i64 + delta) % len) + len) % len) as usize;
    // Keep the focused row visible: adjust scroll so selection is inside
    // the current viewport.
    ensure_visible(selection, scroll, 12);
}

fn scroll_list(count: usize, scroll: &mut usize, delta: i64) {
    let max = count.saturating_sub(12);
    *scroll = ((*scroll as i64 + delta).max(0).min(max as i64)) as usize;
}

fn ensure_visible(selection: &usize, scroll: &mut usize, viewport: usize) {
    if *selection < *scroll {
        *scroll = *selection;
    } else if *selection >= *scroll + viewport {
        *scroll = *selection + 1 - viewport;
    }
}

fn settings_field_count() -> usize {
    52
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

fn draw(frame: &mut ratatui::Frame, app: &TerminalApp) {
    clear_frame(frame);
    let chunks = Layout::default()
        .direction(Direction::Vertical)
        .constraints([
            Constraint::Length(1),
            Constraint::Min(1),
            Constraint::Length(1),
            Constraint::Length(1),
        ])
        .split(frame.area());

    draw_top_bar(frame, chunks[0], app);

    // Body: left navigation + 1-column padding + page content.
    let body = Layout::default()
        .direction(Direction::Horizontal)
        .constraints([
            Constraint::Length(14),
            Constraint::Length(1),
            Constraint::Min(1),
        ])
        .split(chunks[1]);
    draw_nav(frame, body[0], app);
    let content_area = body[2];
    match app.view {
        View::Overview => draw_overview(frame, content_area, app),
        View::Network => draw_network(frame, content_area, app),
        View::Servers => draw_servers(frame, content_area, app),
        View::Routes => draw_routes(frame, content_area, app),
        View::Settings => draw_settings(frame, content_area, app),
    }

    draw_status_bar(frame, chunks[2], app);
    draw_hints_bar(frame, chunks[3], app);

    if app.help_open {
        let popup_area = centered_rect(56, 78, frame.area());
        render_popup(
            frame,
            popup_area,
            "Help",
            vec![
                Line::from(""),
                Line::from(" 1-5 / Tab          switch page"),
                Line::from(" Up/Down / j k      move selection"),
                Line::from(" Ctrl+U / Ctrl+D    half-page scroll"),
                Line::from(" Home / G           first / last"),
                Line::from(" Enter              activate / edit"),
                Line::from(" Space              toggle (TUN / proxy)"),
                Line::from(" /                  filter servers"),
                Line::from(" p / Enter          start core"),
                Line::from(" o                  stop core (confirm twice)"),
                Line::from(" s                  save settings (Settings)"),
                Line::from(" r                  save + restart (Settings/Routes)"),
                Line::from(" i / g / n          bypass mode (Routes)"),
                Line::from(" Esc                cancel / leave filter"),
                Line::from(" q / Ctrl+C         quit (press twice)"),
                Line::from(" ?                  this help"),
                Line::from(""),
                Line::from("  All keys are tmux-safe: no F-key or Ctrl+"),
                Line::from("  prefix/flow-control collisions. Quit uses"),
                Line::from("  a double confirmation before stopping."),
                Line::from(""),
                Line::from("            [Enter / Esc / ?] close"),
            ],
        );
    }
}

fn view_index(view: View) -> usize {
    match view {
        View::Overview => 0,
        View::Network => 1,
        View::Servers => 2,
        View::Routes => 3,
        View::Settings => 4,
    }
}

/// Top status bar: title + connection state + active server latency + rate
/// + primary action (mirrors the window client's top bar).
fn draw_top_bar(frame: &mut ratatui::Frame, area: Rect, app: &TerminalApp) {
    let (state_label, state_color) = if let Some(snapshot) = &app.snapshot {
        match snapshot.phase.as_str() {
            "connected" => ("* connected ", ratatui::style::Color::Green),
            "connecting" | "reconnecting" => ("~ connecting", ratatui::style::Color::Yellow),
            "failed" => ("x failed   ", ratatui::style::Color::Red),
            _ => ("~ pending  ", ratatui::style::Color::Yellow),
        }
    } else if app.launching {
        ("~ starting ", ratatui::style::Color::Yellow)
    } else if app.launcher.is_some() {
        ("~ connecting", ratatui::style::Color::Yellow)
    } else {
        ("o idle     ", ratatui::style::Color::DarkGray)
    };

    let mut spans = vec![
        Span::styled(
            " PPP PRIVATE NETWORK 2 ",
            Style::default().add_modifier(Modifier::BOLD),
        ),
        Span::styled(state_label, Style::default().fg(state_color)),
    ];

    if let Some(snapshot) = &app.snapshot {
        spans.push(Span::raw(format!(" {}", snapshot.server)));
    } else if let Some(index) = app.selected_local_server {
        if let Some(profile) = app.local_servers.get(index) {
            spans.push(Span::raw(format!(
                " {} {}",
                profile.name,
                app.probe_status_text(profile)
            )));
        }
    }

    if let Some(latest) = app.traffic.latest() {
        spans.push(Span::raw(format!(
            "  down {}  up {}",
            format_rate(latest.rx_bytes_per_sec),
            format_rate(latest.tx_bytes_per_sec)
        )));
    }

    // Primary action hint on the right (Enter starts the core).
    if app.launcher.is_none() {
        spans.push(Span::styled(
            "  Enter:start ",
            Style::default().fg(ratatui::style::Color::Green),
        ));
    }
    frame.render_widget(Paragraph::new(Line::from(spans)), area);
}

/// Left navigation rail drawn as a bordered card; the active page row is
/// filled with the ACCENT highlight (select-where-you-are).
fn draw_nav(frame: &mut ratatui::Frame, area: Rect, app: &TerminalApp) {
    let titles = [
        ("1", "Overview"),
        ("2", "Network"),
        ("3", "Servers"),
        ("4", "Routes"),
        ("5", "Settings"),
    ];
    let mut lines = vec![Line::from(Span::styled(
        "┌─ ppp-tui ──┐",
        Style::default().fg(ratatui::style::Color::DarkGray),
    ))];
    for (index, (key, title)) in titles.iter().enumerate() {
        let active = index == view_index(app.view);
        let content = format!("│ {key} {title:<9}│");
        if active {
            // Filled ACCENT background so the current page is obvious.
            lines.push(Line::from(Span::styled(content, selected_style())));
        } else {
            lines.push(Line::from(Span::styled(
                content,
                Style::default().fg(ratatui::style::Color::DarkGray),
            )));
        }
    }
    lines.push(Line::from(Span::styled(
        "└────────────┘",
        Style::default().fg(ratatui::style::Color::DarkGray),
    )));
    lines.push(Line::from(""));
    lines.push(Line::from(Span::styled(
        " 1-5 switch",
        Style::default().fg(ratatui::style::Color::DarkGray),
    )));
    frame.render_widget(Paragraph::new(lines), area);
}

/// Bottom status line: error (red) + status (yellow), full width.
fn draw_status_bar(frame: &mut ratatui::Frame, area: Rect, app: &TerminalApp) {
    let mut spans = Vec::new();
    if let Some(error) = &app.error {
        spans.push(Span::styled(
            format!(" {error} "),
            Style::default().fg(ratatui::style::Color::Red),
        ));
    }
    spans.push(Span::styled(
        format!(" {}", app.status),
        Style::default().fg(ratatui::style::Color::Yellow),
    ));
    frame.render_widget(Paragraph::new(Line::from(spans)), area);
}

/// Bottom hints line: context-sensitive key hints.
fn draw_hints_bar(frame: &mut ratatui::Frame, area: Rect, app: &TerminalApp) {
    let hints = match app.view {
        View::Overview => " 1-5 page | p/Enter start | o stop | q quit ",
        View::Servers => {
            if app.filter_active {
                " typing filter... Esc/Enter done "
            } else {
                " 1-5 page | Up/Down move | Enter switch | p start | o stop | / filter "
            }
        }
        View::Settings => {
            if app.is_editing() {
                " editing field... Esc cancel | Enter/Tab commit "
            } else {
                " 1-5 page | Up/Down move | Enter edit | Space toggle | s save | r restart "
            }
        }
        View::Routes => " 1-5 page | i ip | g geo | n none | s save | r restart ",
        View::Network => " 1-5 page | p/Enter start | o stop | q quit ",
    };
    frame.render_widget(
        Paragraph::new(Line::from(Span::styled(
            hints,
            Style::default().fg(ratatui::style::Color::DarkGray),
        ))),
        area,
    );
}

/// Clear the whole frame first so page switches never leave stale cells
/// from a taller previous page (ratatui diffs, so this is flicker-free).
fn clear_frame(frame: &mut ratatui::Frame) {
    let area = frame.area();
    let buffer = frame.buffer_mut();
    for y in 0..area.height {
        for x in 0..area.width {
            let cell = &mut buffer[(x, y)];
            cell.reset();
            cell.set_symbol(" ");
        }
    }
}

/// Truncate a value so long lines (IPv6 URIs, GUIDs) cannot wrap or push
/// the second column out of the terminal.
fn trunc(value: &str, max: usize) -> String {
    if value.chars().count() <= max {
        value.to_string()
    } else {
        let cut: String = value.chars().take(max.saturating_sub(1)).collect();
        format!("{cut}…")
    }
}

/// Two-column card: `| label value .... label value` doubles the density
/// of the plain ascii_card while staying ASCII-safe and wrap-free.
fn two_col_card(
    title: &str,
    rows: &[(&str, String, &str, String)],
    width: usize,
) -> Vec<Line<'static>> {
    // Rule-based like every other section: no vertical borders.  The value
    // columns adapt to the available width so wide terminals show full
    // IPv6 addresses/GUIDs instead of truncating at a fixed 27 chars.
    let mut lines = vec![Line::from(rule_line(title, width))];
    // " label  value  label  value" -> fixed labels take 2*10+3.
    let value_max = width.saturating_sub(25).max(8) / 2;
    for (l1, v1, l2, v2) in rows {
        lines.push(Line::from(format!(
            " {l1:<9} {:<value_max$} {l2:<9} {}",
            trunc(v1, value_max),
            trunc(v2, value_max),
        )));
    }
    lines
}

fn centered_rect(percent_x: u16, percent_y: u16, area: Rect) -> Rect {
    let vertical = Layout::default()
        .direction(Direction::Vertical)
        .constraints([
            Constraint::Percentage((100 - percent_y) / 2),
            Constraint::Percentage(percent_y),
            Constraint::Percentage((100 - percent_y) / 2),
        ])
        .split(area);
    Layout::default()
        .direction(Direction::Horizontal)
        .constraints([
            Constraint::Percentage((100 - percent_x) / 2),
            Constraint::Percentage(percent_x),
            Constraint::Percentage((100 - percent_x) / 2),
        ])
        .split(vertical[1])[1]
}

fn render_popup(frame: &mut ratatui::Frame, area: Rect, title: &str, lines: Vec<Line<'static>>) {
    frame.render_widget(ratatui::widgets::Clear, area);
    frame.render_widget(
        Paragraph::new(lines).block(
            Block::default()
                .borders(Borders::ALL)
                .title(format!(" {title} ")),
        ),
        area,
    );
}

fn draw_overview(frame: &mut ratatui::Frame, area: Rect, app: &TerminalApp) {
    // Idle: show the server latency preview (mirrors the window client
    // overview page before a core starts).  No VPN is started.
    if app.launcher.is_none() && app.snapshot.is_none() {
        let mut lines = vec![Line::styled(
            rule_line("Server Latency (TCP probe, no VPN)", 60),
            Style::default().add_modifier(Modifier::BOLD),
        )];
        lines.push(Line::from(""));
        for (index, profile) in app.local_servers.iter().enumerate() {
            let latency = app.probe_status_text(profile);
            let color = match latency.as_str() {
                "unreachable" => ratatui::style::Color::Red,
                "pending" | "probing" => ratatui::style::Color::DarkGray,
                _ => {
                    let rtt = latency.trim_end_matches("ms").parse::<i64>().unwrap_or(999);
                    if rtt < 100 {
                        ratatui::style::Color::Green
                    } else if rtt < 300 {
                        ratatui::style::Color::Yellow
                    } else {
                        ratatui::style::Color::Red
                    }
                }
            };
            let selected = app.selected_local_server == Some(index);
            let marker = if selected { ">" } else { " " };
            let row_style = if selected {
                selected_style()
            } else {
                Style::default()
            };
            lines.push(Line::from(vec![
                Span::styled(format!(" {marker} "), row_style),
                Span::styled(
                    profile.name.clone(),
                    if selected {
                        Style::default()
                            .fg(ratatui::style::Color::Black)
                            .bg(ratatui::style::Color::Rgb(86, 166, 255))
                            .add_modifier(Modifier::BOLD)
                    } else {
                        Style::default().add_modifier(Modifier::BOLD)
                    },
                ),
                Span::styled(format!("  {}", profile.server), row_style),
                Span::styled(
                    format!("  [{latency}]"),
                    if selected {
                        Style::default().fg(ratatui::style::Color::Black)
                    } else {
                        Style::default().fg(color)
                    },
                ),
            ]));
        }
        if app.local_servers.is_empty() {
            lines.push(Line::from("  (no server configurations found)"));
        }
        lines.push(Line::from(""));
        lines.push(Line::from(Span::styled(
            " Enter: start selected server ",
            Style::default().fg(ratatui::style::Color::DarkGray),
        )));
        frame.render_widget(Paragraph::new(lines), area);
        return;
    }

    let chunks = Layout::default()
        .direction(Direction::Vertical)
        .constraints([
            Constraint::Length(3),
            Constraint::Length(10),
            Constraint::Min(6),
        ])
        .split(area);

    // Connection controls first (mirrors the GUI layout order).
    let tun_state = if app.settings.tun_enabled {
        "[x]"
    } else {
        "[ ]"
    };
    let proxy_state = if app.settings.system_proxy_enabled {
        "[x]"
    } else {
        "[ ]"
    };
    let core_state = if app.launcher.is_some() {
        "running"
    } else {
        "Enter start"
    };
    frame.render_widget(
        Paragraph::new(Line::from(format!(
            " {tun_state} TUN VPN    {proxy_state} System Proxy    <{core_state}>"
        ))),
        chunks[0],
    );

    // Dense two-column connection info (same fields as the GUI).
    let mut rows: Vec<(&str, String, &str, String)> = Vec::new();
    if let Some(snapshot) = &app.snapshot {
        rows.push((
            "State",
            snapshot.phase.clone(),
            "Server",
            snapshot.server.clone(),
        ));
        let active_outbound = snapshot
            .outbounds
            .iter()
            .find(|outbound| outbound.active)
            .map(|outbound| outbound.display_name.clone())
            .unwrap_or_else(|| "-".to_string());
        rows.push(("Main", active_outbound, "GUID", snapshot.guid.clone()));
        rows.push((
            "Transport",
            snapshot.transport.clone(),
            "Bypass",
            snapshot.bypass_mode.clone(),
        ));
        rows.push((
            "Http",
            snapshot.http_proxy.clone(),
            "Socks",
            snapshot.socks_proxy.clone(),
        ));
        rows.push((
            "Hosting",
            snapshot.role.clone(),
            "Time",
            format_duration_short(snapshot.duration_ms),
        ));
        rows.push((
            "MUX",
            format!(
                "{} · {} links",
                snapshot.effective_mux_mode, snapshot.mux_active_links
            ),
            "Total",
            format!(
                "down {} up {}",
                format_bytes(snapshot.traffic.in_bytes),
                format_bytes(snapshot.traffic.out_bytes)
            ),
        ));
        if let Some(latest) = app.traffic.latest() {
            rows.push((
                "Down",
                format_rate(latest.rx_bytes_per_sec),
                "Up",
                format_rate(latest.tx_bytes_per_sec),
            ));
        }
    }
    let card = two_col_card("Connection Information", &rows, chunks[1].width as usize);
    frame.render_widget(Paragraph::new(card), chunks[1]);

    // Traffic charts: rx and tx stacked top-to-bottom, each filling the
    // content width (2-char columns), height levels of solid blocks.
    let rx: Vec<u64> = app
        .traffic
        .samples()
        .iter()
        .map(|p| p.rx_bytes_per_sec)
        .collect();
    let tx: Vec<u64> = app
        .traffic
        .samples()
        .iter()
        .map(|p| p.tx_bytes_per_sec)
        .collect();
    // Proportional sizing: the chart fills the content width and its
    // height scales with the available space (rx and tx share it), so a
    // fullscreen terminal is used instead of leaving big empty areas.
    // Bars keep a fixed 2-char pitch.
    let chart_width = chunks[2].width as usize;
    let chart_height = ((chunks[2].height as usize).saturating_sub(5) / 2).clamp(3, 8);
    let cols = chart_width.saturating_sub(4) / 2;
    let rx_rows = bar_chart_columns(&rx, cols, chart_height);
    let tx_rows = bar_chart_columns(&tx, cols, chart_height);
    // Colour the chart rows exactly like the window client's traffic
    // graphs: rx in GOOD green, tx in ACCENT blue (truecolor; terminals
    // without colour support fall back to plain text).
    let rx_color = ratatui::style::Color::Rgb(74, 196, 124);
    let tx_color = ratatui::style::Color::Rgb(86, 166, 255);
    let mut chart: Vec<Line<'static>> = vec![Line::from(rule_line("Traffic", chart_width))];
    chart.push(Line::from(Span::styled(
        " rx",
        Style::default().fg(rx_color),
    )));
    for row in 0..chart_height {
        chart.push(Line::from(Span::styled(
            format!("  {}", rx_rows.get(row).cloned().unwrap_or_default()),
            Style::default().fg(rx_color),
        )));
    }
    chart.push(Line::from(Span::styled(
        " tx",
        Style::default().fg(tx_color),
    )));
    for row in 0..chart_height {
        chart.push(Line::from(Span::styled(
            format!("  {}", tx_rows.get(row).cloned().unwrap_or_default()),
            Style::default().fg(tx_color),
        )));
    }
    frame.render_widget(Paragraph::new(chart), chunks[2]);
}

/// HH:MM:SS from a monotonic millisecond duration.
fn format_duration_short(duration_ms: u64) -> String {
    let total_seconds = duration_ms / 1000;
    format!(
        "{:02}:{:02}:{:02}",
        total_seconds / 3600,
        (total_seconds / 60) % 60,
        total_seconds % 60
    )
}

fn draw_network(frame: &mut ratatui::Frame, area: Rect, app: &TerminalApp) {
    let Some(snapshot) = &app.snapshot else {
        frame.render_widget(
            Paragraph::new(Line::from(
                " start the core to see TUN / NIC / proxy / MUX state",
            )),
            area,
        );
        return;
    };
    let mut lines: Vec<Line> = Vec::new();
    let network = &snapshot.network;

    // Proxy-only TUNNEL card (same rows as the GUI).
    if network.mode == "proxy-only" {
        let rows = vec![
            ("Mode".to_string(), dash(&network.mode)),
            ("Adapter".to_string(), dash(&network.adapter)),
            ("Logical IPv4".to_string(), dash(&network.logical_ipv4)),
            ("Logical IPv6".to_string(), dash(&network.logical_ipv6)),
            ("Tunnel DNS".to_string(), dash(&network.tunnel_dns)),
            ("Link State".to_string(), dash(&network.link_state)),
            ("Mux State".to_string(), dash(&network.mux_state)),
            (
                "TCP/IP Transport".to_string(),
                dash(&network.tcp_ip_transport),
            ),
            ("DNS Transport".to_string(), dash(&network.dns_transport)),
        ];
        lines.extend(plain_card("TUNNEL", &rows, area.width as usize));
        lines.push(Line::from(""));
    }

    if let Some(tun) = &network.tun {
        lines.extend(network_interface_card("TUN", tun, true, network));
        lines.push(Line::from(""));
    }
    if let Some(nic) = &network.nic {
        lines.extend(network_interface_card("NIC", nic, false, network));
    }
    if network.tun.is_none() && network.nic.is_none() {
        lines.push(Line::from(" waiting for network information..."));
    }
    frame.render_widget(Paragraph::new(lines), area);
}

/// Selected-row highlight: ACCENT blue background + black bold text,
/// matching the window client's selected card/nav colour.
fn selected_style() -> Style {
    Style::default()
        .fg(ratatui::style::Color::Black)
        .bg(ratatui::style::Color::Rgb(86, 166, 255))
        .add_modifier(Modifier::BOLD)
}

fn dash(value: &str) -> String {
    if value.is_empty() {
        "-".to_string()
    } else {
        value.to_string()
    }
}

/// Interface card with the same rows as the GUI network page.
fn network_interface_card(
    title: &str,
    interface: &crate::rpc::schema::NetworkInterface,
    tun: bool,
    network: &crate::rpc::schema::Network,
) -> Vec<Line<'static>> {
    let name = if interface.description.is_empty() {
        interface.name.clone()
    } else {
        format!("{}[{}]", interface.name, interface.description)
    };
    let interface_text = format!(
        "{} {} {}",
        dash(&interface.ipv4),
        dash(&interface.gateway),
        dash(&interface.subnet_mask)
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
        let ipv6 = vec![
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
            ("Aggligator".to_string(), dash(&network.aggligator)),
            (
                "Proxy Interlayer".to_string(),
                dash(&network.proxy_interlayer),
            ),
            ("TCP/IP CC".to_string(), dash(&network.tcp_ip_cc)),
            ("Block QUIC".to_string(), dash(&network.block_quic)),
            ("Mux State".to_string(), dash(&network.mux_state)),
            ("Link State".to_string(), dash(&network.link_state)),
        ]);
    } else if !interface.ipv6_gateway.is_empty() {
        rows.push(("Interface IPv6".to_string(), interface.ipv6_gateway.clone()));
    }
    for (index, dns) in interface.dns.iter().enumerate() {
        rows.push((format!("DNS Server {}", index + 1), dns.clone()));
    }
    let mut lines = vec![Line::from(rule_line(title, 60))];
    for (label, value) in rows {
        lines.push(Line::from(format!(" {label:<22} {value}")));
    }
    lines
}

fn draw_runtime_servers(frame: &mut ratatui::Frame, area: Rect, app: &TerminalApp) {
    let chunks = Layout::default()
        .direction(Direction::Vertical)
        .constraints([Constraint::Length(3), Constraint::Min(1)])
        .split(area);
    let filter = app.filter.as_deref().unwrap_or("").to_lowercase();
    let visible = app.visible_runtime_outbounds();
    let total = app
        .snapshot
        .as_ref()
        .map(|snapshot| {
            snapshot
                .outbounds
                .iter()
                .filter(|outbound| {
                    outbound.server_menu || outbound.tag.eq_ignore_ascii_case("main")
                })
                .count()
        })
        .unwrap_or(0);
    let title = if filter.is_empty() {
        format!(" Servers ({total}) ")
    } else {
        format!(" Servers filter: {filter} ")
    };
    let hint = if app.filter_active {
        " typing filter... Esc/Enter done "
    } else {
        " Up/Down move | Enter switch | / filter | o stop "
    };
    frame.render_widget(
        Paragraph::new(Line::from(Span::styled(
            title,
            Style::default().add_modifier(Modifier::BOLD),
        ))),
        chunks[0],
    );

    let mut lines = Vec::new();
    let viewport = area.height.saturating_sub(6) as usize;
    let start = app.server_scroll.min(visible.len().saturating_sub(1));
    let end = (start + viewport).min(visible.len());
    for (row, &index) in visible.iter().enumerate().skip(start).take(end - start) {
        let Some(snapshot) = &app.snapshot else { break };
        let Some(outbound) = snapshot.outbounds.get(index) else {
            continue;
        };
        let focused = app.server_selection == row;
        let marker = if focused { ">" } else { " " };
        let state = match outbound.state {
            1 => "connected",
            0 => "connecting",
            2 => "reconnecting",
            _ => "unknown",
        };
        let rtt = if outbound.probe_checked && outbound.probe_reachable {
            format!("{}ms", outbound.probe_rtt_ms)
        } else if outbound.probe_checked {
            "unreachable".to_string()
        } else {
            "pending".to_string()
        };
        let name = if outbound.display_name.is_empty() {
            &outbound.tag
        } else {
            &outbound.display_name
        };
        let detail = if outbound.server.is_empty() {
            outbound.current_entry.clone()
        } else {
            outbound.server.clone()
        };
        let text = format!(
            "{marker} {}  {:<22} {:<16} {:<12} [{}]",
            if outbound.active { "*" } else { " " },
            name,
            detail,
            state,
            rtt
        );
        lines.push(Line::from(Span::styled(
            text,
            if focused {
                selected_style()
            } else {
                Style::default()
            },
        )));
    }
    if lines.is_empty() {
        lines.push(Line::from("  (no runtime servers match)"));
    }
    lines.push(Line::from(Span::styled(
        hint,
        Style::default().fg(if app.filter_active {
            ratatui::style::Color::Yellow
        } else {
            ratatui::style::Color::DarkGray
        }),
    )));
    frame.render_widget(Paragraph::new(lines), chunks[1]);
}

fn draw_servers(frame: &mut ratatui::Frame, area: Rect, app: &TerminalApp) {
    if app.snapshot.is_some() && !app.catalog_core {
        draw_runtime_servers(frame, area, app);
        return;
    }
    let chunks = Layout::default()
        .direction(Direction::Vertical)
        .constraints([Constraint::Length(3), Constraint::Min(1)])
        .split(area);

    // Apply the active filter.
    let filter = app.filter.as_deref().unwrap_or("").to_lowercase();
    let visible = app.visible_server_indices();

    let title = if filter.is_empty() {
        format!(" Servers ({}) ", app.local_servers.len())
    } else {
        format!(" Servers filter: {} ", filter)
    };
    let hint = if app.filter_active {
        vec![Line::from(Span::styled(
            " typing filter... Esc/Enter done ",
            Style::default().fg(ratatui::style::Color::Yellow),
        ))]
    } else {
        vec![Line::from(Span::styled(
            " Up/Down move | Enter start | / filter | p start | o stop ",
            Style::default().fg(ratatui::style::Color::DarkGray),
        ))]
    };
    frame.render_widget(
        Paragraph::new(Line::from(Span::styled(
            title,
            Style::default().add_modifier(Modifier::BOLD),
        ))),
        chunks[0],
    );

    let mut lines = Vec::new();
    let viewport = area.height.saturating_sub(6) as usize;
    let start = app.server_scroll.min(visible.len().saturating_sub(1));
    let end = (start + viewport).min(visible.len());
    for (row, &index) in visible.iter().enumerate().skip(start).take(end - start) {
        let profile = &app.local_servers[index];
        let selected = app.selected_local_server == Some(index);
        let focused = app.server_selection == row;
        let marker = if focused { ">" } else { " " };
        let star = if selected { "*" } else { " " };
        let latency = app.probe_status_text(profile);
        let color = match latency.as_str() {
            "unreachable" => ratatui::style::Color::Red,
            "pending" | "probing" => ratatui::style::Color::DarkGray,
            _ => {
                let rtt = latency.trim_end_matches("ms").parse::<i64>().unwrap_or(999);
                if rtt < 100 {
                    ratatui::style::Color::Green
                } else if rtt < 300 {
                    ratatui::style::Color::Yellow
                } else {
                    ratatui::style::Color::Red
                }
            }
        };
        let row_style = if focused {
            selected_style()
        } else {
            Style::default()
        };
        lines.push(Line::from(vec![
            Span::styled(format!("{marker} {star} "), row_style),
            Span::styled(
                profile.name.clone(),
                if focused {
                    Style::default()
                        .fg(ratatui::style::Color::Black)
                        .bg(ratatui::style::Color::Rgb(86, 166, 255))
                        .add_modifier(Modifier::BOLD)
                } else {
                    Style::default()
                },
            ),
            Span::styled(format!("  {}", profile.server), row_style),
            Span::styled(
                format!("   [{latency}]"),
                if focused {
                    Style::default().fg(ratatui::style::Color::Black)
                } else {
                    Style::default().fg(color)
                },
            ),
        ]));
    }
    if lines.is_empty() {
        lines.push(Line::from("  (no servers match)"));
    }
    lines.push(Line::from(""));
    lines.extend(hint);
    frame.render_widget(Paragraph::new(lines), chunks[1]);
}

#[cfg(test)]
mod tests {
    use super::*;

    fn has_arg(args: &[String], expected: &str) -> bool {
        args.iter().any(|arg| arg == expected)
    }

    #[test]
    fn proxy_effective_mode_clears_tun_and_sets_zero_ports() {
        let mut settings = StartupSettings::default();
        settings.command =
            "--mode=client --headless --extra-flag=yes --tun-ip=10.0.0.1 --proxy-http-port=1080"
                .to_string();
        settings.tun_enabled = false;
        let args = prepared_core_args(&settings);

        assert!(has_arg(&args, "--mode=proxy"));
        assert!(has_arg(&args, "--extra-flag=yes"));
        assert!(has_arg(&args, "--proxy-http-port=0"));
        assert!(has_arg(&args, "--proxy-socks-port=0"));
        assert!(!args.iter().any(|arg| arg.starts_with("--tun-ip")));
        assert!(!has_arg(&args, "--headless"));
    }

    #[test]
    fn client_mode_forwards_tun_and_bypass_settings() {
        let mut settings = StartupSettings::default();
        settings.command = "--mode=proxy --bypass=stale.txt --geo-rules=stale.json".to_string();
        settings.tun_enabled = true;
        settings.bypass_mode = "geo".to_string();
        settings.geo_rules_file = "rules.json".to_string();
        settings.geosite_file = "site.dat".to_string();
        settings.geoip_file = "ip.dat".to_string();
        let args = prepared_core_args(&settings);

        assert!(has_arg(&args, "--mode=client"));
        assert!(args.iter().any(|arg| arg == "--geo-rules=rules.json"));
        assert!(has_arg(&args, "--geosite=site.dat"));
        assert!(has_arg(&args, "--geoip=ip.dat"));
        assert!(!has_arg(&args, "--bypass=stale.txt"));
        assert!(!args.iter().any(|arg| arg.starts_with("--tun-host")));
    }

    #[test]
    fn catalog_mode_is_proxy_only_and_keeps_server_directory() {
        let mut settings = StartupSettings::default();
        settings.command =
            "--mode=client --config=main.json --tun-ip=10.0.0.1 --catalog-only=no".to_string();
        settings.server_dir = "config".to_string();
        let args = catalog_core_args(&settings);

        assert!(has_arg(&args, "--mode=proxy"));
        assert!(has_arg(&args, "--server-dir=config"));
        assert!(has_arg(&args, "--catalog-only=yes"));
        assert!(has_arg(&args, "--proxy-http-port=0"));
        assert!(has_arg(&args, "--proxy-socks-port=0"));
        assert!(!args.iter().any(|arg| arg.starts_with("--config")));
        assert!(!args.iter().any(|arg| arg.starts_with("--tun-ip")));
    }

    #[test]
    fn tcp_ip_cc_setting_maps_to_lwip_switch() {
        let mut settings = StartupSettings::default();
        settings.command = "--mode=client --lwip=yes".to_string();
        settings.tun_enabled = true;

        // auto removes a stale explicit switch and lets the core select its
        // platform/driver-specific default.
        settings.tcp_ip_cc = "auto".to_string();
        let args = prepared_core_args(&settings);
        assert!(!args.iter().any(|arg| arg.starts_with("--lwip")));

        settings.tcp_ip_cc = "lwip".to_string();
        let args = prepared_core_args(&settings);
        assert!(has_arg(&args, "--lwip=yes"));

        settings.tcp_ip_cc = "ctcp".to_string();
        let args = prepared_core_args(&settings);
        assert!(has_arg(&args, "--lwip=no"));
    }

    #[test]
    fn structured_core_options_are_forwarded_with_platform_scope() {
        let mut settings = StartupSettings::default();
        settings.rt = false;
        settings.dns = "1.1.1.1,8.8.8.8".to_string();
        settings.auto_restart = "3600".to_string();
        settings.firewall_rules = "server-rules.txt".to_string();
        settings.nic = "Ethernet".to_string();
        settings.ngw = "192.168.1.1".to_string();
        settings.tun = "OpenPPP2".to_string();
        settings.tun_driver = "wintun".to_string();
        settings.tun_ssmt = "4/mq".to_string();
        settings.tun_lease_time = "7200".to_string();
        settings.bypass_nic = "eth0".to_string();
        settings.bypass_ngw = "192.168.1.1".to_string();
        settings.bypass_nic6 = "eth0".to_string();
        settings.bypass_ngw6 = "fe80::1".to_string();
        settings.tun_promisc = false;
        settings.tun_route = true;
        settings.tun_protect = false;

        let args = prepared_core_args(&settings);
        assert!(has_arg(&args, "--rt=no"));
        assert!(has_arg(&args, "--dns=1.1.1.1,8.8.8.8"));
        assert!(has_arg(&args, "--auto-restart=3600"));
        assert!(has_arg(&args, "--nic=Ethernet"));
        assert!(has_arg(&args, "--ngw=192.168.1.1"));
        assert!(has_arg(&args, "--tun=OpenPPP2"));
        assert!(has_arg(&args, "--bypass-ngw=192.168.1.1"));
        assert!(has_arg(&args, "--bypass-ngw6=fe80::1"));

        if cfg!(target_os = "windows") {
            assert!(has_arg(&args, "--tun-driver=wintun"));
            assert!(has_arg(&args, "--tun-lease-time-in-seconds=7200"));
            assert!(!has_arg(&args, "--tun-ssmt=4/mq"));
            assert!(!has_arg(&args, "--tun-promisc=no"));
            assert!(!has_arg(&args, "--tun-route=yes"));
            assert!(!has_arg(&args, "--tun-protect=no"));
            assert!(!has_arg(&args, "--bypass-nic=eth0"));
            assert!(!has_arg(&args, "--bypass-nic6=eth0"));
        } else {
            assert!(!has_arg(&args, "--tun-driver=wintun"));
            assert!(!has_arg(&args, "--tun-lease-time-in-seconds=7200"));
        }

        if cfg!(any(target_os = "linux", target_os = "macos")) {
            assert!(has_arg(&args, "--tun-ssmt=4/mq"));
            assert!(has_arg(&args, "--tun-promisc=no"));
        }
        if cfg!(target_os = "linux") {
            assert!(has_arg(&args, "--tun-route=yes"));
            assert!(has_arg(&args, "--tun-protect=no"));
            assert!(has_arg(&args, "--bypass-nic=eth0"));
            assert!(has_arg(&args, "--bypass-nic6=eth0"));
        }

        settings.mode = "server".to_string();
        settings.tun_enabled = false;
        let args = prepared_core_args(&settings);
        assert!(has_arg(&args, "--firewall-rules=server-rules.txt"));
        assert!(!args.iter().any(|arg| arg.starts_with("--nic")));
        assert!(!args.iter().any(|arg| arg.starts_with("--tun=")));
        assert!(!args.iter().any(|arg| arg.starts_with("--bypass-ngw")));
    }
}

// ---------------------------------------------------------------------------
// Windows console-close watchdog
// ---------------------------------------------------------------------------
//
// Closing the console window (X), Ctrl+Break, logoff or system shutdown
// terminates this process without running `stop_core`; the kill-on-close
// Job Object would then force-kill the core before it restores Windows
// DNS/routes, leaving the host without working DNS.  The OS grants a
// console app a few seconds on close, so this handler first delivers an
// RPC `shutdown` over a fresh connection and then keeps the process alive
// (its Job Object stays open) until the core finishes restoring and exits
// by itself.
#[cfg(windows)]
mod ctrl {
    use crate::core::launcher::request_graceful_shutdown;
    use std::sync::atomic::{AtomicBool, Ordering};
    use std::sync::Mutex;

    const CTRL_C_EVENT: u32 = 0;
    const CTRL_BREAK_EVENT: u32 = 1;
    const CTRL_CLOSE_EVENT: u32 = 2;
    const CTRL_LOGOFF_EVENT: u32 = 5;
    const CTRL_SHUTDOWN_EVENT: u32 = 6;

    /// The core currently owned by this process (endpoint, token, PID).
    static TARGET: Mutex<Option<(String, String, u32)>> = Mutex::new(None);
    static STOPPING: AtomicBool = AtomicBool::new(false);

    pub fn set_target(target: Option<(String, String, u32)>) {
        *TARGET.lock().unwrap_or_else(|poison| poison.into_inner()) = target;
    }

    #[allow(non_snake_case)]
    unsafe extern "system" fn console_handler(ctrl_type: u32) -> i32 {
        match ctrl_type {
            CTRL_C_EVENT | CTRL_BREAK_EVENT | CTRL_CLOSE_EVENT | CTRL_LOGOFF_EVENT
            | CTRL_SHUTDOWN_EVENT => {
                emergency_shutdown();
                1 // handled
            }
            _ => 0,
        }
    }

    pub fn install() {
        unsafe {
            SetConsoleCtrlHandler(Some(console_handler), 1);
        }
    }

    fn emergency_shutdown() {
        // The handler runs on a fresh thread; it may race the main loop,
        // which is itself stopping the core right now.  Run once.
        if STOPPING.swap(true, Ordering::SeqCst) {
            return;
        }
        let target = {
            TARGET
                .lock()
                .unwrap_or_else(|poison| poison.into_inner())
                .take()
        };
        let Some((endpoint, token, pid)) = target else {
            return;
        };
        // Deliver the shutdown even if the live session was never
        // authenticated (fresh blocking connection with its own hello).
        let _shutdown_acknowledged = request_graceful_shutdown(&endpoint, &token);
        // Even if the acknowledgement is lost because the core closes its
        // RPC session while disposing, it may already be restoring DNS. Never
        // call process::exit immediately after a failed/ambiguous request:
        // the Job Object would kill the core halfway through cleanup.
        // Dispose can take up to 10s waiting for DNS guard workers, so wait
        // before allowing the console process to terminate. (CTRL_C and
        // CTRL_BREAK are not subject to the shorter close-event deadline.)
        wait_for_core_exit(pid, 15000);
        std::process::exit(0);
    }

    fn wait_for_core_exit(pid: u32, timeout_ms: u32) {
        unsafe {
            const SYNCHRONIZE: u32 = 0x0010_0000;
            let handle = OpenProcess(SYNCHRONIZE, 0, pid);
            if handle.is_null() {
                return;
            }
            WaitForSingleObject(handle, timeout_ms);
            CloseHandle(handle);
        }
    }

    extern "system" {
        fn SetConsoleCtrlHandler(
            handler_routine: Option<unsafe extern "system" fn(u32) -> i32>,
            add: i32,
        ) -> i32;
        fn OpenProcess(
            dw_desired_access: u32,
            b_inherit_handle: i32,
            dw_process_id: u32,
        ) -> *mut core::ffi::c_void;
        fn WaitForSingleObject(h_handle: *mut core::ffi::c_void, dw_milliseconds: u32) -> u32;
        fn CloseHandle(h_object: *mut core::ffi::c_void) -> i32;
    }
}

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------

pub fn run(settings: StartupSettings) -> Result<()> {
    std::panic::set_hook(Box::new(|info| {
        let _ = crossterm::terminal::disable_raw_mode();
        let _ = io::stdout().execute(crossterm::event::DisableMouseCapture);
        let _ = io::stdout().execute(LeaveAlternateScreen);
        eprintln!("ppp-tui-cli panic: {info}");
    }));

    // Console-close / Ctrl+Break safety net: closing the console window
    // must not leave the VPN network state behind (the core only restores
    // DNS/routes when it is asked to shut down; a job-object kill cannot).
    #[cfg(windows)]
    ctrl::install();

    enable_raw_mode().context("enable raw mode")?;
    let mut stdout = io::stdout();
    stdout
        .execute(EnterAlternateScreen)
        .context("enter alternate screen")?;
    let backend = CrosstermBackend::new(stdout);
    let mut terminal = Terminal::new(backend).context("create terminal")?;

    // Single-instance guard: only one CLI terminal at a time.
    match std::net::TcpListener::bind(("127.0.0.1", 18992)) {
        Ok(listener) => {
            let _: &'static std::net::TcpListener = Box::leak(Box::new(listener));
        }
        Err(_) => {
            disable_raw_mode().context("disable raw mode")?;
            io::stdout()
                .execute(LeaveAlternateScreen)
                .context("leave alternate screen")?;
            eprintln!("ppp-tui-cli: another terminal instance is already running");
            std::process::exit(1);
        }
    }

    let mut app = TerminalApp::new(settings);
    if app.settings.launch_direct && app.settings.rpc_address.trim().is_empty() {
        app.start_direct_core();
    } else if app.settings.rpc_address.trim().is_empty() {
        app.start_catalog_core();
    }
    let mut events = EventReader::new();

    'outer: loop {
        app.poll_core();

        while let Some(action) = events.next_action() {
            app.on_action(action);
            if app.quit {
                break 'outer;
            }
        }

        terminal.draw(|frame| draw(frame, &app))?;

        if event::poll(Duration::from_millis(100)).context("poll events")? {
            if let Event::Key(key) = event::read().context("read event")? {
                // Raw text entry modes take priority over action keys.
                if app.is_editing() {
                    app.handle_editing_key(key);
                } else if app.is_filtering() {
                    app.handle_filter_key(key);
                } else {
                    events.push(key);
                }
            }
        }
    }

    // Teardown: stop the core we launched, restore the terminal.
    app.stop_core();
    let _ = io::stdout().execute(crossterm::event::DisableMouseCapture);
    disable_raw_mode().context("disable raw mode")?;
    io::stdout()
        .execute(LeaveAlternateScreen)
        .context("leave alternate screen")?;
    Ok(())
}

fn draw_routes(frame: &mut ratatui::Frame, area: Rect, app: &TerminalApp) {
    let mut lines: Vec<Line> = Vec::new();
    let rows = vec![
        ("Bypass Mode".to_string(), app.settings.bypass_mode.clone()),
        ("Config".to_string(), app.settings.config_path.clone()),
        ("Direct DNS".to_string(), "via core".to_string()),
    ];
    lines.extend(plain_card("Routes", &rows, area.width as usize));
    lines.push(Line::from(""));

    // Bypass mode cards (i/g/n keys), mirroring the GUI routes page.
    let mut mode_row = String::new();
    for (value, label) in [("ip", "IP"), ("geo", "GEO"), ("none", "NONE")] {
        let active = app.settings.bypass_mode == value;
        mode_row.push_str(&format!(" [{}] {label} ", if active { "x" } else { " " }));
    }
    lines.push(Line::from(Span::styled(
        mode_row,
        Style::default().fg(ratatui::style::Color::DarkGray),
    )));

    let Some(snapshot) = &app.snapshot else {
        lines.push(Line::from(Span::styled(
            " start the core to see the runtime summary and split rules",
            Style::default().fg(ratatui::style::Color::DarkGray),
        )));
        frame.render_widget(Paragraph::new(lines), area);
        return;
    };

    // Runtime Summary (routes info reported by the core).
    let routes = &snapshot.routes;
    let summary = vec![
        ("Bypass IPv4".to_string(), dash(&routes.bypass_ipv4_file)),
        ("Bypass IPv6".to_string(), dash(&routes.bypass_ipv6_file)),
        ("Gateway".to_string(), dash(&routes.bypass_gateway)),
        (
            "Gateway IPv6".to_string(),
            dash(&routes.bypass_gateway_ipv6),
        ),
        ("DNS Rules".to_string(), {
            let count = routes.dns_rule_count;
            if routes.dns_rules_file.is_empty() {
                "-".to_string()
            } else {
                format!("{} ({count})", trunc(&routes.dns_rules_file, 36))
            }
        }),
        ("Geo Rules".to_string(), trunc(&routes.geo_rules_file, 36)),
        ("Geosite".to_string(), trunc(&routes.geosite_file, 36)),
        ("GeoIP".to_string(), trunc(&routes.geoip_file, 36)),
    ];
    lines.push(Line::from(""));
    lines.extend(plain_card("Runtime Summary", &summary, area.width as usize));

    // Split rules (geo mode).
    lines.push(Line::from(""));
    if snapshot.geo.split_rules.is_empty() {
        lines.push(Line::from(Span::styled(
            " (no split rules in this mode)",
            Style::default().fg(ratatui::style::Color::DarkGray),
        )));
    } else {
        let split: Vec<(String, String)> = snapshot
            .geo
            .split_rules
            .iter()
            .map(|rule| {
                let display = if rule.display.is_empty() {
                    rule.outbound.clone()
                } else {
                    rule.display.clone()
                };
                (rule.matcher.clone(), display)
            })
            .collect();
        lines.extend(plain_card("Split Rules", &split, area.width as usize));
    }
    lines.push(Line::from(""));
    lines.push(Line::from(Span::styled(
        " i ip | g geo | n none | s save (bypass mode)",
        Style::default().fg(ratatui::style::Color::DarkGray),
    )));
    frame.render_widget(Paragraph::new(lines), area);
}

/// Field labels for the Settings page (indices 0..=51, see
/// settings_field_count).  Toggle fields get a [x]/[ ] prefix at render
/// time; text fields show their value after the label.
fn setting_label(index: usize) -> &'static str {
    match index {
        0 => "TUN VPN",
        1 => "System Proxy",
        2 => "Mode",
        3 => "Config",
        4 => "Server Dir",
        5 => "Bypass Mode",
        6 => "TUN IP",
        7 => "TUN GW",
        8 => "TUN Mask",
        9 => "TUN MUX",
        10 => "MUX Acceleration",
        11 => "Link Restart",
        12 => "HTTP Proxy Port",
        13 => "SOCKS Proxy Port",
        14 => "Bypass File",
        15 => "Bypass6 File",
        16 => "DNS Rules File",
        17 => "Geo Rules File",
        18 => "Geosite File",
        19 => "Geoip File",
        20 => "Core Log File",
        21 => "TUI Log File",
        22 => "Settings File",
        23 => "Working Dir",
        24 => "RPC Address",
        25 => "RPC Token",
        26 => "Core Path",
        27 => "TUN Host",
        28 => "TUN VNet",
        29 => "TUN Static",
        30 => "TUN Flash",
        31 => "Block QUIC",
        32 => "Command",
        33 => "Core Log Level",
        34 => "TCP/IP CC",
        35 => "Real-time Mode",
        36 => "DNS Servers",
        37 => "Auto Restart",
        38 => "Firewall Rules",
        39 => "Physical NIC",
        40 => "Physical Gateway",
        41 => "TUN Adapter",
        42 => "TUN Driver",
        43 => "TUN SSMT",
        44 => "TUN Lease Seconds",
        45 => "Bypass NIC",
        46 => "Bypass Gateway",
        47 => "Bypass IPv6 NIC",
        48 => "Bypass IPv6 Gateway",
        49 => "TUN Promisc",
        50 => "TUN Route",
        51 => "TUN Protect",
        _ => "",
    }
}

/// Current value of a text settings field (index 2..=26, 32..=34 and 36..=48); toggle
/// fields are rendered with [x]/[ ] and return an empty value here.
fn settings_value(app: &TerminalApp, index: usize) -> String {
    match index {
        2 => app.settings.mode.clone(),
        3 => app.settings.config_path.clone(),
        4 => app.settings.server_dir.clone(),
        5 => app.settings.bypass_mode.clone(),
        6 => app.settings.tun_ip.clone(),
        7 => app.settings.tun_gw.clone(),
        8 => app.settings.tun_mask.clone(),
        9 => app.settings.tun_mux.clone(),
        10 => app.settings.tun_mux_acceleration.clone(),
        11 => app.settings.link_restart.clone(),
        12 => app.settings.proxy_http_port.clone(),
        13 => app.settings.proxy_socks_port.clone(),
        14 => app.settings.bypass_file.clone(),
        15 => app.settings.bypass6_file.clone(),
        16 => app.settings.dns_rules_file.clone(),
        17 => app.settings.geo_rules_file.clone(),
        18 => app.settings.geosite_file.clone(),
        19 => app.settings.geoip_file.clone(),
        20 => app.settings.log_file.clone(),
        21 => app.settings.tui_log_file.clone(),
        22 => app.settings.settings_file.clone(),
        23 => app.settings.working_dir.clone(),
        24 => app.settings.rpc_address.clone(),
        25 => app.settings.rpc_token.clone(),
        26 => app.settings.core_path.clone().unwrap_or_default(),
        32 => app.settings.command.clone(),
        33 => app.settings.log_level.clone(),
        34 => app.settings.tcp_ip_cc.clone(),
        36 => app.settings.dns.clone(),
        37 => app.settings.auto_restart.clone(),
        38 => app.settings.firewall_rules.clone(),
        39 => app.settings.nic.clone(),
        40 => app.settings.ngw.clone(),
        41 => app.settings.tun.clone(),
        42 => app.settings.tun_driver.clone(),
        43 => app.settings.tun_ssmt.clone(),
        44 => app.settings.tun_lease_time.clone(),
        45 => app.settings.bypass_nic.clone(),
        46 => app.settings.bypass_ngw.clone(),
        47 => app.settings.bypass_nic6.clone(),
        48 => app.settings.bypass_ngw6.clone(),
        _ => String::new(),
    }
}

fn draw_settings(frame: &mut ratatui::Frame, area: Rect, app: &TerminalApp) {
    let mut lines: Vec<Line> = Vec::new();
    lines.push(Line::from(Span::styled(
        format!(" Settings ({}) ", settings_field_count()),
        Style::default().add_modifier(Modifier::BOLD),
    )));

    let width = area.width as usize;
    let value_max = width.saturating_sub(28).max(8);
    let toggle_on = |index: usize| match index {
        0 => app.settings.tun_enabled,
        1 => app.settings.system_proxy_enabled,
        27 => app.settings.tun_host,
        28 => app.settings.tun_vnet,
        29 => app.settings.tun_static,
        30 => app.settings.tun_flash,
        31 => app.settings.block_quic,
        35 => app.settings.rt,
        49 => app.settings.tun_promisc,
        50 => app.settings.tun_route,
        51 => app.settings.tun_protect,
        _ => false,
    };

    let viewport = area.height.saturating_sub(5) as usize;
    let start = app
        .settings_scroll
        .min(settings_field_count().saturating_sub(1));
    let mut rendered = 0usize;
    for index in start..settings_field_count() {
        if rendered >= viewport {
            break;
        }
        let toggle = matches!(index, 0 | 1 | 27 | 28 | 29 | 30 | 31 | 35 | 49 | 50 | 51);
        let focused = app.settings_selection == index;
        let style = if focused {
            selected_style()
        } else {
            Style::default()
        };
        let marker = if focused { "> " } else { "  " };
        let label = setting_label(index);
        let prefix = if toggle {
            let on = toggle_on(index);
            format!("{marker}[{}] ", if on { "x" } else { " " })
        } else {
            format!("{marker}   ")
        };
        if toggle {
            lines.push(Line::from(Span::styled(format!("{prefix}{label}"), style)));
            rendered += 1;
        } else {
            let value = settings_value(app, index);
            if focused && app.editing == Some(index) {
                let text = format!("{prefix}{label} = {}|", app.editing_text);
                lines.push(Line::from(Span::styled(text, style)));
                rendered += 1;
            } else if index == 32 && value.chars().count() > value_max {
                // Command: wrap to multiple lines so long argument lists
                // stay visible instead of being truncated to one row.
                lines.push(Line::from(Span::styled(
                    format!("{prefix}{label} ="),
                    style,
                )));
                rendered += 1;
                let indent = " ".repeat(prefix.chars().count() + label.chars().count() + 3);
                let mut rest = value;
                while !rest.is_empty() && rendered < viewport {
                    let take = rest.chars().count().min(value_max);
                    let chunk: String = rest.chars().take(take).collect();
                    rest = rest.chars().skip(take).collect();
                    let line_style = if focused { style } else { Style::default() };
                    lines.push(Line::from(Span::styled(
                        format!("{indent}{chunk}"),
                        line_style,
                    )));
                    rendered += 1;
                }
            } else {
                let text = format!("{prefix}{label} = {}", trunc(&value, value_max));
                lines.push(Line::from(Span::styled(text, style)));
                rendered += 1;
            }
        }
    }
    lines.push(Line::from(""));
    lines.push(Line::from(Span::styled(
        " Enter edit | Space toggle | s save | r save+restart ",
        Style::default().fg(ratatui::style::Color::DarkGray),
    )));
    frame.render_widget(Paragraph::new(lines), area);
}
