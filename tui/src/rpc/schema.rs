//! Runtime snapshot contract types (docs/RUST_TUI_DESIGN_CN.md §4.4).
//!
//! Field names and semantics mirror the C++ core's
//! `PppApplication::BuildRuntimeSnapshot` output and the Android
//! `get_runtime_snapshot` contract (tests/contracts/runtime-snapshot/).
//! Unknown future fields are tolerated via `#[serde(default)]`; fields are
//! never removed, only added.

use serde::{Deserialize, Serialize};

pub const SCHEMA_VERSION: u64 = 1;

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
#[serde(default)]
pub struct Snapshot {
    pub schema_version: u64,
    pub generation: u64,
    pub monotonic_ms: u64,
    pub phase: String,
    pub role: String,
    pub server: String,
    /// Active client configuration GUID, formatted with braces like C++ TUI.
    pub guid: String,
    /// C++ TUI-compatible remote server display, including static/dynamic
    /// allocation and proxy-only markers.
    pub vpn_server: String,
    pub transport: String,
    pub bypass_mode: String,
    pub http_proxy: String,
    pub socks_proxy: String,
    pub connection: String,
    pub mux_state: String,
    pub hosting_environment: String,
    pub duration_ms: u64,
    pub capabilities: Vec<String>,
    pub requested_mux_mode: String,
    pub effective_mux_mode: String,
    pub mux_receiver_ordering: String,
    pub mux_active_links: u32,
    pub mux_fallback_reason: String,
    pub connected_monotonic_ms: u64,
    pub traffic: Traffic,
    pub network: Network,
    pub routes: RouteInfo,
    pub geo: Geo,
    pub outbounds: Vec<Outbound>,
    pub last_error: LastError,
    pub log_level: String,
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
#[serde(default)]
pub struct Traffic {
    /// Byte delta of the last core OnTick period (~1s window) = current rate.
    pub rx_bytes: u64,
    pub tx_bytes: u64,
    /// Cumulative totals (core-side IN/OUT, mirroring the built-in TUI).
    pub in_bytes: u64,
    pub out_bytes: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
#[serde(default)]
pub struct Network {
    pub mode: String,
    pub adapter: String,
    pub logical_ipv4: String,
    pub logical_ipv6: String,
    pub tunnel_dns: String,
    pub link_state: String,
    pub mux_state: String,
    pub tcp_ip_transport: String,
    pub dns_transport: String,
    pub aggligator: String,
    pub proxy_interlayer: String,
    pub tcp_ip_cc: String,
    pub block_quic: String,
    pub tun: Option<NetworkInterface>,
    pub nic: Option<NetworkInterface>,
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
#[serde(default)]
pub struct NetworkInterface {
    pub name: String,
    pub description: String,
    pub index: i32,
    pub id: String,
    pub ipv4: String,
    pub gateway: String,
    pub subnet_mask: String,
    pub ipv6: String,
    pub ipv6_address: String,
    pub ipv6_gateway: String,
    pub ipv6_subnet_mask: String,
    pub dns: Vec<String>,
}

/// Mode-specific route inputs and counters, mirroring the C++ Routes page.
#[derive(Debug, Clone, Serialize, Deserialize, Default)]
#[serde(default)]
pub struct RouteInfo {
    pub bypass_ipv4_file: String,
    pub bypass_ipv6_file: String,
    pub bypass_gateway: String,
    pub bypass_gateway_ipv6: String,
    pub dns_rules_file: String,
    pub dns_rule_count: u64,
    pub geo_rules_file: String,
    pub geosite_file: String,
    pub geoip_file: String,
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
#[serde(default)]
pub struct Geo {
    pub direct_dns: Vec<String>,
    pub rule_count: u64,
    pub static_networks: u64,
    pub split_rules: Vec<SplitRule>,
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
#[serde(default)]
pub struct SplitRule {
    pub matcher: String,
    pub outbound: String,
    pub display: String,
}

/// One SERVERS-page row; `state` follows the core's NetworkState mapping:
/// 0=connecting, 1=established, 2=reconnecting, -1=unknown.
#[derive(Debug, Clone, Serialize, Deserialize, Default)]
#[serde(default)]
pub struct Outbound {
    pub tag: String,
    pub display_name: String,
    pub server: String,
    pub state: i32,
    pub reconnects: u32,
    pub active: bool,
    pub server_menu: bool,
    pub route_used: bool,
    pub multiple_entries: bool,
    pub probe_enabled: bool,
    pub probe_checked: bool,
    pub probe_reachable: bool,
    pub probe_rtt_ms: i32,
    pub current_entry: String,
    pub ranked_first_entry: String,
    /// Best entry from the latest background probe when not connected.
    pub probe_entry: String,
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
#[serde(default)]
pub struct LastError {
    pub code: i64,
    pub severity: String,
    pub retryable: bool,
    pub user_message_key: String,
    pub diagnostic_detail: String,
}

impl Snapshot {
    /// True when the tunnel is fully established.
    pub fn is_connected(&self) -> bool {
        self.phase == "connected"
    }

    /// True when the tunnel is trying to (re)connect.
    pub fn is_transitioning(&self) -> bool {
        matches!(self.phase.as_str(), "connecting" | "reconnecting")
    }

    /// Rate of the live MUX ordering (display string).
    pub fn ordering_label(&self) -> &str {
        match self.mux_receiver_ordering.as_str() {
            "flow_v2" => "flow-v2",
            other => other,
        }
    }
}
