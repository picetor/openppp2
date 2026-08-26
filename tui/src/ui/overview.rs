//! Overview (dashboard) page: C++ TUI-compatible runtime facts,
//! live traffic sparklines, current vs ranked-first entry.

use ratatui::layout::{Constraint, Direction, Layout, Rect};
use ratatui::style::{Modifier, Style};
use ratatui::text::{Line, Span};
use ratatui::Frame;

use crate::core::traffic::{format_bytes, format_rate, TrafficHistory};
use crate::rpc::schema::Snapshot;
use crate::ui::theme;
use crate::ui::widgets::{kv_block, phase_badge, traffic_sparklines};

pub fn draw(frame: &mut Frame, area: Rect, snapshot: &Snapshot, history: &TrafficHistory) {
    let chunks = Layout::default()
        .direction(Direction::Vertical)
        .constraints([
            // Runtime facts and dashboard details are kept in one block,
            // matching the previous Overview layout without a duplicate panel.
            Constraint::Length(15),
            Constraint::Min(6),
            Constraint::Length(6),
        ])
        .split(area);

    let server = if snapshot.vpn_server.is_empty() {
        snapshot.server.clone()
    } else {
        snapshot.vpn_server.clone()
    };
    let guid = if snapshot.guid.is_empty() {
        "-".to_string()
    } else {
        snapshot.guid.clone()
    };
    let http_proxy = if snapshot.http_proxy.is_empty() {
        "off".to_string()
    } else {
        snapshot.http_proxy.clone()
    };
    let socks_proxy = if snapshot.socks_proxy.is_empty() {
        "off".to_string()
    } else {
        snapshot.socks_proxy.clone()
    };
    let hosting_environment = if snapshot.hosting_environment.is_empty() {
        snapshot.role.clone()
    } else {
        snapshot
            .hosting_environment
            .split_once(':')
            .map(|(role, _)| role.to_string())
            .unwrap_or_else(|| snapshot.hosting_environment.clone())
    };

    // Facts block. The ordering follows the C++ TUI's runtime information.
    let mut rows = vec![
        ("State".to_string(), phase_badge(snapshot)),
        ("Server".to_string(), Line::from(server)),
        ("GUID".to_string(), Line::from(guid)),
        (
            "Transport".to_string(),
            Line::from(Span::styled(
                snapshot.transport.clone(),
                Style::default().fg(theme::INFO_TEXT),
            )),
        ),
        (
            "Bypass Mode".to_string(),
            Line::from(snapshot.bypass_mode.clone()),
        ),
        ("Http Proxy".to_string(), Line::from(http_proxy)),
        ("Socks Proxy".to_string(), Line::from(socks_proxy)),
    ];

    let active_outbound = snapshot.outbounds.iter().find(|o| o.active);
    rows.push((
        "Main".to_string(),
        Line::from(match active_outbound {
            Some(outbound) if !outbound.display_name.is_empty() => outbound.display_name.clone(),
            _ => "-".to_string(),
        }),
    ));

    rows.push((
        "MUX".to_string(),
        Line::from(vec![
            Span::styled(
                snapshot.effective_mux_mode.clone(),
                Style::default().fg(mux_color(&snapshot.effective_mux_mode)),
            ),
            Span::raw(format!(
                " · {} links · {}",
                snapshot.mux_active_links,
                snapshot.ordering_label()
            )),
        ]),
    ));
    rows.push((
        "Hosting Environment".to_string(),
        Line::from(hosting_environment),
    ));
    rows.push((
        "Duration".to_string(),
        Line::from(format_duration(snapshot.duration_ms)),
    ));

    let latest = history.latest();
    let rate_rx = latest
        .map(|p| format_rate(p.rx_bytes_per_sec))
        .unwrap_or_else(|| "0 B/s".into());
    let rate_tx = latest
        .map(|p| format_rate(p.tx_bytes_per_sec))
        .unwrap_or_else(|| "0 B/s".into());
    rows.push((
        "Rate".to_string(),
        Line::from(vec![
            // Reserve the two-character download-arrow cell so the rate
            // values line up with the numeric cells in Total below.
            Span::raw("  "),
            Span::styled(
                format!("{:<10}", rate_rx),
                Style::default().fg(theme::RX_COLOR),
            ),
            Span::raw("    "),
            Span::styled(
                format!("{:<10}", rate_tx),
                Style::default().fg(theme::TX_COLOR),
            ),
        ]),
    ));
    rows.push((
        "Total".to_string(),
        // Cumulative totals retain the C++ TUI's direction arrows.  Use
        // Unicode escapes so the source encoding cannot turn them into
        // invisible/garbled characters in the Windows build.  Each value has
        // its own fixed-width cell so the decimal point cannot be visually
        // crowded by the adjacent direction/value cell.
        Line::from(vec![
            Span::styled(
                format!("\u{2193} {:<10}", format_bytes(snapshot.traffic.in_bytes)),
                Style::default().fg(theme::RX_COLOR),
            ),
            Span::raw("  "),
            Span::styled(
                format!("\u{2191} {:<10}", format_bytes(snapshot.traffic.out_bytes)),
                Style::default().fg(theme::TX_COLOR),
            ),
        ]),
    ));

    frame.render_widget(kv_block("Overview", rows), chunks[0]);

    // Traffic sparklines.
    traffic_sparklines(chunks[1], history, frame);

    // Last error (if any).
    let error_line = Line::from(vec![
        Span::styled(
            "Last Error",
            Style::default()
                .fg(theme::WARN_TEXT)
                .add_modifier(Modifier::BOLD),
        ),
        Span::raw(format!(": {}", snapshot.last_error.diagnostic_detail)),
    ]);
    frame.render_widget(
        ratatui::widgets::Paragraph::new(error_line).style(Style::default().fg(theme::ERROR_TEXT)),
        chunks[2],
    );
}

fn format_duration(duration_ms: u64) -> String {
    let total_seconds = duration_ms / 1000;
    let hours = total_seconds / 3600;
    let minutes = (total_seconds % 3600) / 60;
    let seconds = total_seconds % 60;
    format!("{hours:02}:{minutes:02}:{seconds:02}")
}

fn mux_color(mode: &str) -> ratatui::style::Color {
    match mode {
        "compat" => theme::MUX_COMPAT,
        "flow" => theme::MUX_FLOW,
        "balance" => theme::MUX_BALANCE,
        "stripe" => theme::MUX_STRIPE,
        _ => theme::MUX_DISABLED,
    }
}
