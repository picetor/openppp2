use std::env;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;

fn main() {
    let manifest_dir = PathBuf::from(env::var_os("CARGO_MANIFEST_DIR").unwrap());
    let repo_root = manifest_dir
        .parent()
        .expect("tui crate must live below the repository root");
    let out_dir = PathBuf::from(env::var_os("OUT_DIR").unwrap());

    println!("cargo:rustc-check-cfg=cfg(ppp_in_process_core)");
    let configured_library = env::var_os("PPP_TUI_CORE_LIB").map(PathBuf::from);
    let configured_external = env::var_os("PPP_TUI_CORE_PATH").map(PathBuf::from);
    // An explicit executable path is an intentional compatibility request.
    // Do not silently replace it with an auto-discovered static library just
    // because a developer build happens to leave ppp-core.lib in the tree.
    let core_library = if configured_external.is_none() {
        configured_library
            .filter(|path| path.is_file())
            .or_else(|| {
                [
                    repo_root.join("x64/Release/ppp-core.lib"),
                    repo_root.join("Release/ppp-core.lib"),
                    repo_root.join("x64/Debug/ppp-core.lib"),
                    repo_root.join("Debug/ppp-core.lib"),
                ]
                .into_iter()
                .find(|path| path.is_file())
            })
    } else {
        None
    };

    println!("cargo:rerun-if-env-changed=PPP_TUI_CORE_LIB");
    println!("cargo:rerun-if-env-changed=PPP_TUI_CORE_LIBS");
    println!("cargo:rerun-if-env-changed=PPP_TUI_CORE_LIB_DIRS");
    println!("cargo:rerun-if-env-changed=PPP_TUI_CORE_SYSTEM_LIBS");
    println!("cargo:rerun-if-env-changed=PPP_TUI_CORE_STATIC_SYSTEM_LIBS");
    println!("cargo:rerun-if-env-changed=PPP_TUI_CORE_PATH");

    if let Some(core_library) = core_library {
        // The in-process build links the C++ engine directly.  Do not require
        // or embed a ppp.exe in this mode: a Rust binary with the static core
        // is the product, while ppp.exe remains a separate thin CLI host.
        // Cargo does not otherwise know that the archive changed when the
        // C++ core is rebuilt in place, so make the embedded core a real build
        // input. This prevents a TUI rebuild from silently retaining an old
        // network/runtime implementation.
        println!("cargo:rerun-if-changed={}", core_library.display());
        println!("cargo:rustc-cfg=ppp_in_process_core");
        if let Some(parent) = core_library.parent() {
            println!("cargo:rustc-link-search=native={}", parent.display());
        }
        let library_name = core_library
            .file_stem()
            .and_then(|value| value.to_str())
            .unwrap_or("ppp-core")
            .strip_prefix("lib")
            .unwrap_or_else(|| {
                core_library
                    .file_stem()
                    .and_then(|value| value.to_str())
                    .unwrap_or("ppp-core")
            });
        println!("cargo:rustc-link-lib=static={library_name}");
        // `rustc-link-lib` from a package build script is attached to the
        // package library target.  The products are binaries, so pass the
        // concrete archive to both final binary link steps as well.
        emit_link_argument_for_bins(&core_library);

        let dependency_directories = env::var_os("PPP_TUI_CORE_LIB_DIRS")
            .map(|directories| {
                directories
                    .to_string_lossy()
                    .split(|character| character == ';' || character == ',')
                    .filter_map(|directory| {
                        let directory = directory.trim();
                        (!directory.is_empty()).then(|| PathBuf::from(directory))
                    })
                    .collect::<Vec<_>>()
            })
            .unwrap_or_default();
        for directory in &dependency_directories {
            println!("cargo:rustc-link-search=native={}", directory.display());
        }

        if let Some(libraries) = env::var_os("PPP_TUI_CORE_LIBS") {
            let value = libraries.to_string_lossy();
            for library in value.split(|character| character == ';' || character == ',') {
                let library = library.trim();
                if !library.is_empty() {
                    println!("cargo:rustc-link-lib=static={library}");
                    emit_named_library_for_bins(library, &dependency_directories, true, true);
                }
            }
        }
        if let Some(libraries) = env::var_os("PPP_TUI_CORE_SYSTEM_LIBS") {
            for library in libraries
                .to_string_lossy()
                .split(|character| character == ';' || character == ',')
            {
                let library = library.trim();
                if !library.is_empty() {
                    println!("cargo:rustc-link-lib={library}");
                    emit_named_library_for_bins(library, &dependency_directories, false, false);
                }
            }
        }
        if let Some(libraries) = env::var_os("PPP_TUI_CORE_STATIC_SYSTEM_LIBS") {
            for library in libraries
                .to_string_lossy()
                .split(|character| character == ';' || character == ',')
            {
                let library = library.trim();
                if !library.is_empty() {
                    println!("cargo:rustc-link-lib=static={library}");
                    emit_named_library_for_bins(library, &dependency_directories, true, false);
                }
            }
            #[cfg(not(windows))]
            emit_link_argument_for_bins(Path::new("-Wl,-Bdynamic"));
        }

        embed_cjk_font(&out_dir);
        #[cfg(windows)]
        embed_windows_icon(repo_root, &out_dir);
        return;
    }

    let configured = env::var_os("PPP_TUI_CORE_PATH").map(PathBuf::from);
    let candidates = [
        repo_root.join("x64/Release/ppp.exe"),
        repo_root.join("Release/ppp.exe"),
        repo_root.join("x64/Debug/ppp.exe"),
        repo_root.join("Debug/ppp.exe"),
    ];

    for candidate in &candidates {
        println!("cargo:rerun-if-changed={}", candidate.display());
    }

    let core_path = match configured {
        Some(path) if path.is_file() => path,
        Some(path) => panic!(
            "PPP_TUI_CORE_PATH does not point to a file: {}",
            path.display()
        ),
        None => candidates
            .into_iter()
            .find(|path| path.is_file())
            .unwrap_or_else(|| {
                panic!("ppp-tui requires an embedded core executable; set PPP_TUI_CORE_PATH to a built ppp.exe")
            }),
    };

    let embedded_path = out_dir.join("ppp-embedded.exe");
    fs::copy(&core_path, &embedded_path).unwrap_or_else(|error| {
        panic!(
            "cannot copy core {} to {}: {error}",
            core_path.display(),
            embedded_path.display()
        )
    });

    println!("cargo:rerun-if-env-changed=PPP_TUI_CORE_PATH");
    println!(
        "cargo:rustc-env=PPP_TUI_EMBEDDED_CORE={}",
        embedded_path.display()
    );
    println!(
        "cargo:rustc-env=PPP_TUI_EMBEDDED_CORE_SOURCE={}",
        core_path.display()
    );

    embed_cjk_font(&out_dir);

    #[cfg(windows)]
    embed_windows_icon(repo_root, &out_dir);
}

fn emit_link_argument_for_bins(path: &Path) {
    for binary in ["ppp-tui", "ppp-tui-cli"] {
        println!("cargo:rustc-link-arg-bin={binary}={}", path.display());
    }
}

fn emit_named_library_for_bins(
    name: &str,
    directories: &[PathBuf],
    _static_library: bool,
    warn_missing: bool,
) {
    if let Some(path) = find_library_file(name, directories) {
        emit_link_argument_for_bins(&path);
        return;
    }

    // System libraries are normally not present in the configured vcpkg or
    // downloaded dependency directories. Keep a linker-native fallback for
    // them, while warning about missing third-party archives.
    #[cfg(windows)]
    {
        let argument = if name.ends_with(".lib") {
            name.to_string()
        } else {
            format!("{name}.lib")
        };
        for binary in ["ppp-tui", "ppp-tui-cli"] {
            println!("cargo:rustc-link-arg-bin={binary}={argument}");
        }
    }
    #[cfg(not(windows))]
    {
        let argument = if _static_library {
            format!("-Wl,-Bstatic,-l{name}")
        } else {
            format!("-l{name}")
        };
        for binary in ["ppp-tui", "ppp-tui-cli"] {
            println!("cargo:rustc-link-arg-bin={binary}={argument}");
        }
    }

    if warn_missing {
        println!(
            "cargo:warning=ppp-tui native library '{name}' was not found in configured directories"
        );
    }
}

fn find_library_file(name: &str, directories: &[PathBuf]) -> Option<PathBuf> {
    let mut candidates = Vec::new();
    for candidate in [
        name.to_string(),
        format!("{name}.lib"),
        format!("{name}.a"),
        format!("lib{name}.lib"),
        format!("lib{name}.a"),
    ] {
        if !candidates.contains(&candidate) {
            candidates.push(candidate);
        }
    }

    for directory in directories {
        for candidate in &candidates {
            let path = directory.join(candidate);
            if path.is_file() {
                return Some(path);
            }
        }
    }
    None
}

fn embed_cjk_font(out_dir: &std::path::Path) {
    let configured = env::var_os("PPP_TUI_CJK_FONT_PATH").map(PathBuf::from);
    let candidates = if let Some(windir) = env::var_os("WINDIR") {
        let fonts = PathBuf::from(windir).join("Fonts");
        vec![
            fonts.join("Noto Sans SC (TrueType).otf"),
            fonts.join("simhei.ttf"),
            fonts.join("msyh.ttc"),
        ]
    } else {
        Vec::new()
    };

    let source = match configured {
        Some(path) if path.is_file() => path,
        Some(path) => panic!(
            "PPP_TUI_CJK_FONT_PATH does not point to a file: {}",
            path.display()
        ),
        None => candidates
            .into_iter()
            .find(|path| path.is_file())
            .unwrap_or_else(|| {
                panic!("Chinese font not found; set PPP_TUI_CJK_FONT_PATH to a .ttf/.otf font")
            }),
    };

    let embedded_path = out_dir.join("ppp-cjk-font");
    fs::copy(&source, &embedded_path).unwrap_or_else(|error| {
        panic!(
            "cannot copy Chinese font {} to {}: {error}",
            source.display(),
            embedded_path.display()
        )
    });
    println!("cargo:rerun-if-changed={}", source.display());
    println!("cargo:rerun-if-env-changed=PPP_TUI_CJK_FONT_PATH");
    println!(
        "cargo:rustc-env=PPP_TUI_CJK_FONT_PATH={}",
        embedded_path.display()
    );
}

#[cfg(windows)]
fn embed_windows_icon(repo_root: &std::path::Path, out_dir: &std::path::Path) {
    let icon_path = repo_root.join("icon.ico");
    if !icon_path.is_file() {
        panic!("Rust TUI icon not found: {}", icon_path.display());
    }
    println!("cargo:rerun-if-changed={}", icon_path.display());

    let rc_path = out_dir.join("ppp-tui-icon.rc");
    let res_path = out_dir.join("ppp-tui-icon.res");
    let icon_for_rc = icon_path.to_string_lossy().replace('\\', "/");
    fs::write(&rc_path, format!("1 ICON \"{icon_for_rc}\"\r\n")).unwrap_or_else(|error| {
        panic!(
            "cannot write Windows icon resource {}: {error}",
            rc_path.display()
        )
    });

    let rc = find_resource_compiler().unwrap_or_else(|| {
        panic!("rc.exe was not found; install the Windows SDK or add its bin directory to PATH")
    });
    let status = Command::new(&rc)
        .current_dir(repo_root)
        .arg("/nologo")
        .arg("/fo")
        .arg(&res_path)
        .arg(&rc_path)
        .status()
        .unwrap_or_else(|error| panic!("failed to run {}: {error}", rc.display()));
    if !status.success() {
        panic!("rc.exe failed while compiling the Rust TUI icon: {status}");
    }

    println!("cargo:rustc-link-arg-bin=ppp-tui={}", res_path.display());
    println!(
        "cargo:rustc-link-arg-bin=ppp-tui-cli={}",
        res_path.display()
    );
}

#[cfg(windows)]
fn find_resource_compiler() -> Option<PathBuf> {
    let mut candidates = Vec::new();

    if let Some(path) = env::var_os("PATH") {
        for directory in env::split_paths(&path) {
            candidates.push(directory.join("rc.exe"));
        }
    }

    let mut sdk_roots = Vec::new();
    if let Some(path) = env::var_os("WindowsSdkDir") {
        sdk_roots.push(PathBuf::from(path).join("bin"));
    }
    if let Some(path) = env::var_os("ProgramFiles(x86)") {
        sdk_roots.push(PathBuf::from(path).join("Windows Kits/10/bin"));
    }
    if let Some(path) = env::var_os("ProgramFiles") {
        sdk_roots.push(PathBuf::from(path).join("Windows Kits/10/bin"));
    }

    for root in sdk_roots {
        if let Ok(versions) = fs::read_dir(root) {
            for version in versions.flatten() {
                let path = version.path();
                candidates.push(path.join("x64/rc.exe"));
                candidates.push(path.join("x86/rc.exe"));
            }
        }
    }

    candidates.into_iter().find(|path| path.is_file())
}
