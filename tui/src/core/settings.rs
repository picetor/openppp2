//! Persistent startup settings shared by the window and terminal front-ends.

use std::fs;
use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};

use crate::core::command::{command_bool, command_value, join_command_args, normalize_core_args};

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
    /// TCP/IP implementation selected by the core: auto, lwip or ctcp.
    /// `auto` omits --lwip and preserves the core/platform default.
    pub tcp_ip_cc: String,
    /// Core startup options that are intentionally kept separate from the
    /// free-form command so the terminal client can edit and round-trip them.
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
            log_file: String::new(),
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
    /// Parse the command line: `--rpc/--token/--ppp/--embedded-core` are TUI
    /// options; everything else is a core launch command (launch_direct).
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

    pub fn load_saved(&mut self) {
        let path = resolve_settings_path(&self.settings_file);
        let Ok(contents) = fs::read_to_string(path) else {
            return;
        };
        let Ok(mut saved) = serde_json::from_str::<StartupSettings>(&contents) else {
            return;
        };
        if !contents.contains("\"tui_log_enabled\"") {
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

    pub fn save(&self) -> anyhow::Result<()> {
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

    pub fn normalize_paths(&mut self) {
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

fn default_tui_log_enabled() -> bool {
    true
}

// ---------------------------------------------------------------------------
// Path helpers
// ---------------------------------------------------------------------------

pub fn resolve_settings_path(value: &str) -> PathBuf {
    let path = PathBuf::from(value.trim());
    if path.is_absolute() {
        path
    } else {
        std::env::current_dir()
            .unwrap_or_else(|_| PathBuf::from("."))
            .join(path)
    }
}

pub fn normalize_user_path(value: &str) -> String {
    let value = value.trim().strip_prefix("\\\\?\\").unwrap_or(value.trim());
    value.replace('\\', "/")
}

pub fn normalize_path_field(value: &mut String) {
    let normalized = normalize_user_path(value);
    *value = normalized;
}

pub fn path_to_forward_slashes(path: &Path) -> String {
    normalize_user_path(&path.to_string_lossy())
}

pub fn display_path_string(path: &Path) -> String {
    path_to_forward_slashes(&normalize_display_path(path))
}

pub fn normalize_display_path(path: &Path) -> PathBuf {
    let mut normalized = PathBuf::new();
    for component in path.components() {
        match component {
            std::path::Component::CurDir => {}
            std::path::Component::ParentDir => {
                normalized.push("..");
            }
            other => normalized.push(other.as_os_str()),
        }
    }
    if normalized.as_os_str().is_empty() {
        PathBuf::from(".")
    } else {
        normalized
    }
}

pub fn normalized_launch_mode(value: &str) -> &'static str {
    match value.trim().to_ascii_lowercase().as_str() {
        "proxy" | "proxy-only" | "proxy_only" => "proxy",
        "server" => "server",
        _ => "client",
    }
}

pub fn normalize_log_level(value: &str) -> String {
    match value.trim().to_ascii_lowercase().as_str() {
        "none" | "off" => "none".to_string(),
        "warn" | "warning" => "warn".to_string(),
        "info" => "info".to_string(),
        "debug" => "debug".to_string(),
        _ => "error".to_string(),
    }
}

/// Normalize the UI representation of the core's `--lwip` switch.
///
/// `auto` is deliberately supported so a saved TUI setting does not change
/// the platform/driver-specific default.  The core itself receives yes/no
/// only when the user explicitly selects lwip/ctcp.
pub fn normalize_tcp_ip_cc(value: &str) -> String {
    match value.trim().to_ascii_lowercase().as_str() {
        "lwip" | "yes" | "true" | "1" => "lwip".to_string(),
        "ctcp" | "no" | "false" | "0" => "ctcp".to_string(),
        _ => "auto".to_string(),
    }
}

pub fn working_directory(settings: &StartupSettings) -> PathBuf {
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

pub fn resolve_from_working_dir(base: &Path, value: &str) -> PathBuf {
    let path = PathBuf::from(value.trim());
    if path.is_absolute() {
        path
    } else {
        base.join(path)
    }
}

pub fn core_path_string(base: &Path, path: &Path) -> String {
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

pub fn comparable_path(path: &Path) -> String {
    fs::canonicalize(path)
        .unwrap_or_else(|_| path.to_path_buf())
        .to_string_lossy()
        .to_ascii_lowercase()
}
