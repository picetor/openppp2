param(
    [ValidateSet("Debug", "Release")]
    [string] $CoreConfiguration = "Release",
    [string] $CorePath
)

$ErrorActionPreference = "Stop"

$tuiDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoDir = Split-Path -Parent $tuiDir

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

Write-Host "Embedding core: $CorePath"
cargo build --release --manifest-path (Join-Path $tuiDir "Cargo.toml")

$output = Join-Path $tuiDir "target\release\ppp-tui.exe"
Write-Host "Standalone Rust TUI: $output"
