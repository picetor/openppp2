//! Terminal event handling: key presses are translated into semantic
//! actions; the application state machine consumes actions, not keys.

use crossterm::event::{KeyCode, KeyEvent, KeyModifiers};
use std::collections::VecDeque;

use super::View;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Action {
    Quit,
    Cancel,
    SwitchView(View),
    MoveSelection(i64),
    PageScroll(i64),
    JumpEnd(bool),
    Activate,
    Toggle,
    Save,
    Restart,
    StopCore,
    StartCore,
    BypassMode(&'static str),
    Help,
    StartFilter,
}

/// Collects key events between frames and converts them to actions.
pub struct EventReader {
    queue: VecDeque<Action>,
}

impl EventReader {
    pub fn new() -> Self {
        Self {
            queue: VecDeque::new(),
        }
    }

    pub fn push(&mut self, key: KeyEvent) {
        if let Some(action) = translate(key) {
            self.queue.push_back(action);
        }
    }

    pub fn next_action(&mut self) -> Option<Action> {
        self.queue.pop_front()
    }
}

impl Default for EventReader {
    fn default() -> Self {
        Self::new()
    }
}

fn translate(key: KeyEvent) -> Option<Action> {
    // Windows-style terminals emit Release events too; ignore them so a
    // single key press does not trigger two actions.
    if key.kind == crossterm::event::KeyEventKind::Release {
        return None;
    }

    let ctrl = key.modifiers.contains(KeyModifiers::CONTROL);
    let shift = key.modifiers.contains(KeyModifiers::SHIFT);

    match key.code {
        // q and Ctrl+C both use the app-level double confirmation before
        // quitting, so a stray key press cannot close the app immediately.
        KeyCode::Char('q') if !ctrl => Some(Action::Quit),
        KeyCode::Char('c') if ctrl => Some(Action::Quit),
        KeyCode::Char('?') => Some(Action::Help),
        KeyCode::Esc => Some(Action::Cancel),
        // tmux-friendly navigation: unmodified digits switch pages (F-keys
        // and Ctrl+digits are unreliable over SSH + tmux chains).
        KeyCode::Char('1') if !ctrl => Some(Action::SwitchView(View::Overview)),
        KeyCode::Char('2') if !ctrl => Some(Action::SwitchView(View::Network)),
        KeyCode::Char('3') if !ctrl => Some(Action::SwitchView(View::Servers)),
        KeyCode::Char('4') if !ctrl => Some(Action::SwitchView(View::Routes)),
        KeyCode::Char('5') if !ctrl => Some(Action::SwitchView(View::Settings)),
        // F-keys and Ctrl+digits remain as optional enhancements.
        KeyCode::F(1) => Some(Action::SwitchView(View::Overview)),
        KeyCode::F(2) => Some(Action::SwitchView(View::Network)),
        KeyCode::F(3) => Some(Action::SwitchView(View::Servers)),
        KeyCode::F(4) => Some(Action::SwitchView(View::Routes)),
        KeyCode::F(5) => Some(Action::SwitchView(View::Settings)),
        KeyCode::Char('1') if ctrl => Some(Action::SwitchView(View::Overview)),
        KeyCode::Char('2') if ctrl => Some(Action::SwitchView(View::Network)),
        KeyCode::Char('3') if ctrl => Some(Action::SwitchView(View::Servers)),
        KeyCode::Char('4') if ctrl => Some(Action::SwitchView(View::Routes)),
        KeyCode::Char('5') if ctrl => Some(Action::SwitchView(View::Settings)),
        KeyCode::Tab => Some(Action::MoveSelection(if shift { -1 } else { 1 })),
        KeyCode::BackTab => Some(Action::MoveSelection(-1)),
        KeyCode::Up | KeyCode::Char('k') if !ctrl => Some(Action::MoveSelection(-1)),
        KeyCode::Down | KeyCode::Char('j') if !ctrl => Some(Action::MoveSelection(1)),
        KeyCode::PageUp => Some(Action::PageScroll(10)),
        KeyCode::PageDown => Some(Action::PageScroll(-10)),
        KeyCode::Char('u') if ctrl => Some(Action::PageScroll(10)),
        KeyCode::Char('d') if ctrl => Some(Action::PageScroll(-10)),
        KeyCode::Home => Some(Action::JumpEnd(false)),
        KeyCode::End | KeyCode::Char('G') if !ctrl => Some(Action::JumpEnd(true)),
        KeyCode::Enter => Some(Action::Activate),
        KeyCode::Char(' ') => Some(Action::Toggle),
        // Page-scoped single keys (tmux-safe; no Ctrl+ prefix collisions).
        // Page-scoped single keys mirror the terminal help bar.
        KeyCode::Char('p') if !ctrl => Some(Action::StartCore),
        KeyCode::Char('o') if !ctrl => Some(Action::StopCore),
        KeyCode::Char('s') if !ctrl => Some(Action::Save),
        KeyCode::Char('r') if !ctrl => Some(Action::Restart),
        KeyCode::Char('i') if !ctrl => Some(Action::BypassMode("ip")),
        KeyCode::Char('g') if !ctrl => Some(Action::BypassMode("geo")),
        KeyCode::Char('g') if ctrl => Some(Action::BypassMode("geo")),
        KeyCode::Char('n') if !ctrl => Some(Action::BypassMode("no")),
        KeyCode::Char('/') if !ctrl => Some(Action::StartFilter),
        // Ctrl+S/T/P/R remain available where the terminal allows them.
        KeyCode::Char('s') if ctrl => Some(Action::Save),
        KeyCode::Char('r') if ctrl => Some(Action::Restart),
        KeyCode::Char('t') if ctrl => Some(Action::StopCore),
        KeyCode::Char('p') if ctrl => Some(Action::StartCore),
        _ => None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn press(code: KeyCode) -> KeyEvent {
        KeyEvent::new(code, KeyModifiers::NONE)
    }

    #[test]
    fn function_keys_switch_views() {
        assert_eq!(
            translate(press(KeyCode::F(3))),
            Some(Action::SwitchView(View::Servers))
        );
        assert_eq!(
            translate(press(KeyCode::F(5))),
            Some(Action::SwitchView(View::Settings))
        );
    }

    #[test]
    fn ctrl_shortcuts() {
        let ctrl_s = KeyEvent::new(KeyCode::Char('s'), KeyModifiers::CONTROL);
        assert_eq!(translate(ctrl_s), Some(Action::Save));
        let ctrl_g = KeyEvent::new(KeyCode::Char('g'), KeyModifiers::CONTROL);
        assert_eq!(translate(ctrl_g), Some(Action::BypassMode("geo")));
    }

    #[test]
    fn tmux_safe_single_keys() {
        // Unmodified digits switch pages (no F-key / Ctrl dependency).
        assert_eq!(
            translate(press(KeyCode::Char('3'))),
            Some(Action::SwitchView(View::Servers))
        );
        assert_eq!(
            translate(press(KeyCode::Char('j'))),
            Some(Action::MoveSelection(1))
        );
        assert_eq!(
            translate(press(KeyCode::Char('k'))),
            Some(Action::MoveSelection(-1))
        );
        assert_eq!(
            translate(press(KeyCode::Char('g'))),
            Some(Action::BypassMode("geo"))
        );
        assert_eq!(
            translate(press(KeyCode::Char('G'))),
            Some(Action::JumpEnd(true))
        );
        assert_eq!(translate(press(KeyCode::Char('?'))), Some(Action::Help));
        assert_eq!(
            translate(press(KeyCode::Char('/'))),
            Some(Action::StartFilter)
        );
        assert_eq!(
            translate(press(KeyCode::Char('p'))),
            Some(Action::StartCore)
        );
        assert_eq!(translate(press(KeyCode::Char('o'))), Some(Action::StopCore));
        assert_eq!(translate(press(KeyCode::Char('q'))), Some(Action::Quit));
        assert_eq!(translate(press(KeyCode::Char('s'))), Some(Action::Save));
        assert_eq!(translate(press(KeyCode::Char('r'))), Some(Action::Restart));
        assert_eq!(
            translate(press(KeyCode::Char('i'))),
            Some(Action::BypassMode("ip"))
        );
        assert_eq!(
            translate(press(KeyCode::Char('n'))),
            Some(Action::BypassMode("no"))
        );
        // Ctrl+I is Tab (0x09); it must NOT map to a bypass action.
        let ctrl_i = KeyEvent::new(KeyCode::Char('i'), KeyModifiers::CONTROL);
        assert_ne!(translate(ctrl_i), Some(Action::BypassMode("ip")));
    }

    #[test]
    fn navigation_keys() {
        assert_eq!(
            translate(press(KeyCode::Down)),
            Some(Action::MoveSelection(1))
        );
        assert_eq!(
            translate(press(KeyCode::PageUp)),
            Some(Action::PageScroll(10))
        );
        assert_eq!(
            translate(press(KeyCode::Home)),
            Some(Action::JumpEnd(false))
        );
        assert_eq!(translate(press(KeyCode::Enter)), Some(Action::Activate));
        assert_eq!(translate(press(KeyCode::Esc)), Some(Action::Cancel));
    }

    #[test]
    fn quit_keys_require_app_confirmation() {
        assert_eq!(translate(press(KeyCode::Char('q'))), Some(Action::Quit));
        assert_ne!(translate(press(KeyCode::Char('Q'))), Some(Action::Quit));
        let ctrl_c = KeyEvent::new(KeyCode::Char('c'), KeyModifiers::CONTROL);
        assert_eq!(translate(ctrl_c), Some(Action::Quit));
    }

    #[test]
    fn queued_events_are_consumed_in_input_order() {
        let mut reader = EventReader::new();
        reader.push(press(KeyCode::Char('1')));
        reader.push(press(KeyCode::Char('2')));
        assert_eq!(
            reader.next_action(),
            Some(Action::SwitchView(View::Overview))
        );
        assert_eq!(
            reader.next_action(),
            Some(Action::SwitchView(View::Network))
        );
    }
}
