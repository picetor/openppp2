<#
.SYNOPSIS
    Tests whether the tunnel network category is what makes a third-party
    security product (for example Huorong) drop the packets the client injects
    into the TUN adapter.

.DESCRIPTION
    Windows classifies a freshly created tunnel adapter as a Public network.
    Some third-party WFP filter drivers block unsolicited inbound traffic on
    untrusted networks, which breaks the injected DNS answers and ICMP echo
    replies even though Windows Firewall allow rules are present (those are
    enforced at the ALE layer and cannot override another WFP provider).

    This script flips the tunnel adapter to Private, runs the probe, and reports
    a verdict.  Without -Keep it restores the original category automatically
    when the change does not help, so the experiment is safe.

.PARAMETER InterfaceIndex
    Tunnel adapter index.  Auto-detected when omitted.

.PARAMETER Keep
    Keep the Private category when the probe improves.

.EXAMPLE
    powershell -File tests/tools/Try-TunNetworkZone.ps1
    powershell -File tests/tools/Try-TunNetworkZone.ps1 -Keep
#>
[CmdletBinding()]
param(
    [int] $InterfaceIndex = 0,
    [switch] $Keep
)

$ErrorActionPreference = 'Continue'

function Get-TunIndex {
    if ($InterfaceIndex -gt 0) { return $InterfaceIndex }
    # Leftover TAP adapters from earlier sessions enumerate first, so pick the
    # adapter that is actually up (preferring Wintun, then the best metric).
    $candidates = New-Object System.Collections.ArrayList
    foreach ($ip in (Get-NetIPConfiguration -ErrorAction SilentlyContinue)) {
        $a = Get-NetAdapter -InterfaceIndex $ip.InterfaceIndex -ErrorAction SilentlyContinue
        if (-not $a) { continue }
        if ($a.InterfaceDescription -notmatch 'Wintun|TAP-Windows|PPP PRIVATE NETWORK') { continue }
        if (-not ($ip.IPv4Address -and $ip.IPv4DefaultGateway)) { continue }
        $metric = (Get-NetIPInterface -InterfaceIndex $ip.InterfaceIndex -AddressFamily IPv4 -ErrorAction SilentlyContinue).InterfaceMetric
        if (-not $metric) { $metric = 9999 }
        [void]$candidates.Add([pscustomobject]@{
                Index  = $ip.InterfaceIndex
                Up     = ($a.Status -eq 'Up')
                Wintun = ($a.InterfaceDescription -match 'Wintun')
                Metric = [int]$metric
            })
    }
    $pick = $candidates | Where-Object { $_.Up } |
        Sort-Object -Property @{ Expression = { $_.Wintun }; Descending = $true }, Metric |
        Select-Object -First 1
    if (-not $pick) {
        $pick = $candidates | Sort-Object Metric | Select-Object -First 1
    }
    if ($pick) { return $pick.Index }
    return 0
}

function Get-ProbeVerdict {
    $probe = Join-Path $PSScriptRoot 'TunInboundProbe.ps1'
    if (-not (Test-Path $probe)) { return $null }
    $out = & powershell -NoProfile -ExecutionPolicy Bypass -File $probe 2>&1
    $out | ForEach-Object { Write-Output ("  {0}" -f $_) }
    $line = $out | Where-Object { $_ -match 'injected into the TUN' } | Select-Object -First 1
    if ($line -match 'replies\s+(\d+)/(\d+)') {
        return [pscustomobject]@{ Replies = [int]$Matches[1]; Total = [int]$Matches[2]; Line = $line }
    }
    return $null
}

$isAdmin = ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()
    ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

$index = Get-TunIndex
if ($index -le 0) {
    Write-Output 'No active tunnel adapter found. Start the TUN client first.'
    return
}

$profile = Get-NetConnectionProfile -InterfaceIndex $index -ErrorAction SilentlyContinue
$alias = (Get-NetAdapter -InterfaceIndex $index -ErrorAction SilentlyContinue).Name
$original = if ($profile) { [string]$profile.NetworkCategory } else { 'Unknown' }
Write-Output ("tunnel adapter : ifIndex {0} '{1}', network category = {2}" -f $index, $alias, $original)

if (-not $isAdmin) {
    Write-Output ''
    Write-Output 'This test changes the adapter network category, so it needs an elevated PowerShell.'
    Write-Output ("Run:  powershell -File `"{0}`" -Keep" -f $PSCommandPath)
    return
}

Write-Output ''
Write-Output '=== probe with the current category ==='
$before = Get-ProbeVerdict

Write-Output ''
Write-Output ("=== switching ifIndex {0} to Private ===" -f $index)
try {
    Set-NetConnectionProfile -InterfaceIndex $index -NetworkCategory Private -ErrorAction Stop
    Write-Output '  category is now Private'
}
catch {
    Write-Output ("  FAILED: {0}" -f $_.Exception.Message)
    return
}

Write-Output ''
Write-Output '=== probe with Private ==='
$after = Get-ProbeVerdict

Write-Output ''
if ($after -and $after.Replies -eq $after.Total) {
    Write-Output 'Verdict: SUCCESS - the network category was the trigger.'
    Write-Output '         The client can set this itself at takeover, so the tunnel then works'
    Write-Output '         with the security product fully enabled and no per-app exception.'
    if (-not $Keep) { Write-Output '         (category left as Private; pass -Keep to silence this note)' }
}
else {
    $nowReplies = if ($after) { $after.Replies } else { 'n/a' }
    $wasReplies = if ($before) { $before.Replies } else { 'n/a' }
    Write-Output ("Verdict: NOT the trigger (replies {0} -> {1})." -f $wasReplies, $nowReplies)
    Write-Output '         The security product is filtering for another reason, so it needs a'
    Write-Output '         per-app / per-network exception, or the injected path must be avoided'
    Write-Output '         (DNS already is: the TUN resolver lists the loopback proxy first).'
    try {
        Set-NetConnectionProfile -InterfaceIndex $index -NetworkCategory $original -ErrorAction Stop
        Write-Output ("  restored the category to {0}" -f $original)
    }
    catch {
        Write-Output ("  could not restore the category ({0}); set it back manually" -f $_.Exception.Message)
    }
}
