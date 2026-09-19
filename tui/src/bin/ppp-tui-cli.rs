//! Terminal entry point for the shared Rust TUI client.

use anyhow::Result;
use ppp_tui::core::control::{execute_once, format_human_result, parse_cli_control};
use ppp_tui::core::settings::StartupSettings;

fn main() {
    if let Err(error) = run() {
        eprintln!("ppp-tui-cli 错误：{error:#}");
        std::process::exit(1);
    }
}

fn run() -> Result<()> {
    let args = std::env::args().skip(1).collect::<Vec<_>>();
    if let Some(request) = parse_cli_control(&args)? {
        let value = execute_once(&request.address, &request.token, request.command.clone())?;
        if request.json {
            println!("{}", serde_json::to_string_pretty(&value)?);
        } else {
            println!("{}", format_human_result(&request.command, &value));
        }
        return Ok(());
    }

    ppp_tui::terminal::run(StartupSettings::from_cli())
}
