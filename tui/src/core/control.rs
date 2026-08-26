//! Shared control-plane helpers for the GUI, TUI and one-shot CLI commands.
//!
//! The presentation layers decide how a command is triggered. This module
//! defines the scriptable CLI surface and the common request/response loop so
//! all front-ends use the same typed RPC contract.

use std::thread;
use std::time::{Duration, Instant};

use anyhow::{bail, Context, Result};
use serde_json::Value;

use crate::rpc::{CoreCommand, Response, RpcClient};

const HELLO_TIMEOUT: Duration = Duration::from_secs(2);
const COMMAND_TIMEOUT: Duration = Duration::from_secs(8);

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CliControlRequest {
    pub address: String,
    pub token: String,
    pub command: CoreCommand,
    pub json: bool,
}

/// Parse a one-shot control command.
///
/// Returning `Ok(None)` means the arguments are normal TUI/core startup
/// arguments and should be handled by `StartupSettings::from_cli`.
/// Commands intentionally have to be the first positional argument so core
/// arguments such as `--config` and `--mode` remain unambiguous.
pub fn parse_cli_control(args: &[String]) -> Result<Option<CliControlRequest>> {
    let Some(name) = args.first().map(String::as_str) else {
        return Ok(None);
    };

    let recognized = matches!(
        name,
        "status"
            | "snapshot"
            | "logs"
            | "switch"
            | "switch-rank1"
            | "log-level"
            | "stop"
            | "shutdown"
            | "restart"
    );
    if !recognized {
        return Ok(None);
    }

    let mut address = None;
    let mut token = None;
    let mut json_output = false;
    let mut since_seq = 0_u64;
    let mut rank1 = name == "switch-rank1";
    let mut positional = Vec::new();
    let mut index = 1;

    while index < args.len() {
        let arg = args[index].as_str();
        match arg {
            "--json" => json_output = true,
            "--rank1" => rank1 = true,
            "--rpc" => {
                index += 1;
                address = Some(next_value(args, index, "--rpc")?);
            }
            "--token" => {
                index += 1;
                token = Some(next_value(args, index, "--token")?);
            }
            "--since" | "--since-seq" => {
                index += 1;
                since_seq = next_value(args, index, arg)?
                    .parse()
                    .with_context(|| format!("invalid sequence number for {arg}"))?;
            }
            _ if arg.starts_with("--rpc=") => address = Some(arg[6..].to_string()),
            _ if arg.starts_with("--token=") => token = Some(arg[8..].to_string()),
            _ if arg.starts_with("--since=") => {
                since_seq = arg[8..]
                    .parse()
                    .context("invalid sequence number for --since")?;
            }
            _ if arg.starts_with("--since-seq=") => {
                since_seq = arg[12..]
                    .parse()
                    .context("invalid sequence number for --since-seq")?;
            }
            _ if arg.starts_with("--") => bail!("unknown control option: {arg}"),
            _ => positional.push(arg.to_string()),
        }
        index += 1;
    }

    let command = match name {
        "status" | "snapshot" => {
            require_no_positional(name, &positional)?;
            CoreCommand::GetSnapshot
        }
        "logs" => {
            require_no_positional(name, &positional)?;
            CoreCommand::GetLogs { since_seq }
        }
        "switch" | "switch-rank1" => {
            let tag = one_positional(name, &positional)?;
            CoreCommand::Switch {
                tag,
                ranked_first: rank1,
            }
        }
        "log-level" => CoreCommand::SetLogLevel {
            level: one_positional(name, &positional)?,
        },
        "stop" | "shutdown" => {
            require_no_positional(name, &positional)?;
            CoreCommand::Shutdown { restart: false }
        }
        "restart" => {
            require_no_positional(name, &positional)?;
            CoreCommand::Shutdown { restart: true }
        }
        _ => unreachable!(),
    };

    let address = address
        .filter(|value| !value.trim().is_empty())
        .context("control commands require --rpc <address>")?;
    let token = token
        .filter(|value| !value.trim().is_empty())
        .context("control commands require --token <token>")?;

    Ok(Some(CliControlRequest {
        address,
        token,
        command,
        json: json_output,
    }))
}

fn next_value(args: &[String], index: usize, option: &str) -> Result<String> {
    args.get(index)
        .filter(|value| !value.starts_with("--"))
        .cloned()
        .with_context(|| format!("{option} requires a value"))
}

fn require_no_positional(command: &str, values: &[String]) -> Result<()> {
    if let Some(value) = values.first() {
        bail!("{command} does not accept positional argument '{value}'")
    }
    Ok(())
}

fn one_positional(command: &str, values: &[String]) -> Result<String> {
    if values.len() != 1 {
        bail!("{command} requires exactly one positional argument")
    }
    Ok(values[0].clone())
}

/// Execute one typed command against an already-running headless core.
pub fn execute_once(address: &str, token: &str, command: CoreCommand) -> Result<Value> {
    let mut rpc = RpcClient::new(address.to_string(), token.to_string());
    rpc.connect().context("connect to core RPC")?;

    wait_for_response(&mut rpc, "hello", HELLO_TIMEOUT)?;
    if !rpc.is_authenticated() {
        bail!("core RPC authentication failed")
    }

    let method = command.method();
    rpc.request_command(command)?;
    wait_for_response(&mut rpc, method, COMMAND_TIMEOUT)
}

fn wait_for_response(rpc: &mut RpcClient, method: &str, timeout: Duration) -> Result<Value> {
    let deadline = Instant::now() + timeout;
    while Instant::now() < deadline {
        match rpc.poll().context("poll core RPC")? {
            Some(Response::Result {
                method: response_method,
                value,
                ..
            }) if response_method == method => return Ok(value),
            Some(Response::Error {
                method: response_method,
                code,
                message,
                ..
            }) if response_method == method => {
                bail!("core rejected {method} ({code}): {message}")
            }
            Some(_) | None => thread::sleep(Duration::from_millis(5)),
        }
    }
    bail!("timeout waiting for core RPC method {method}")
}

/// Render a stable human-readable result for the one-shot CLI. `--json`
/// callers can serialize the returned value directly instead.
pub fn format_human_result(command: &CoreCommand, value: &Value) -> String {
    match command {
        CoreCommand::GetSnapshot => {
            let fields = [
                ("phase", value.get("phase").and_then(Value::as_str)),
                ("role", value.get("role").and_then(Value::as_str)),
                ("server", value.get("server").and_then(Value::as_str)),
                (
                    "connection",
                    value.get("connection").and_then(Value::as_str),
                ),
                ("log_level", value.get("log_level").and_then(Value::as_str)),
            ];
            fields
                .into_iter()
                .map(|(key, value)| format!("{key:<12} {}", value.unwrap_or("")))
                .collect::<Vec<_>>()
                .join("\n")
        }
        CoreCommand::GetLogs { .. } => value
            .get("logs")
            .and_then(Value::as_array)
            .map(|logs| {
                logs.iter()
                    .map(|entry| {
                        let seq = entry.get("seq").and_then(Value::as_u64).unwrap_or(0);
                        let level = entry.get("level").and_then(Value::as_str).unwrap_or("");
                        let line = entry.get("line").and_then(Value::as_str).unwrap_or("");
                        format!("[{seq}] {level:<5} {line}")
                    })
                    .collect::<Vec<_>>()
                    .join("\n")
            })
            .unwrap_or_default(),
        CoreCommand::Switch { .. } => format!(
            "accepted  {}\ntag        {}",
            value
                .get("accepted")
                .and_then(Value::as_bool)
                .unwrap_or(false),
            value.get("tag").and_then(Value::as_str).unwrap_or("")
        ),
        CoreCommand::SetLogLevel { .. } => format!(
            "level      {}",
            value.get("level").and_then(Value::as_str).unwrap_or("")
        ),
        CoreCommand::Shutdown { restart } => format!(
            "accepted  {}\naction    {}",
            value
                .get("accepted")
                .and_then(Value::as_bool)
                .unwrap_or(false),
            if *restart { "restart" } else { "shutdown" }
        ),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn args(values: &[&str]) -> Vec<String> {
        values.iter().map(|value| (*value).to_string()).collect()
    }

    #[test]
    fn command_wire_names_and_params_are_stable() {
        let command = CoreCommand::Switch {
            tag: "main".to_string(),
            ranked_first: true,
        };
        assert_eq!(command.method(), "switch_rank1");
        assert_eq!(command.params()["tag"], "main");
    }

    #[test]
    fn parses_status_control_command() {
        let request = parse_cli_control(&args(&[
            "status",
            "--rpc=127.0.0.1:39100",
            "--token",
            "secret",
            "--json",
        ]))
        .unwrap()
        .unwrap();
        assert_eq!(request.command, CoreCommand::GetSnapshot);
        assert!(request.json);
    }

    #[test]
    fn parses_switch_and_logs_controls() {
        let switch = parse_cli_control(&args(&[
            "switch",
            "hk",
            "--rpc",
            "127.0.0.1:1",
            "--token",
            "t",
            "--rank1",
        ]))
        .unwrap()
        .unwrap();
        assert_eq!(
            switch.command,
            CoreCommand::Switch {
                tag: "hk".to_string(),
                ranked_first: true,
            }
        );

        let logs = parse_cli_control(&args(&[
            "logs",
            "--since-seq=12",
            "--rpc",
            "127.0.0.1:1",
            "--token",
            "t",
        ]))
        .unwrap()
        .unwrap();
        assert_eq!(logs.command, CoreCommand::GetLogs { since_seq: 12 });
    }

    #[test]
    fn normal_tui_arguments_are_not_control_commands() {
        assert!(parse_cli_control(&args(&["--mode=client"]))
            .unwrap()
            .is_none());
    }
}
