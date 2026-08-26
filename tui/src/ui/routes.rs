//! Routes page: mode-specific routing facts and GEO split rules.

use ratatui::layout::{Constraint, Direction, Layout, Rect};
use ratatui::style::{Modifier, Style};
use ratatui::text::{Line, Span};
use ratatui::widgets::{Block, Borders, Cell, Paragraph, Row, Table};
use ratatui::Frame;

use crate::rpc::schema::Snapshot;
use crate::ui::theme;
use crate::ui::widgets::kv_block;

pub fn draw(frame: &mut Frame, area: Rect, snapshot: &Snapshot) {
    let rows = route_rows(snapshot);
    let facts_height = (rows.len() as u16).saturating_add(2);
    let chunks = Layout::default()
        .direction(Direction::Vertical)
        .constraints([Constraint::Length(facts_height), Constraint::Min(1)])
        .split(area);

    frame.render_widget(kv_block("Routes", rows), chunks[0]);

    match snapshot.bypass_mode.as_str() {
        "geo" => draw_geo_rules(frame, chunks[1], snapshot),
        "no" => frame.render_widget(
            Paragraph::new("Split-routing files are not used in global mode."),
            chunks[1],
        ),
        // IP mode has no GEO split-rule table.  Its actual routing inputs and
        // counts are shown in the facts block above, just like the C++ TUI.
        _ => frame.render_widget(Paragraph::new(""), chunks[1]),
    }
}

fn route_rows(snapshot: &Snapshot) -> Vec<(String, Line<'static>)> {
    let route = &snapshot.routes;
    match snapshot.bypass_mode.as_str() {
        "geo" => vec![
            (
                "Bypass Mode".to_string(),
                Line::from(Span::styled(
                    "geo (geosite/geoip)",
                    Style::default().fg(theme::MUX_FLOW),
                )),
            ),
            (
                "GEO Policy YAML".to_string(),
                value_line(&route.geo_rules_file),
            ),
            (
                "GeoSite Database".to_string(),
                value_line(&route.geosite_file),
            ),
            ("GeoIP Database".to_string(), value_line(&route.geoip_file)),
            (
                "Direct DNS".to_string(),
                Line::from(if snapshot.geo.direct_dns.is_empty() {
                    "none".to_string()
                } else {
                    snapshot.geo.direct_dns.join(", ")
                }),
            ),
            (
                "Rule Count".to_string(),
                Line::from(snapshot.geo.rule_count.to_string()),
            ),
            (
                "DNS Rule Count".to_string(),
                Line::from(route.dns_rule_count.to_string()),
            ),
        ],
        "no" => vec![(
            "Bypass Mode".to_string(),
            Line::from(Span::styled(
                "no rules",
                Style::default().fg(theme::MUX_DISABLED),
            )),
        )],
        _ => vec![
            (
                "Bypass Mode".to_string(),
                Line::from(Span::styled(
                    "ip (ip.txt/ipv6.txt)",
                    Style::default().fg(theme::OK_TEXT),
                )),
            ),
            (
                "Bypass IPv4 File".to_string(),
                value_line(&route.bypass_ipv4_file),
            ),
            (
                "Bypass IPv6 File".to_string(),
                value_line(&route.bypass_ipv6_file),
            ),
            (
                "Bypass Gateway".to_string(),
                value_line(&route.bypass_gateway),
            ),
            (
                "Bypass Gateway IPv6".to_string(),
                value_line(&route.bypass_gateway_ipv6),
            ),
            (
                "DNS Rules File".to_string(),
                value_line(&route.dns_rules_file),
            ),
            (
                "IP List Entries".to_string(),
                Line::from(route.ip_list_entries.to_string()),
            ),
            (
                "IPv6 List Entries".to_string(),
                Line::from(route.ipv6_list_entries.to_string()),
            ),
        ],
    }
}

fn value_line(value: &str) -> Line<'static> {
    Line::from(if value.is_empty() {
        "(none)".to_string()
    } else {
        value.to_string()
    })
}

fn draw_geo_rules(frame: &mut Frame, area: Rect, snapshot: &Snapshot) {
    if snapshot.geo.split_rules.is_empty() {
        frame.render_widget(
            Paragraph::new("none").block(
                Block::default()
                    .borders(Borders::ALL)
                    .title(" Split Rules "),
            ),
            area,
        );
        return;
    }

    let header = Row::new(vec![
        Cell::from(Span::styled(
            "MATCHER",
            Style::default().add_modifier(Modifier::BOLD),
        )),
        Cell::from(Span::styled(
            "OUTBOUND",
            Style::default().add_modifier(Modifier::BOLD),
        )),
    ]);
    let table_rows: Vec<Row> = snapshot
        .geo
        .split_rules
        .iter()
        .map(|rule| {
            Row::new(vec![
                Cell::from(Line::from(rule.matcher.clone())),
                Cell::from(Line::from(format!("{} ({})", rule.outbound, rule.display))),
            ])
        })
        .collect();

    let table = Table::new(
        table_rows,
        [Constraint::Percentage(60), Constraint::Percentage(40)],
    )
    .header(header)
    .block(
        Block::default()
            .borders(Borders::ALL)
            .title(" Split Rules "),
    );
    frame.render_widget(table, area);
}
