<#
.SYNOPSIS
Feeds vcpkg's download cache through a GitHub mirror.

.DESCRIPTION
vcpkg fetches its tools and port sources from github.com, which can be
unreachable or extremely slow on some networks (measured 14 KB/s direct
against 760 KB/s through a mirror).  This helper repeatedly runs vcpkg
with --only-downloads, reads the 'Downloading <url> -> <file>' lines it
prints, fetches those files through a mirror into vcpkg's downloads
directory, and repeats until vcpkg stops asking for anything new.
vcpkg still verifies every SHA512 itself, so the mirror cannot inject a
different file.  When the loop settles, run the real install (or this
script with -Build).

.PARAMETER Ports
Ports to install; defaults to the set the Windows CI uses.

.PARAMETER Triplet
vcpkg triplet, default x64-windows-static.

.PARAMETER Mirror
URL prefix used for github.com/codeload.github.com downloads.

.PARAMETER Build
After the cache is complete, run the real (building) install.
#>
[CmdletBinding()]
param(
    [string[]] $Ports = @('boost-asio','boost-beast','boost-json','boost-program-options','boost-system',
        'boost-thread','boost-lockfree','boost-uuid','boost-context','boost-coroutine','boost-filesystem',
        'boost-stacktrace','openssl','jemalloc'),
    [string] $Triplet = 'x64-windows-static',
    [string] $Mirror = 'https://gh-proxy.com/',
    [string] $Proxy = 'http://127.0.0.1:8080',
    [string] $VcpkgRoot = 'D:\vcpkg',
    [string] $InstallRoot = 'D:\github\vcpkg_installed',
    [switch] $Build
)

$ErrorActionPreference = 'Stop'
$vcpkg  = Join-Path $VcpkgRoot 'vcpkg.exe'
$cache  = Join-Path $VcpkgRoot 'downloads'
$logDir = Join-Path $env:TEMP 'vcpkg-feed'
New-Item -ItemType Directory -Path $cache, $logDir -Force | Out-Null
if (-not (Test-Path $vcpkg)) { throw "vcpkg.exe not found at $vcpkg" }

function Get-Mirrored([string] $url, [string] $destination) {
    $target = $url
    if ($url -match '^https://(github\.com|codeload\.github\.com|objects\.githubusercontent\.com)/') {
        $target = $Mirror + $url
    }
    Write-Host ("    fetching {0}" -f (Split-Path $destination -Leaf))
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    try {
        # The VPN client's own HTTP proxy (default port 8080) is usually the
        # fastest route to github.com on a throttled network; the mirror is the
        # fallback when no proxy is available.
        $params = @{ Uri = $target; OutFile = $destination; TimeoutSec = 900 }
        if ($Proxy) { $params.Proxy = $Proxy }
        Invoke-WebRequest @params
        $sw.Stop()
        $mb = (Get-Item $destination).Length / 1MB
        Write-Host ("      {0:N2} MB in {1:N0}s ({2:N0} KB/s)" -f $mb, $sw.Elapsed.TotalSeconds, ($mb*1024/$sw.Elapsed.TotalSeconds))
        return $true
    }
    catch {
        Write-Warning ("      failed: {0}" -f $_.Exception.Message)
        Remove-Item $destination -Force -ErrorAction SilentlyContinue
        return $false
    }
}

if ($Proxy) {
    # vcpkg's own downloader honours these, so the real install goes through the
    # proxy as well instead of crawling to github.com directly.
    $env:HTTP_PROXY = $Proxy
    $env:HTTPS_PROXY = $Proxy
    $env:ALL_PROXY = $Proxy
}

$installArgs = @('install') + $Ports + @('--triplet', $Triplet, "--x-install-root=$InstallRoot")
for ($round = 1; $round -le 12; $round++) {
    $log = Join-Path $logDir ("round-{0}.log" -f $round)
    Write-Host ("=== download round {0} ===" -f $round)
    # vcpkg stops making progress when a github download hangs, so give each
    # round a bounded budget and then collect what it asked for.
    $proc = Start-Process -FilePath $vcpkg -ArgumentList ($installArgs + '--only-downloads') -NoNewWindow -PassThru -RedirectStandardOutput $log -RedirectStandardError ($log + '.err')
    $finished = $proc.WaitForExit(180000)
    if (-not $finished) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue; Write-Host '  (round timed out; continuing)' }

    $text = (Get-Content $log -Raw -ErrorAction SilentlyContinue) + (Get-Content ($log + '.err') -Raw -ErrorAction SilentlyContinue)
    $wanted = [regex]::Matches($text, 'Downloading\s+(\S+)\s+->\s+(\S+)')
    $missing = @()
    foreach ($m in $wanted) {
        $url = $m.Groups[1].Value
        $name = Split-Path $m.Groups[2].Value -Leaf
        $dest = Join-Path $cache $name
        if (-not (Test-Path $dest)) { $missing += ,@($url, $dest) }
    }
    Get-ChildItem $cache -Filter '*.part' -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
    Write-Host ("  wanted {0}, missing {1}" -f $wanted.Count, $missing.Count)
    if ($missing.Count -eq 0) {
        if ($finished) { Write-Host '  nothing left to fetch'; break }
        continue
    }
    foreach ($item in $missing) { Get-Mirrored $item[0] $item[1] | Out-Null }
}

if ($Build) {
    Write-Host '=== vcpkg install (building) ==='
    & $vcpkg @installArgs
    exit $LASTEXITCODE
}
