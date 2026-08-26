//! Root layout: tab bar, page content and help overlay.

pub mod logs;
pub mod network;
pub mod overview;
pub mod routes;
pub mod servers;
pub mod theme;
pub mod widgets;

use ratatui::layout::{Constraint, Direction, Layout};
use ratatui::style::{Modifier, Style};
use ratatui::text::{Line, Span};
use ratatui::widgets::{Block, Borders, Tabs};
use ratatui::Frame;

use crate::app::{App, Page};
use crate::ui::widgets::BlankArea;

pub const TAB_TITLES: [&str; 5] = [" Overview ", " Network ", " SERVERS ", " Logs ", " Routes "];

pub fn draw(frame: &mut Frame, app: &App) {
    // Write spaces into ratatui's in-memory frame. The final buffer is then
    // diffed against the previous frame, so shrinking values are erased
    // without a physical terminal clear and without visible flicker.
    frame.render_widget(BlankArea, frame.area());

    let chunks = Layout::default()
        .direction(Direction::Vertical)
        .constraints([Constraint::Length(3), Constraint::Min(1)])
        .split(frame.area());

    // Tab bar.
    let titles: Vec<Line> = TAB_TITLES
        .iter()
        .map(|t| {
            Line::from(Span::styled(
                *t,
                Style::default().add_modifier(Modifier::BOLD),
            ))
        })
        .collect();
    let tabs = Tabs::new(titles)
        .select(app.page.index())
        .block(Block::default().borders(Borders::ALL).title(" ppp-tui "))
        .style(Style::default().fg(ratatui::style::Color::DarkGray))
        .highlight_style(
            Style::default()
                .fg(ratatui::style::Color::White)
                .add_modifier(Modifier::BOLD),
        );
    frame.render_widget(tabs, chunks[0]);

    match (&app.page, &app.snapshot) {
        (Page::Overview, Some(snapshot)) => {
            overview::draw(frame, chunks[1], snapshot, &app.traffic)
        }
        (Page::Network, Some(snapshot)) => network::draw(frame, chunks[1], snapshot),
        (Page::Servers, Some(snapshot)) => servers::draw(frame, chunks[1], snapshot, app),
        (Page::Logs, _) => logs::draw(frame, chunks[1], app),
        (Page::Routes, Some(snapshot)) => routes::draw(frame, chunks[1], snapshot),
        _ => {
            let text = if app.snapshot.is_some() {
                "no data"
            } else {
                "waiting for the core snapshot..."
            };
            frame.render_widget(ratatui::widgets::Paragraph::new(text), chunks[1]);
        }
    }

    // Help overlay.
    if app.help_open {
        let popup_area = widgets::centered_rect(70, 70, frame.area());
        widgets::render_popup(
            frame,
            popup_area,
            "Help",
            vec![
                Line::from(""),
                Line::from(" Tab / Left / Right / 1-5 : switch page"),
                Line::from(" Up / Down                  : move selection (SERVERS)"),
                Line::from(" Enter                      : switch server (active = Rank #1)"),
                Line::from(" /                          : filter (SERVERS) / search (Logs)"),
                Line::from(" Esc                        : cancel / leave filter"),
                Line::from(" End                        : logs back to follow"),
                Line::from(" Mouse                      : terminal selection / right-click copy"),
                Line::from("                             scroll wheel on Logs"),
                Line::from(" q / Ctrl+C                 : quit (core keeps running)"),
                Line::from(" ?                          : this help"),
                Line::from(""),
                Line::from("                    [Enter / Esc / ?] close"),
            ],
        );
    }
}
