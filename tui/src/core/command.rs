//! Core command-line construction/parsing helpers shared by the window and
//! terminal front-ends.

use std::path::PathBuf;

/// Split a command line into arguments, honouring double quotes.
pub fn split_command_line(input: &str) -> Vec<String> {
    let mut args = Vec::new();
    let mut current = String::new();
    let mut quoted = false;
    for ch in input.chars() {
        if ch == '"' {
            quoted = !quoted;
        } else if ch.is_whitespace() && !quoted {
            if !current.is_empty() {
                args.push(std::mem::take(&mut current));
            }
        } else {
            current.push(ch);
        }
    }
    if !current.is_empty() {
        args.push(current);
    }
    args
}

/// Look up `--name value` or `--name=value` in an argument list.
pub fn command_value(args: &[String], name: &str) -> Option<String> {
    let prefix = format!("{name}=");
    let mut index = 0;
    while index < args.len() {
        if let Some(value) = args[index].strip_prefix(&prefix) {
            return Some(value.to_string());
        }
        if args[index] == name {
            if let Some(value) = args.get(index + 1) {
                if !value.starts_with("--") {
                    return Some(value.clone());
                }
            }
        }
        index += 1;
    }
    None
}

/// Import `--name` into a destination string when present.
pub fn import_text(args: &[String], name: &str, destination: &mut String) {
    if let Some(value) = command_value(args, name) {
        *destination = value;
    }
}

/// Boolean `--name` value with a default (yes/y/true/1/on, no/n/false/0/off).
pub fn command_bool(args: &[String], name: &str, default: bool) -> bool {
    let Some(value) = command_value(args, name) else {
        return default;
    };
    match value.trim().to_ascii_lowercase().as_str() {
        "yes" | "y" | "true" | "1" | "on" => true,
        "no" | "n" | "false" | "0" | "off" => false,
        _ => default,
    }
}

fn is_core_executable_arg(value: &str) -> bool {
    PathBuf::from(value)
        .file_name()
        .and_then(|name| name.to_str())
        .map(|name| {
            matches!(
                name.to_ascii_lowercase().as_str(),
                "ppp-tui.exe" | "ppp-tui" | "ppp.exe" | "ppp" | "ppp-core.exe" | "ppp-core"
            )
        })
        .unwrap_or(false)
}

/// Strip shell keywords (`start`, `/wait`, window titles, the exe path) from
/// a traditional start command so only core arguments remain.
pub fn normalize_core_args(mut args: Vec<String>) -> Vec<String> {
    while matches!(
        args.first()
            .map(|value| value.to_ascii_lowercase())
            .as_deref(),
        Some("start") | Some("/wait") | Some("/b") | Some("/min") | Some("/max")
    ) {
        args.remove(0);
    }

    if args.len() > 1 && !is_core_executable_arg(&args[0]) && is_core_executable_arg(&args[1]) {
        // `start "window title" ppp.exe ...` leaves the title as the first
        // token after the shell keyword.
        args.remove(0);
    }
    if args
        .first()
        .is_some_and(|value| is_core_executable_arg(value))
    {
        args.remove(0);
    }
    args
}

/// Remove `--name` and its optional value from an argument list.
pub fn remove_command_argument(args: &mut Vec<String>, name: &str) {
    let prefix = format!("{name}=");
    let mut filtered = Vec::with_capacity(args.len());
    let mut index = 0;
    while index < args.len() {
        if args[index] == name {
            index += 1;
            if index < args.len() && !args[index].starts_with("--") {
                index += 1;
            }
        } else if args[index].starts_with(&prefix) {
            index += 1;
        } else {
            filtered.push(std::mem::take(&mut args[index]));
            index += 1;
        }
    }
    *args = filtered;
}

pub fn set_command_argument(args: &mut Vec<String>, name: &str, value: &str) {
    remove_command_argument(args, name);
    args.push(format!("{name}={value}"));
}

pub fn set_optional_command_argument(args: &mut Vec<String>, name: &str, value: &str) {
    remove_command_argument(args, name);
    if !value.trim().is_empty() {
        args.push(format!("{name}={value}"));
    }
}

pub fn set_bool_if_non_default(args: &mut Vec<String>, name: &str, value: bool, default: bool) {
    remove_command_argument(args, name);
    if value != default {
        set_command_argument(args, name, if value { "yes" } else { "no" });
    }
}

pub fn set_optional_if_not_default(args: &mut Vec<String>, name: &str, value: &str, default: &str) {
    remove_command_argument(args, name);
    let value = value.trim();
    if !value.is_empty() && value != default {
        args.push(format!("{name}={value}"));
    }
}

pub fn join_command_args(args: &[String]) -> String {
    args.iter()
        .map(|arg| format_command_arg(arg))
        .collect::<Vec<_>>()
        .join(" ")
}

pub fn format_command_preview(args: &[String]) -> String {
    args.iter()
        .map(|arg| format!("  {}", format_command_arg(arg)))
        .collect::<Vec<_>>()
        .join("\n")
}

pub fn format_command_arg(arg: &str) -> String {
    if arg.chars().any(char::is_whitespace) {
        format!("\"{}\"", arg.replace('"', "\\\""))
    } else {
        arg.to_string()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn traditional_start_command_is_normalized_without_losing_windows_paths() {
        let raw = r#"start ppp.exe --mode=client --config=./config/rfcJP.json --server-dir=C:\openppp2\config --tun-gw=192.168.12.1"#;
        let args = normalize_core_args(split_command_line(raw));
        assert_eq!(args[0], "--mode=client");
        assert_eq!(
            command_value(&args, "--config").as_deref(),
            Some("./config/rfcJP.json")
        );
        assert_eq!(
            command_value(&args, "--server-dir").as_deref(),
            Some(r"C:\openppp2\config")
        );
    }

    #[test]
    fn command_title_form_is_supported() {
        let args = normalize_core_args(split_command_line(
            r#"start "PPP client" .\ppp.exe --mode=proxy"#,
        ));
        assert_eq!(args, vec!["--mode=proxy"]);
    }

    #[test]
    fn quoted_values_are_preserved() {
        let args = split_command_line(r#"--config="C:\my dir\a.json""#);
        assert_eq!(args, vec![r#"--config=C:\my dir\a.json"#]);
    }

    #[test]
    fn bool_defaults_and_forms() {
        let args = vec!["--tun-flash=yes".to_string(), "--tun-static=no".to_string()];
        assert!(command_bool(&args, "--tun-flash", false));
        assert!(!command_bool(&args, "--tun-static", true));
        assert!(command_bool(&args, "--tun-vnet", true));
    }
}
