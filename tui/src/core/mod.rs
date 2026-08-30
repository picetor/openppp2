//! Core-side helpers: traffic sampling, in-process core control, command-line
//! construction, persistent settings and the server configuration catalog.
//! This is the shared business layer used by both front-ends.

pub mod command;
pub mod control;
pub mod in_process;
pub mod probe;
pub mod server_catalog;
pub mod settings;
pub mod traffic;
