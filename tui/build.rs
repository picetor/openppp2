use std::env;
use std::fs;
use std::path::PathBuf;
use std::process::Command;

fn main() {
    let manifest_dir = PathBuf::from(env::var_os("CARGO_MANIFEST_DIR").unwrap());
    let repo_root = manifest_dir
        .parent()
        .expect("tui crate must live below the repository root");

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

    let out_dir = PathBuf::from(env::var_os("OUT_DIR").unwrap());
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
