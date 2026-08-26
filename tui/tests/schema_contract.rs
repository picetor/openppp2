//! Golden contract tests: the Rust schema must parse every runtime-snapshot
//! fixture produced for the Android UI (tests/contracts/runtime-snapshot/).
//! The fixtures are embedded at compile time, so these tests run offline.

use ppp_tui::rpc::schema::Snapshot;

// The fixtures live one directory above the crate.
const FIXTURES: [(&str, &str); 5] = [
    (
        "idle",
        include_str!("../../tests/contracts/runtime-snapshot/idle.json"),
    ),
    (
        "connected",
        include_str!("../../tests/contracts/runtime-snapshot/connected.json"),
    ),
    (
        "failed",
        include_str!("../../tests/contracts/runtime-snapshot/failed.json"),
    ),
    (
        "reconnecting",
        include_str!("../../tests/contracts/runtime-snapshot/reconnecting.json"),
    ),
    (
        "unsupported-schema",
        include_str!("../../tests/contracts/runtime-snapshot/unsupported-schema.json"),
    ),
];

#[test]
fn all_fixtures_parse() {
    for (name, json) in FIXTURES {
        let snapshot: Snapshot = serde_json::from_str(json)
            .unwrap_or_else(|e| panic!("fixture '{name}' failed to parse: {e}"));
        assert!(
            snapshot.schema_version >= 1,
            "fixture '{name}' must carry schema_version"
        );
    }
}

#[test]
fn connected_fixture_phase() {
    let snapshot: Snapshot = serde_json::from_str(include_str!(
        "../../tests/contracts/runtime-snapshot/connected.json"
    ))
    .expect("connected.json parses");
    assert_eq!(snapshot.phase, "connected");
    assert!(snapshot.is_connected());
    assert_eq!(snapshot.transport, "wss");
    assert_eq!(snapshot.traffic.rx_bytes, 10_485_760);
    assert_eq!(snapshot.mux_active_links, 2);
    assert_eq!(snapshot.effective_mux_mode, "flow");
}

#[test]
fn idle_fixture_phase() {
    let snapshot: Snapshot = serde_json::from_str(include_str!(
        "../../tests/contracts/runtime-snapshot/idle.json"
    ))
    .expect("idle.json parses");
    assert_eq!(snapshot.phase, "idle");
    assert!(!snapshot.is_connected());
}

#[test]
fn reconnecting_fixture_phase() {
    let snapshot: Snapshot = serde_json::from_str(include_str!(
        "../../tests/contracts/runtime-snapshot/reconnecting.json"
    ))
    .expect("reconnecting.json parses");
    assert_eq!(snapshot.phase, "reconnecting");
    assert!(snapshot.is_transitioning());
}

#[test]
fn failed_fixture_phase() {
    let snapshot: Snapshot = serde_json::from_str(include_str!(
        "../../tests/contracts/runtime-snapshot/failed.json"
    ))
    .expect("failed.json parses");
    assert_eq!(snapshot.phase, "failed");
    assert!(!snapshot.is_connected());
    assert!(!snapshot.is_transitioning());
}

#[test]
fn unknown_future_fields_are_ignored() {
    // The Android fixtures may gain fields the desktop schema does not know.
    // serde(default) + no deny_unknown_fields keeps forward compatibility.
    let snapshot: Snapshot = serde_json::from_str(include_str!(
        "../../tests/contracts/runtime-snapshot/unsupported-schema.json"
    ))
    .expect("unsupported-schema.json parses");
    let _ = snapshot.generation;
}

#[test]
fn desktop_snapshot_parses() {
    // Full desktop snapshot mirroring PppApplication::BuildRuntimeSnapshot
    // output: desktop-only extensions (network/geo/outbounds) included.
    let json = include_str!("fixtures/desktop-snapshot.json");
    let snapshot: Snapshot = serde_json::from_str(json).expect("desktop-snapshot.json parses");
    assert_eq!(snapshot.phase, "connected");
    assert_eq!(snapshot.bypass_mode, "geo");
    assert_eq!(snapshot.mux_receiver_ordering, "flow_v2");
    assert_eq!(snapshot.mux_active_links, 2);
    assert_eq!(snapshot.ordering_label(), "flow-v2");

    let tun = snapshot.network.tun.expect("tun interface present");
    assert_eq!(tun.name, "PPP");
    assert_eq!(tun.ipv4, "10.0.0.2");
    assert_eq!(snapshot.geo.rule_count, 128);
    assert_eq!(snapshot.geo.split_rules.len(), 2);
    assert_eq!(snapshot.outbounds.len(), 2);

    let active = snapshot
        .outbounds
        .iter()
        .find(|o| o.active)
        .expect("one active outbound");
    assert_eq!(active.tag, "main");
    assert_eq!(active.current_entry, "104.16.1.1:443");
    assert!(active.probe_reachable);
    assert_eq!(active.probe_rtt_ms, 42);
}
