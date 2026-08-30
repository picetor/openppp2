param(
    [ValidateSet("Debug", "Release")]
    [string] $CoreConfiguration = "Release",
    [string] $CoreLibraryPath
)

$ErrorActionPreference = "Stop"

$tuiDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoDir = Split-Path -Parent $tuiDir

if ([string]::IsNullOrWhiteSpace($CoreLibraryPath)) {
    $candidate = Join-Path $repoDir ("x64\{0}\ppp-core.lib" -f $CoreConfiguration)
    if (Test-Path -LiteralPath $candidate) {
        $CoreLibraryPath = $candidate
    }
}

if ([string]::IsNullOrWhiteSpace($CoreLibraryPath) -or -not (Test-Path -LiteralPath $CoreLibraryPath)) {
    throw "Static core library not found. Build x64\$CoreConfiguration\ppp-core.lib first or pass -CoreLibraryPath."
}

$CoreLibraryPath = (Resolve-Path -LiteralPath $CoreLibraryPath).Path
$tripletCandidates = @(
    (Join-Path $repoDir "vcpkg_installed\x64-windows-static\lib"),
    (Join-Path (Split-Path -Parent $repoDir) "vcpkg_installed\x64-windows-static\lib")
)
$triplet = $tripletCandidates |
    Where-Object { Test-Path -LiteralPath $_ } |
    Select-Object -First 1
if ([string]::IsNullOrWhiteSpace($triplet)) {
    throw "Static dependency directory not found: vcpkg_installed\x64-windows-static\lib"
}
$boostSuffix = "x64"
$env:PPP_TUI_CORE_LIB = $CoreLibraryPath
$env:PPP_TUI_CORE_LIB_DIRS = "$triplet;$([System.IO.Path]::GetDirectoryName($CoreLibraryPath))"
$env:PPP_TUI_CORE_LIBS = "libssl;libcrypto;jemalloc_s;boost_context-vc145-mt-$boostSuffix-1_91;boost_coroutine-vc145-mt-$boostSuffix-1_91;boost_thread-vc145-mt-$boostSuffix-1_91;boost_filesystem-vc145-mt-$boostSuffix-1_91"
$env:PPP_TUI_CORE_SYSTEM_LIBS = "ws2_32;iphlpapi;shlwapi;qwave;pdh;winmm;wbemuuid;shell32;crypt32;propsys;dbghelp;rpcrt4;ole32;comsuppw;setupapi;fwpuclnt;netapi32;wininet;cryptui;advapi32;secur32;bcrypt;psapi"
Write-Host "Linking in-process core: $CoreLibraryPath"

cargo build --release --manifest-path (Join-Path $tuiDir "Cargo.toml")

$output = Join-Path $tuiDir "target\release\ppp-tui.exe"
Write-Host "Standalone Rust TUI: $output"
