//! The C++ headless core bundled into the Rust TUI executable at build time.
//!
//! `build.rs` copies the selected `ppp.exe` into Cargo's `OUT_DIR`; the bytes
//! below are then linked into `ppp-tui`.  The core is still launched as a
//! child process so the existing local RPC boundary and the C++ TUI code stay
//! unchanged.

pub const CORE_EXE: &[u8] = include_bytes!(env!("PPP_TUI_EMBEDDED_CORE"));

#[allow(dead_code)]
pub const CORE_SOURCE: &str = env!("PPP_TUI_EMBEDDED_CORE_SOURCE");
