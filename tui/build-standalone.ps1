param(
    [ValidateSet("Debug", "Release")]
    [string] $CoreConfiguration = "Release",
    [string] $CorePath,
    [string] $CoreLibraryPath
)

$ErrorActionPreference = "Stop"

$tuiDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoDir = Split-Path -Parent $tuiDir

if ([string]::IsNullOrWhiteSpace($CoreLibraryPath) -and [string]::IsNullOrWhiteSpace($CorePath)) {
    $candidate = Join-Path $repoDir ("x64\{0}\ppp-core.lib" -f $CoreConfiguration)
    if (Test-Path -LiteralPath $candidate) {
        $CoreLibraryPath = $candidate
    }
}

if (-not [string]::IsNullOrWhiteSpace($CoreLibraryPath)) {
    $CoreLibraryPath = (Resolve-Path -LiteralPath $CoreLibraryPath).Path
    $triplet = Join-Path $repoDir "vcpkg_installed\x64-windows-static\lib"
    $boostSuffix = "x64"
    $env:PPP_TUI_CORE_LIB = $CoreLibraryPath
    $env:PPP_TUI_CORE_LIB_DIRS = "$triplet;$([System.IO.Path]::GetDirectoryName($CoreLibraryPath))"
    $env:PPP_TUI_CORE_LIBS = "libssl;libcrypto;jemalloc_s;boost_context-vc145-mt-$boostSuffix-1_91;boost_coroutine-vc145-mt-$boostSuffix-1_91;boost_thread-vc145-mt-$boostSuffix-1_91;boost_filesystem-vc145-mt-$boostSuffix-1_91"
    $env:PPP_TUI_CORE_SYSTEM_LIBS = "ws2_32;iphlpapi;shlwapi;qwave;pdh;winmm;wbemuuid;shell32;crypt32;propsys;dbghelp;rpcrt4;ole32;comsuppw;setupapi;fwpuclnt;netapi32;wininet;cryptui;advapi32;secur32;bcrypt;psapi"
    Remove-Item Env:PPP_TUI_CORE_PATH -ErrorAction SilentlyContinue
    Write-Host "Linking in-process core: $CoreLibraryPath"
} else {
    if ([string]::IsNullOrWhiteSpace($CorePath)) {
        $CorePath = Join-Path $repoDir ("x64\{0}\ppp.exe" -f $CoreConfiguration)
    }

    $CorePath = (Resolve-Path -LiteralPath $CorePath).Path
    $coreText = [System.Text.Encoding]::ASCII.GetString(
        [System.IO.File]::ReadAllBytes($CorePath)
    )
    if (-not $coreText.Contains("--headless") -or -not $coreText.Contains("RPC_LISTEN=")) {
        throw "Core does not contain the headless/RPC implementation: $CorePath. Rebuild the current C++ core first."
    }

    $env:PPP_TUI_CORE_PATH = $CorePath
    Write-Host "Using compatibility external core: $CorePath"
}

cargo build --release --manifest-path (Join-Path $tuiDir "Cargo.toml")

$output = Join-Path $tuiDir "target\release\ppp-tui.exe"
Write-Host "Standalone Rust TUI: $output"
