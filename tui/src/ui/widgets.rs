//! Shared widgets: status badges, K/V blocks, traffic sparklines.

use ratatui::buffer::Buffer;
use ratatui::layout::{Alignment, Constraint, Direction, Layout, Rect};
use ratatui::style::{Modifier, Style};
use ratatui::text::{Line, Span};
use ratatui::widgets::{Block, Borders, Paragraph, Sparkline, Widget};
use ratatui::Frame;

use crate::core::traffic::{format_rate, TrafficHistory};
use crate::rpc::schema::Snapshot;
use crate::ui::theme;

/// Fill an area with real spaces before drawing the frame.
///
/// Ratatui's `Clear` resets cells to an empty symbol. That is enough for
/// overlays in the normal diff model, but a terminal cannot erase an old
/// glyph when it receives an empty string. Writing spaces makes shrinking
/// values and page changes deterministic without a physical full-screen
/// clear (and therefore without flicker).
pub struct BlankArea;

impl Widget for BlankArea {
    fn render(self, area: Rect, buf: &mut Buffer) {
        for y in area.top()..area.bottom() {
            for x in area.left()..area.right() {
                let cell = &mut buf[(x, y)];
                cell.reset();
                cell.set_symbol(" ");
            }
        }
    }
}

/// One "phase" badge: a coloured dot plus text.
pub fn phase_badge(snapshot: &Snapshot) -> Line<'static> {
    let (color, text) = match snapshot.phase.as_str() {
        "connected" => (theme::PHASE_CONNECTED, "● connected"),
        "connecting" => (theme::PHASE_TRANSITION, "◐ connecting"),
        "reconnecting" => (theme::PHASE_TRANSITION, "◑ reconnecting"),
        "failed" => (theme::PHASE_FAILED, "● failed"),
        _ => (theme::PHASE_IDLE, "○ idle"),
    };
    Line::from(Span::styled(
        text,
        Style::default().fg(color).add_modifier(Modifier::BOLD),
    ))
}

/// Outbound state badge for one row of the SERVERS table.
pub fn outbound_badge(
    state: i32,
    reachable: bool,
    checked: bool,
    probe_enabled: bool,
) -> Line<'static> {
    let (color, text) = match state {
        1 => (theme::OUTBOUND_ESTABLISHED, "established"),
        0 => (theme::OUTBOUND_CONNECTING, "connecting"),
        2 => (theme::OUTBOUND_RECONNECTING, "reconnecting"),
        _ => {
            if checked && !reachable {
                (theme::OUTBOUND_UNREACHABLE, "unreachable")
            } else if !probe_enabled {
                (theme::OUTBOUND_UNKNOWN, "probe off")
            } else {
                (theme::OUTBOUND_UNKNOWN, "unknown")
            }
        }
    };
    Line::from(Span::styled(text, Style::default().fg(color)))
}

/// MUX mode badge.
pub fn mux_badge(mode: &str, enabled: bool) -> Line<'static> {
    let color = if !enabled {
        theme::MUX_DISABLED
    } else {
        match mode {
            "compat" => theme::MUX_COMPAT,
            "flow" => theme::MUX_FLOW,
            "balance" => theme::MUX_BALANCE,
            "stripe" => theme::MUX_STRIPE,
            _ => theme::MUX_DISABLED,
        }
    };
    let text = if enabled {
        mode.to_string()
    } else {
        "disabled".to_string()
    };
    Line::from(Span::styled(text, Style::default().fg(color)))
}

/// A titled key/value block.
pub fn kv_block<'a>(title: &'a str, rows: Vec<(String, Line<'a>)>) -> Paragraph<'a> {
    let mut lines: Vec<Line> = Vec::with_capacity(rows.len());
    for (key, value) in rows {
        let mut spans = vec![Span::styled(
            format!("{key:<22}: "),
            Style::default().fg(theme::INFO_TEXT),
        )];
        spans.extend(value.spans);
        lines.push(Line::from(spans));
    }
    Paragraph::new(lines).block(
        Block::default()
            .borders(Borders::ALL)
            .title(format!(" {title} "))
            .border_style(Style::default().fg(theme::INFO_TEXT)),
    )
}

/// Render the rx/tx rate history as two sparklines (down = rx, up = tx).
pub fn traffic_sparklines(area: Rect, history: &TrafficHistory, frame: &mut Frame) {
    let chunks = Layout::default()
        .direction(Direction::Vertical)
        .constraints([Constraint::Ratio(1, 2), Constraint::Ratio(1, 2)])
        .split(area);

    let rx: Vec<u64> = history
        .samples()
        .iter()
        .map(|p| p.rx_bytes_per_sec)
        .collect();
    let tx: Vec<u64> = history
        .samples()
        .iter()
        .map(|p| p.tx_bytes_per_sec)
        .collect();

    frame.render_widget(
        Sparkline::default()
            .block(
                Block::default()
                    .borders(Borders::ALL)
                    .title(format!(" ▼ rx ({}) ", latest_rate(&rx)))
                    .border_style(Style::default().fg(theme::RX_COLOR)),
            )
            .data(&rx)
            .style(Style::default().fg(theme::RX_COLOR))
            .max(max_of(&rx).max(1)),
        chunks[0],
    );
    frame.render_widget(
        Sparkline::default()
            .block(
                Block::default()
                    .borders(Borders::ALL)
                    .title(format!(" ▲ tx ({}) ", latest_rate(&tx)))
                    .border_style(Style::default().fg(theme::TX_COLOR)),
            )
            .data(&tx)
            .style(Style::default().fg(theme::TX_COLOR))
            .max(max_of(&tx).max(1)),
        chunks[1],
    );
}

fn max_of(data: &[u64]) -> u64 {
    data.iter().copied().max().unwrap_or(0)
}

fn latest_rate(data: &[u64]) -> String {
    match data.last() {
        Some(v) => format_rate(*v),
        None => "waiting for traffic...".to_string(),
    }
}

/// Centered confirmation popup.
pub fn centered_rect(percent_x: u16, percent_y: u16, area: Rect) -> Rect {
    let popup_layout = Layout::default()
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
        .split(popup_layout[1])[1]
}

/// Small helper to render a centered text popup (used by confirmations).
pub fn render_popup(frame: &mut Frame, area: Rect, title: &str, lines: Vec<Line>) {
    let block = Block::default()
        .borders(Borders::ALL)
        .title(format!(" {title} "))
        .border_style(Style::default().fg(theme::WARN_TEXT));
    let paragraph = Paragraph::new(lines)
        .block(block)
        .alignment(Alignment::Center)
        .wrap(ratatui::widgets::Wrap { trim: true });
    frame.render_widget(paragraph, area);
}

/// Format a phase label for the status bar.
pub fn status_bar_spans(snapshot: &Snapshot) -> Vec<Span<'static>> {
    let mut spans: Vec<Span> = Vec::new();
    let (color, text) = match snapshot.phase.as_str() {
        "connected" => (theme::PHASE_CONNECTED, "connected"),
        "connecting" => (theme::PHASE_TRANSITION, "connecting"),
        "reconnecting" => (theme::PHASE_TRANSITION, "reconnecting"),
        "failed" => (theme::PHASE_FAILED, "failed"),
        _ => (theme::PHASE_IDLE, "idle"),
    };
    spans.push(Span::styled(
        text,
        Style::default().fg(color).add_modifier(Modifier::BOLD),
    ));
    spans.push(Span::raw(" · "));
    spans.push(Span::raw(snapshot.server.clone()));
    spans.push(Span::raw(" · "));
    spans.push(Span::raw(snapshot.transport.clone()));
    spans.push(Span::raw(" · mux "));
    spans.push(Span::styled(
        format!(
            "{}({})",
            snapshot.effective_mux_mode, snapshot.mux_active_links
        ),
        Style::default().fg(theme::MUX_FLOW),
    ));
    spans.push(Span::raw(" · ? help"));
    spans
}
