<#
.SYNOPSIS
Builds the Windows client locally, the way .github/workflows/windows.yml does.

.DESCRIPTION
Reproduces the CI toolchain on this machine:
  * Visual Studio 2026 Build Tools (toolset v145, Windows SDK 10.0.26100)
  * vcpkg pinned to the CI commit, manifest disabled, static triplet
  * dependencies installed into the directory ppp.vcxproj expects
    (ProjectDir..\vcpkg_installed\<triplet>)

.PARAMETER Triplet
vcpkg triplet: x64-windows-static (default) or x86-windows-static.

.PARAMETER SkipDependencies
Only compile; assume the vcpkg tree is already installed.

.PARAMETER Clean
Run the Clean target first.
#>
[CmdletBinding()]
param(
    [string] $Triplet = 'x64-windows-static',
    [switch] $SkipDependencies,
    [switch] $Web,
    [switch] $Clean
)

$ErrorActionPreference = 'Stop'
$repo        = Split-Path $PSScriptRoot -Parent
$vcpkgRoot   = if ($env:VCPKG_ROOT) { $env:VCPKG_ROOT } else { 'D:\vcpkg' }
$installRoot = Join-Path (Split-Path $repo -Parent) 'vcpkg_installed'
$platform    = if ($Triplet -like 'x86-*') { 'Win32' } else { 'x64' }
$arch        = if ($platform -eq 'Win32') { 'x86' } else { 'x64' }

Write-Host "repo        : $repo"
Write-Host "vcpkg       : $vcpkgRoot"
Write-Host "install root: $installRoot"
Write-Host "triplet     : $Triplet (platform $platform)"

$vswhere = Join-Path (${env:ProgramFiles(x86)}) 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { throw 'vswhere not found; install the VS Build Tools first' }
$vsRoot = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsRoot) { throw 'Visual C++ tools not found' }
$devCmd = Join-Path $vsRoot 'Common7\Tools\VsDevCmd.bat'
Write-Host "VS          : $vsRoot"

if (-not $SkipDependencies) {
    $vcpkgExe = Join-Path $vcpkgRoot 'vcpkg.exe'
    if (-not (Test-Path $vcpkgExe)) { throw "vcpkg.exe not found at $vcpkgExe" }
    $packages = @(
        'boost-asio', 'boost-beast', 'boost-json', 'boost-program-options', 'boost-system',
        'boost-thread', 'boost-lockfree', 'boost-uuid', 'boost-context', 'boost-coroutine',
        'boost-filesystem', 'boost-stacktrace', 'openssl', 'jemalloc')
    Write-Host '=== vcpkg install ==='
    & $vcpkgExe install @packages --triplet $Triplet "--x-install-root=$installRoot"
    if ($LASTEXITCODE -ne 0) { throw "vcpkg install failed with exit code $LASTEXITCODE" }
}

# --- stage the dependencies the way the project expects them ---------------
# ppp.vcxproj links fixed boost names (vc145 ... 1_91) while the pinned vcpkg
# registry now ships 1.92, so the CI normalizes one unnamed candidate per
# component.  Same here, otherwise the link fails with LNK1104.
$tripletDir = Join-Path $installRoot $Triplet
$libDir     = Join-Path $tripletDir 'lib'
if (-not (Test-Path $libDir)) { throw "dependency tree missing: $libDir" }
$archSuffix = if ($platform -eq 'Win32') { 'x32' } else { 'x64' }
foreach ($component in 'context', 'coroutine', 'thread', 'filesystem') {
    $expectedName = "boost_$component-vc145-mt-$archSuffix-1_91.lib"
    $expected     = Join-Path $libDir $expectedName
    if (Test-Path $expected) { continue }
    $candidates = @(Get-ChildItem -LiteralPath $libDir -File -Filter "boost_$component-*.lib")
    if ($candidates.Count -eq 1) {
        Write-Host "  normalizing $($candidates[0].Name) -> $expectedName"
        Copy-Item -LiteralPath $candidates[0].FullName -Destination $expected -Force
    }
    else {
        throw "cannot satisfy $expectedName ($($candidates.Count) candidates in $libDir)"
    }
}

# The link step copies the CA bundle into the output directory.
$caBundle = Join-Path $repo 'cacert.pem'
if (-not (Test-Path $caBundle)) {
    Write-Host '  fetching cacert.pem'
    $fetch = @{ Uri = 'https://curl.se/ca/cacert.pem'; OutFile = $caBundle; TimeoutSec = 120 }
    if ($env:HTTPS_PROXY) { $fetch.Proxy = $env:HTTPS_PROXY }
    Invoke-WebRequest @fetch
}

# The resource pipeline expects these, exactly as the CI provides them.
foreach ($placeholder in 'starrylink.net.key', 'starrylink.net.pem') {
    $path = Join-Path $repo $placeholder
    if (-not (Test-Path $path)) { '# placeholder' | Out-File -FilePath $path -Encoding ascii }
}

$targets = if ($Clean) { '/t:Clean;Build' } else { '/t:Build' }
$project = Join-Path $repo 'ppp.vcxproj'
$cmd = 'call "' + $devCmd + '" -arch=' + $arch + ' -host_arch=x64 >nul && msbuild "' + $project +
       '" /p:Configuration=Release /p:Platform=' + $platform + ' ' + $targets +
       ' /p:VcpkgEnableManifest=false /m'
Write-Host '=== msbuild ==='
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw "msbuild failed with exit code $LASTEXITCODE" }

$exe = Join-Path $repo ($platform + '\Release\ppp.exe')
if (Test-Path $exe) { Write-Host "built: $exe" } else { Write-Warning "ppp.exe not found under $platform\Release" }

# --- web host (ppp-web.exe) -------------------------------------------------
# The GUI host links the core as a static library, exactly like the CI does
# before it runs web\build.ps1 -SkipCore.
if ($Web) {
    Write-Host '=== core as a static library ==='
    $coreLib = 'call "' + $devCmd + '" -arch=' + $arch + ' -host_arch=x64 >nul && msbuild "' +
               (Join-Path $repo 'ppp.vcxproj') + '" /t:Build /p:Configuration=Release /p:Platform=' + $platform +
               ' /p:BuildCoreLibrary=true /p:ConfigurationType=StaticLibrary /p:VcpkgEnableManifest=false /p:PreferredToolArchitecture=x64 /m /v:minimal'
    cmd /c $coreLib
    if ($LASTEXITCODE -ne 0) { throw "core static library build failed with exit code $LASTEXITCODE" }

    Write-Host '=== web host ==='
    # Not $web: that name would collide with the -Web switch parameter
    # (PowerShell variable names are case insensitive).
    $webCmd = 'call "' + $devCmd + '" -arch=' + $arch + ' -host_arch=x64 >nul && msbuild "' +
              (Join-Path $repo 'web\ppp-web.vcxproj') + '" /t:Build /p:Configuration=Release /p:Platform=' + $platform +
              ' /p:PreferredToolArchitecture=x64 /m /v:minimal'
    cmd /c $webCmd
    if ($LASTEXITCODE -ne 0) { throw "web host build failed with exit code $LASTEXITCODE" }

    $gui = Join-Path $repo 'web\dist\ppp-web.exe'
    if (Test-Path $gui) { Write-Host "built: $gui" } else { Write-Warning 'ppp-web.exe was not produced' }
}
