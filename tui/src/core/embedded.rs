//! Legacy child-process core resources.
//!
//! The in-process build links `ppp-core` directly and does not include an
//! executable. These constants only exist for the compatibility build that
//! still launches an embedded headless core.

#[cfg(not(ppp_in_process_core))]
pub const CORE_EXE: &[u8] = include_bytes!(env!("PPP_TUI_EMBEDDED_CORE"));

#[cfg(not(ppp_in_process_core))]
#[allow(dead_code)]
pub const CORE_SOURCE: &str = env!("PPP_TUI_EMBEDDED_CORE_SOURCE");
