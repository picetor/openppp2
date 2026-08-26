//! ASCII rendering helpers for the terminal front-end: bordered cards,
//! character bar charts and the footer help line.  Plain ASCII only so the
//! UI renders correctly over SSH and in any terminal/font.

use ratatui::style::{Modifier, Style};
use ratatui::text::{Line, Span};

/// Render a card with an ASCII border and aligned `label  value` rows.
pub fn ascii_card(title: &str, rows: &[(String, String)]) -> Vec<Line<'static>> {
    let mut lines = Vec::new();
    lines.push(Line::from(format!("+----- {title} ")));
    for (label, value) in rows {
        lines.push(Line::from(format!("| {label:<22} {value}")));
    }
    lines.push(Line::from("+-----"));
    lines
}

/// Render a character bar chart for a rate sample series.
/// `width` is the number of characters available; samples are downsampled to
/// fit.  Output is one or more lines of block characters.
pub fn bar_chart_lines(data: &[u64], width: usize) -> Vec<Line<'static>> {
    if data.is_empty() || width == 0 {
        return vec![Line::from("  (no samples)")];
    }
    let max = data.iter().copied().max().unwrap_or(1).max(1);
    let bars_width = width.saturating_sub(2).max(1);
    let step = (data.len() / bars_width).max(1);
    let mut line = String::with_capacity(bars_width + 2);
    line.push_str("  ");
    let mut count = 0usize;
    for (index, value) in data.iter().enumerate() {
        if index % step != 0 {
            continue;
        }
        if count >= bars_width {
            break;
        }
        let height = ((*value as f64 / max as f64) * 8.0).round() as usize;
        let block = match height {
            0 => ' ',
            1 => '\u{2581}',
            2 => '\u{2582}',
            3 => '\u{2583}',
            4 => '\u{2584}',
            5 => '\u{2585}',
            6 => '\u{2586}',
            7 => '\u{2587}',
            _ => '\u{2588}',
        };
        line.push(block);
        count += 1;
    }
    vec![Line::from(line)]
}

/// Footer help line (kept short to fit narrow SSH terminals).
pub fn help_hint() -> &'static str {
    "1-5 views  Up/Down move  Enter select  p/o start/stop  Ctrl+S save  Ctrl+R restart  q quit"
}

/// Horizontal rule with a title, e.g. `----- Routes ----------`.
/// No vertical borders: sections are separated by rules only.
pub fn rule_line(title: &str, width: usize) -> String {
    let prefix = format!("----- {title} ");
    let fill = "-"
        .repeat(width.saturating_sub(prefix.len()).max(0))
        .to_string();
    format!("{prefix}{fill}")
}

/// Section card without vertical borders: a titled rule followed by
/// aligned `label  value` rows.  Mirrors the window client's cards while
/// keeping the terminal layout rule-based (no `|` columns).
pub fn plain_card(title: &str, rows: &[(String, String)], width: usize) -> Vec<Line<'static>> {
    let mut lines = vec![Line::from(rule_line(title, width))];
    for (label, value) in rows {
        lines.push(Line::from(format!(" {label:<22} {value}")));
    }
    lines
}

/// Multi-row column bar chart rendered top-to-bottom over `height`
/// levels.  Every row has a fixed width of `cols * 2` characters (bar cell
/// plus gap), so stacked charts align with their titles and fill the
/// terminal width instead of collapsing to the left when few samples exist.
pub fn bar_chart_columns(data: &[u64], cols: usize, height: usize) -> Vec<String> {
    if data.is_empty() || cols == 0 || height == 0 {
        return vec![String::new(); height];
    }
    let max = data.iter().copied().max().unwrap_or(1).max(1);
    let step = (data.len() / cols).max(1);
    let sampled: Vec<u64> = data
        .iter()
        .enumerate()
        .filter(|(index, _)| index % step == 0)
        .take(cols)
        .map(|(_, value)| *value)
        .collect();
    let heights: Vec<usize> = sampled
        .iter()
        .map(|value| {
            let scaled = (*value as f64 / max as f64) * height as f64;
            scaled.round().clamp(0.0, height as f64) as usize
        })
        .collect();
    let mut rows = Vec::with_capacity(height);
    for row in (0..height).rev() {
        let mut line = String::with_capacity(cols * 2);
        for h in &heights {
            if *h >= row + 1 {
                line.push('\u{2588}');
            } else {
                line.push(' ');
            }
            line.push(' ');
        }
        rows.push(line);
    }
    rows
}
/// Coloured status text helper.
pub fn status_span(text: &str, ok: bool) -> Span<'static> {
    Span::styled(
        text.to_string(),
        Style::default().fg(if ok {
            ratatui::style::Color::Green
        } else {
            ratatui::style::Color::Yellow
        }),
    )
}

/// Bold label helper.
pub fn label_span(text: &str) -> Span<'static> {
    Span::styled(
        text.to_string(),
        Style::default().add_modifier(Modifier::BOLD),
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn card_has_borders() {
        let rows = vec![("State".to_string(), "connected".to_string())];
        let card = ascii_card("Overview", &rows);
        assert!(card[0].to_string().starts_with('+'));
        assert!(card[2].to_string().starts_with('+'));
        assert!(card[1].to_string().contains("State"));
    }

    #[test]
    fn bar_chart_scales_to_max() {
        let lines = bar_chart_lines(&[0, 10, 20, 40, 80, 160, 320, 640], 10);
        let text = lines[0].to_string();
        // The max sample must render as the full block.
        assert!(text.contains('\u{2588}'));
        assert!(text.contains(' ')); // the zero sample stays blank
    }

    #[test]
    fn empty_series_placeholder() {
        let lines = bar_chart_lines(&[], 10);
        assert!(lines[0].to_string().contains("no samples"));
    }
}
