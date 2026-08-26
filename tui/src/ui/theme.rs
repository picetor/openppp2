//! Colour/theme constants (docs/RUST_TUI_DESIGN_CN.md §5.5.4).
//! All badge colours are centralized here; a future `--theme` option can
//! override this module.

use ratatui::style::Color;

pub const PHASE_CONNECTED: Color = Color::Green;
pub const PHASE_TRANSITION: Color = Color::Yellow;
pub const PHASE_FAILED: Color = Color::Red;
pub const PHASE_IDLE: Color = Color::DarkGray;

pub const OUTBOUND_ESTABLISHED: Color = Color::Green;
pub const OUTBOUND_CONNECTING: Color = Color::Yellow;
pub const OUTBOUND_RECONNECTING: Color = Color::Yellow;
pub const OUTBOUND_UNREACHABLE: Color = Color::Red;
pub const OUTBOUND_UNKNOWN: Color = Color::DarkGray;

pub const MUX_COMPAT: Color = Color::Blue;
pub const MUX_FLOW: Color = Color::Cyan;
pub const MUX_BALANCE: Color = Color::Magenta;
pub const MUX_STRIPE: Color = Color::LightMagenta;
pub const MUX_DISABLED: Color = Color::DarkGray;

pub const RX_COLOR: Color = Color::Green;
pub const TX_COLOR: Color = Color::Blue;
pub const HOT_SWITCH_ACTIVE: Color = Color::LightCyan;

pub const ERROR_TEXT: Color = Color::Red;
pub const WARN_TEXT: Color = Color::Yellow;
pub const INFO_TEXT: Color = Color::Gray;
pub const DEBUG_TEXT: Color = Color::DarkGray;
pub const OK_TEXT: Color = Color::Green;
