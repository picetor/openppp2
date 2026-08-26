#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod gui;

fn main() -> eframe::Result<()> {
    // Persist any panic to ./ppp-tui.log (merged with the other TUI logs)
    // so a headless failure (no console in the release build) still leaves
    // a trace for diagnosis.
    std::panic::set_hook(Box::new(|info| {
        let message = format!("ppp-tui panic: {info}");
        eprintln!("{message}");
        gui::write_diagnostic_log("ppp-tui.log", &format!("{message}\n"));
    }));
    let result = gui::run(gui::StartupSettings::from_cli());
    if let Err(error) = &result {
        let message = format!("ppp-tui startup error: {error:#}");
        eprintln!("{message}");
        gui::write_diagnostic_log("ppp-tui.log", &format!("{message}\n"));
    }
    result
}
