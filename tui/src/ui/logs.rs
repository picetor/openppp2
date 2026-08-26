//! Logs page: timestamped, level-coloured, scrollable, searchable.

use ratatui::layout::{Alignment, Constraint, Rect};
use ratatui::style::{Modifier, Style};
use ratatui::text::{Line, Span};
use ratatui::widgets::{Block, Borders, List, ListItem, ListState, Paragraph};
use ratatui::Frame;

use crate::app::{App, LogEntry};
use crate::ui::theme;

/// Format the core-side monotonic timestamp as HH:MM:SS.mmm.
pub fn format_timestamp(monotonic_ms: u64) -> String {
    let total_ms = monotonic_ms % (24 * 60 * 60 * 1000);
    let h = total_ms / 3_600_000;
    let m = (total_ms % 3_600_000) / 60_000;
    let s = (total_ms % 60_000) / 1000;
    let ms = total_ms % 1000;
    format!("{h:02}:{m:02}:{s:02}.{ms:03}")
}

fn level_color(level: &str) -> ratatui::style::Color {
    match level.to_uppercase().as_str() {
        "ERROR" => theme::ERROR_TEXT,
        "WARN" | "WARNING" => theme::WARN_TEXT,
        "DEBUG" => theme::DEBUG_TEXT,
        _ => theme::INFO_TEXT,
    }
}

fn entry_line(entry: &LogEntry) -> Line<'static> {
    Line::from(vec![
        Span::styled(
            format_timestamp(entry.timestamp_ms),
            Style::default().fg(theme::DEBUG_TEXT),
        ),
        Span::raw(" "),
        Span::styled(
            format!("[{:<5}]", entry.level.to_uppercase()),
            Style::default().fg(level_color(&entry.level)),
        ),
        Span::raw(" "),
        Span::raw(entry.line.clone()),
    ])
}

pub fn draw(frame: &mut Frame, area: Rect, app: &App) {
    let chunks = ratatui::layout::Layout::default()
        .direction(ratatui::layout::Direction::Vertical)
        .constraints([Constraint::Min(1), Constraint::Length(3)])
        .split(area);

    // Apply the search filter (matches line content, case-insensitive).
    let search = app.filter.as_deref().unwrap_or("");
    let visible: Vec<&LogEntry> = app
        .logs
        .iter()
        .filter(|e| {
            search.is_empty()
                || e.line.to_lowercase().contains(&search.to_lowercase())
                || e.level.to_lowercase().contains(&search.to_lowercase())
        })
        .collect();

    let total = visible.len();
    let follow = app.logs_follow;
    // When following, show the newest page; otherwise show `logs_scroll`
    // lines above the bottom.
    let scroll_offset = if follow {
        0
    } else {
        app.logs_scroll.min(total)
    };

    let items: Vec<ListItem> = visible
        .iter()
        .rev()
        .skip(scroll_offset)
        .take(area.height.saturating_sub(6) as usize)
        .map(|e| ListItem::new(entry_line(e)))
        .collect();

    let title = if search.is_empty() {
        format!(
            " Logs ({total}) {} ",
            if follow { "follow" } else { "paused" }
        )
    } else {
        format!(" Logs ({total}) filter: {search} ")
    };

    let mut state = ListState::default();
    state.select(Some(0));
    let list = List::new(items)
        .block(
            Block::default()
                .borders(Borders::ALL)
                .title(title)
                .border_style(Style::default().fg(theme::INFO_TEXT)),
        )
        .highlight_style(Style::default().add_modifier(Modifier::BOLD));
    frame.render_stateful_widget(list, chunks[0], &mut state);

    // Bottom hint bar.
    let hint = if follow {
        Line::from(" follow · scroll up to pause · / search · q quit ")
    } else {
        Line::from(vec![
            Span::raw(" paused "),
            Span::styled(
                "[End] back to follow",
                Style::default()
                    .fg(theme::HOT_SWITCH_ACTIVE)
                    .add_modifier(Modifier::BOLD),
            ),
            Span::raw(" · / search · q quit "),
        ])
    };
    frame.render_widget(Paragraph::new(hint).alignment(Alignment::Left), chunks[1]);
}
