#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod gui;

fn main() -> eframe::Result<()> {
    let settings = gui::StartupSettings::from_cli();
    gui::configure_tui_logging(&settings);

    // Persist panics only when the configured TUI log path is enabled.  The
    // same setting must govern startup diagnostics and runtime TUI logs.
    std::panic::set_hook(Box::new(|info| {
        let message = format!("ppp-tui panic: {info}");
        eprintln!("{message}");
        gui::write_diagnostic_log(&format!("{message}\n"));
    }));
    let result = gui::run(settings);
    if let Err(error) = &result {
        let message = format!("ppp-tui startup error: {error:#}");
        eprintln!("{message}");
        gui::write_diagnostic_log(&format!("{message}\n"));
    }
    result
}
