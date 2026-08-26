//! SERVERS page: compact two-line rows matching the built-in C++ TUI.

use ratatui::layout::Rect;
use ratatui::style::{Modifier, Style};
use ratatui::text::{Line, Span};
use ratatui::widgets::{Block, Borders, List, ListItem, ListState, Paragraph};
use ratatui::Frame;

use crate::app::App;
use crate::rpc::schema::{Outbound, Snapshot};
use crate::ui::theme;

pub fn draw(frame: &mut Frame, area: Rect, snapshot: &Snapshot, app: &App) {
    let filter = app.filter.as_deref().unwrap_or("");
    let rows = app.visible_outbounds();
    let server_count = snapshot
        .outbounds
        .iter()
        .filter(|o| o.server_menu || o.tag.eq_ignore_ascii_case("main"))
        .count();

    let title = if filter.is_empty() {
        format!(" SERVERS ({server_count}) ")
    } else {
        format!(
            " SERVERS ({}/{}) filter: {} ",
            rows.len(),
            server_count,
            filter
        )
    };

    let mut items = Vec::with_capacity(rows.len() + 2);
    items.push(ListItem::new(Line::from(format!(
        "Servers : {}",
        server_count
    ))));
    items.push(ListItem::new(Line::from(
        "Use Up/Down to select. Enter switches server; active = Rank #1.",
    )));

    for (visible_index, &snapshot_index) in rows.iter().enumerate() {
        let Some(outbound) = snapshot.outbounds.get(snapshot_index) else {
            continue;
        };
        let selected = visible_index == app.server_selection;
        let marker = if selected { '>' } else { ' ' };
        let active = if outbound.active { '*' } else { ' ' };
        let name = format!("{marker}{active} {:<24}", outbound.display_name);
        let usage = usage_text(outbound);
        let first_line = Line::from(vec![
            Span::styled(
                name,
                if selected {
                    Style::default()
                        .fg(theme::HOT_SWITCH_ACTIVE)
                        .add_modifier(Modifier::BOLD)
                } else {
                    Style::default()
                },
            ),
            Span::raw(usage),
        ]);
        let second_line = Line::from(format!("    {}", entry_text(outbound)));
        items.push(ListItem::new(vec![first_line, second_line]));
    }

    let selected_item = if rows.is_empty() {
        None
    } else {
        Some(2 + app.server_selection.min(rows.len() - 1))
    };
    let mut state = ListState::default();
    state.select(selected_item);
    let list = List::new(items)
        .block(Block::default().borders(Borders::ALL).title(title))
        .highlight_style(
            Style::default()
                .fg(theme::HOT_SWITCH_ACTIVE)
                .add_modifier(Modifier::BOLD),
        )
        .highlight_symbol("");
    frame.render_stateful_widget(list, area, &mut state);

    // Filter input hint.
    if app.filter_active {
        let hint = Line::from(format!("filter: {}", filter));
        frame.render_widget(
            Paragraph::new(hint).block(Block::default().borders(Borders::ALL).title(" filter ")),
            Rect {
                x: area.x + area.width.saturating_sub(40).min(area.width / 2),
                y: area.y + area.height.saturating_sub(3),
                width: 40.min(area.width),
                height: 3,
            },
        );
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
        1 => usage.push_str(" connected"),
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
