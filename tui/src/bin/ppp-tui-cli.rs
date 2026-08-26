//! Terminal entry point for the shared Rust TUI client.

use ppp_tui::core::settings::StartupSettings;

fn main() {
    if let Err(error) = ppp_tui::terminal::run(StartupSettings::from_cli()) {
        eprintln!("ppp-tui-cli error: {error:#}");
        std::process::exit(1);
    }
}
