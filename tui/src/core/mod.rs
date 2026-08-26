//! Core-side helpers: traffic sampling, core process launching, command-line
//! construction, persistent settings and the server configuration catalog.
//! This is the shared business layer used by both the window and the
//! terminal front-ends.

pub mod command;
pub mod control;
pub mod embedded;
pub mod launcher;
pub mod probe;
pub mod server_catalog;
pub mod settings;
pub mod traffic;
