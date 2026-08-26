//! Server configuration catalog: scans a directory of client JSON files and
//! exposes them as selectable profiles.  Shared by both front-ends.

use std::fs;
use std::path::PathBuf;

use serde_json::Value;

use crate::core::settings::{
    comparable_path, display_path_string, resolve_from_working_dir, working_directory,
    StartupSettings,
};

#[derive(Debug, Clone)]
pub struct LocalServerProfile {
    pub name: String,
    pub path: PathBuf,
    pub server: String,
    pub entries: Vec<String>,
}

/// Scan `settings.server_dir` for client JSON files and build profiles.
/// Returns (profiles, first-warning) — individual unreadable files degrade to
/// a warning, never a hard failure.
pub fn load_server_catalog(
    settings: &StartupSettings,
) -> (Vec<LocalServerProfile>, Option<String>) {
    let working_dir = working_directory(settings);
    let directory = resolve_from_working_dir(&working_dir, &settings.server_dir);
    let read_dir = match fs::read_dir(&directory) {
        Ok(read_dir) => read_dir,
        Err(error) => {
            return (
                Vec::new(),
                Some(format!(
                    "服务器目录无法读取：{} ({error})",
                    display_path_string(&directory)
                )),
            );
        }
    };

    let mut files: Vec<PathBuf> = read_dir
        .filter_map(Result::ok)
        .map(|entry| entry.path())
        .filter(|path| {
            path.is_file()
                && path
                    .extension()
                    .and_then(|extension| extension.to_str())
                    .is_some_and(|extension| extension.eq_ignore_ascii_case("json"))
        })
        .collect();
    files.sort_by_key(|path| {
        path.file_name()
            .map(|name| name.to_string_lossy().to_ascii_lowercase())
            .unwrap_or_default()
    });

    let mut profiles = Vec::new();
    let mut errors = Vec::new();
    for path in files {
        let file_name = path
            .file_name()
            .map(|name| name.to_string_lossy().into_owned())
            .unwrap_or_else(|| display_path_string(&path));
        let contents = match fs::read_to_string(&path) {
            Ok(contents) => contents,
            Err(error) => {
                errors.push(format!("{file_name}: {error}"));
                continue;
            }
        };
        // JSON files exported by the Windows tooling may start with an UTF-8
        // BOM. serde_json intentionally does not treat that marker as JSON
        // whitespace, so remove it before parsing the server catalog.
        let contents = contents.strip_prefix('\u{feff}').unwrap_or(&contents);
        let root = match serde_json::from_str::<Value>(contents) {
            Ok(root) => root,
            Err(error) => {
                errors.push(format!("{file_name}: JSON 无法解析 ({error})"));
                continue;
            }
        };
        let Some(client) = root.get("client").and_then(Value::as_object) else {
            errors.push(format!("{file_name}: 缺少 client 配置段"));
            continue;
        };

        let mut entries = Vec::new();
        if let Some(server) = client.get("server").and_then(Value::as_str) {
            let server = server.trim();
            if !server.is_empty() {
                entries.push(server.to_string());
            }
        }
        if let Some(extra_entries) = client.get("servers").and_then(Value::as_array) {
            for entry in extra_entries.iter().filter_map(Value::as_str) {
                let entry = entry.trim();
                if !entry.is_empty() && !entries.iter().any(|item| item == entry) {
                    entries.push(entry.to_string());
                }
            }
        }
        if entries.is_empty() {
            errors.push(format!(
                "{file_name}: 没有 client.server 或 client.servers 入口"
            ));
            continue;
        }

        let name = path
            .file_stem()
            .map(|stem| stem.to_string_lossy().into_owned())
            .unwrap_or_else(|| file_name.trim_end_matches(".json").to_string());
        profiles.push(LocalServerProfile {
            name,
            path,
            server: entries[0].clone(),
            entries,
        });
    }

    let warning = errors.first().map(|first| {
        if errors.len() == 1 {
            format!("服务器目录中有配置无法读取：{first}")
        } else {
            format!("服务器目录中有 {} 个配置无法读取：{first}", errors.len())
        }
    });
    (profiles, warning)
}

/// Find the profile whose file matches `settings.config_path`.
pub fn find_selected_local_server(
    settings: &StartupSettings,
    profiles: &[LocalServerProfile],
) -> Option<usize> {
    let working_dir = working_directory(settings);
    let configured_path = resolve_from_working_dir(&working_dir, &settings.config_path);
    let configured_key = comparable_path(&configured_path);
    profiles
        .iter()
        .position(|profile| comparable_path(&profile.path) == configured_key)
}
