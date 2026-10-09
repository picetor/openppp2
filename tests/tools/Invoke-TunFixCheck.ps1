<#
.SYNOPSIS
    Applies (or rolls back) the two host-side changes that make the openppp2 TUN
    usable, then verifies the result.

.DESCRIPTION
    The openppp2 TUN client injects its replies (DNS answers, ICMP echo replies)
    into the tunnel adapter from userspace.  Windows Firewall drops those packets
    because no enabled inbound rule matches traffic that belongs to no process,
    so the host never sees a DNS answer or a ping reply.

    This script performs two independent, reversible changes:

      1. Firewall: add inbound Allow rules for UDP and ICMPv4 scoped to the
         tunnel subnet (exactly what the client code does by itself once built).
      2. DNS: point the tunnel adapter's resolver at the client's loopback DNS
         proxy (127.0.0.1:53) instead of the virtual gateway.  The loopback proxy
         answers over loopback, which the firewall never blocks, so name
         resolution works immediately without waiting for a rebuild.

    After applying, it runs the probe plus ping/nslookup so the result is visible.
    "-Rollback" removes the test rules and restores the tunnel gateway as the
    adapter resolver.

.PARAMETER Apply
    Make the changes (requires an elevated PowerShell).

.PARAMETER Rollback
    Undo the changes (requires an elevated PowerShell).

.PARAMETER SkipDns
    Do not touch the adapter resolver; only handle the firewall rules.

.EXAMPLE
    powershell -File tests/tools/Invoke-TunFixCheck.ps1 -Apply
    powershell -File tests/tools/Invoke-TunFixCheck.ps1 -Rollback
#>
[CmdletBinding()]
param(
    [switch] $Apply,
    [switch] $Rollback,
    [switch] $SkipDns,
    # The core re-applies its own TUN resolver list every 30 seconds, which
    # reverts a manual change.  When > 0, a temporary background loop re-asserts
    # 127.0.0.1 on the tunnel adapter every 10 seconds for this many seconds, so
    # name resolution keeps working on the loopback proxy without a rebuild.
    [int] $KeepDnsSeconds = 0
)

$ErrorActionPreference = 'Continue'

$ruleNames = @('openppp2 TUN test UDP', 'openppp2 TUN test ICMPv4')
$dnsBackupPath = Join-Path $env:TEMP 'openppp2-tun-dns-backup.txt'
$dnsKeepJobName = 'openppp2-tun-dns-keepalive'

function Get-TunAdapter {
    # Leftover TAP adapters from earlier sessions enumerate first, so pick the
    # adapter that is actually up (preferring Wintun, then the best metric).
    $candidates = New-Object System.Collections.ArrayList
    foreach ($cfg in (Get-NetIPConfiguration -ErrorAction SilentlyContinue)) {
        $a = Get-NetAdapter -InterfaceIndex $cfg.InterfaceIndex -ErrorAction SilentlyContinue
        if (-not $a) { continue }
        if ($a.InterfaceDescription -notmatch 'PPP PRIVATE NETWORK|TAP-Windows|Wintun') { continue }
        if (-not ($cfg.IPv4Address -and $cfg.IPv4DefaultGateway)) { continue }
        $metric = (Get-NetIPInterface -InterfaceIndex $cfg.InterfaceIndex -AddressFamily IPv4 -ErrorAction SilentlyContinue).InterfaceMetric
        if (-not $metric) { $metric = 9999 }
        [void]$candidates.Add([pscustomobject]@{
                Index   = $cfg.InterfaceIndex
                Name    = $cfg.InterfaceAlias
                Address = [string]$cfg.IPv4Address.IPAddress
                Gateway = [string]$cfg.IPv4DefaultGateway.NextHop
                Up      = ($a.Status -eq 'Up')
                Wintun  = ($a.InterfaceDescription -match 'Wintun')
                Metric  = [int]$metric
            })
    }
    $pick = $candidates | Where-Object { $_.Up } |
        Sort-Object -Property @{ Expression = { $_.Wintun }; Descending = $true }, Metric |
        Select-Object -First 1
    if (-not $pick) {
        $pick = $candidates | Sort-Object Metric | Select-Object -First 1
    }
    return $pick
}

function Show-DnsState([string] $Label) {
    Write-Output ""
    Write-Output "== $Label =="
    foreach ($i in @(71, 15, 24, 17, 20, 13, 18)) {
        $dns = Get-DnsClientServerAddress -InterfaceIndex $i -ErrorAction SilentlyContinue |
            Where-Object AddressFamily -eq 2
        if ($dns) {
            $alias = (Get-NetAdapter -InterfaceIndex $i -ErrorAction SilentlyContinue).Name
            Write-Output ("  ifIndex {0,-3} {1,-28} DNS = {2}" -f $i, $alias, ($dns.ServerAddresses -join ', '))
        }
    }
}

function Invoke-Probe {
    $probe = Join-Path $PSScriptRoot 'TunInboundProbe.ps1'
    if (Test-Path $probe) {
        Write-Output ""
        Write-Output '== probe =='
        & powershell -NoProfile -ExecutionPolicy Bypass -File $probe
    }
    Write-Output ""
    Write-Output '== ping tunnel gateway =='
    if ($script:tun) { ping -n 3 -w 1500 $script:tun.Gateway | Select-String 'Packets|Lost|Reply' }
    Write-Output '== nslookup www.baidu.com =='
    # Capture the native stderr as text; otherwise PowerShell records the
    # expected timeout as an error and the script exits non-zero.
    & cmd.exe /c 'nslookup www.baidu.com 2>&1' | Select-Object -First 8
}

$isAdmin = ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()
    ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

$script:tun = Get-TunAdapter
if (-not $script:tun) {
    Write-Output 'No active tunnel adapter found. Start the TUN client first.'
    return
}

Write-Output ("tunnel adapter : ifIndex {0} '{1}' addr={2} gw={3}" -f
    $script:tun.Index, $script:tun.Name, $script:tun.Address, $script:tun.Gateway)
$subnet = ($script:tun.Address -replace '\.\d+$', '.0') + '/24'
Write-Output ("firewall scope : {0}" -f $subnet)

if ($Rollback) {
    if (-not $isAdmin) { Write-Output '-Rollback requires an elevated PowerShell.'; return }
    Write-Output ''
    Write-Output '== rollback: removing test rules and restoring the tunnel resolver =='
    $keepJob = Get-Job -Name $dnsKeepJobName -ErrorAction SilentlyContinue
    if ($keepJob) {
        $keepJob | Stop-Job -ErrorAction SilentlyContinue
        $keepJob | Remove-Job -ErrorAction SilentlyContinue
        Write-Output ("  stopped the temporary DNS re-assert loop ('{0}')" -f $dnsKeepJobName)
    }
    foreach ($n in $ruleNames) {
        Remove-NetFirewallRule -DisplayName $n -ErrorAction SilentlyContinue
        Write-Output ("  removed rule '{0}' (if present)" -f $n)
    }
    if (Test-Path $dnsBackupPath) {
        $restore = (Get-Content $dnsBackupPath -Raw).Trim()
        if ($restore) {
            Set-DnsClientServerAddress -InterfaceIndex $script:tun.Index -ServerAddresses $restore -ErrorAction SilentlyContinue
            Write-Output ("  restored adapter resolver to {0}" -f $restore)
        }
        Remove-Item $dnsBackupPath -ErrorAction SilentlyContinue
    }
    else {
        Write-Output '  no DNS backup found; leaving the adapter resolver untouched'
    }
    Show-DnsState 'After rollback'
    return
}

if (-not $Apply) {
    Write-Output ''
    Write-Output 'Report only. Apply with:'
    Write-Output '  powershell -File tests/tools/Invoke-TunFixCheck.ps1 -Apply      (elevated)'
    Write-Output 'Roll back with:'
    Write-Output '  powershell -File tests/tools/Invoke-TunFixCheck.ps1 -Rollback   (elevated)'
    Invoke-Probe
    return
}

if (-not $isAdmin) { Write-Output '-Apply requires an elevated PowerShell.'; return }

Write-Output ''
Write-Output '== 1/2 firewall: allow inbound UDP/ICMPv4 on the tunnel subnet =='
foreach ($spec in @(
        @{ Name = $ruleNames[0]; Proto = 'UDP' },
        @{ Name = $ruleNames[1]; Proto = 'ICMPv4' })) {
    Remove-NetFirewallRule -DisplayName $spec.Name -ErrorAction SilentlyContinue
    try {
        New-NetFirewallRule -DisplayName $spec.Name -Direction Inbound -Action Allow `
            -Protocol $spec.Proto -InterfaceAlias $script:tun.Name -LocalAddress $subnet -Profile Any `
            -ErrorAction Stop | Out-Null
        Write-Output ("  created '{0}'" -f $spec.Name)
    }
    catch {
        Write-Output ("  FAILED '{0}': {1}" -f $spec.Name, $_.Exception.Message)
    }
}

if (-not $SkipDns) {
    Write-Output ''
    Write-Output '== 2/2 DNS: point the tunnel adapter resolver at the loopback proxy =='
    $current = (Get-DnsClientServerAddress -InterfaceIndex $script:tun.Index -AddressFamily IPv4 -ErrorAction SilentlyContinue).ServerAddresses
    if ($current) { Set-Content -Path $dnsBackupPath -Value ($current -join ',') -Encoding ASCII }
    try {
        Set-DnsClientServerAddress -InterfaceIndex $script:tun.Index -ServerAddresses 127.0.0.1 -ErrorAction Stop
        Write-Output ("  adapter resolver now 127.0.0.1 (previous: {0})" -f ($current -join ', '))
        Write-Output ("  backup saved to {0}" -f $dnsBackupPath)
    }
    catch {
        Write-Output ("  FAILED to set the adapter resolver: {0}" -f $_.Exception.Message)
    }
    Clear-DnsClientCache

    if ($KeepDnsSeconds -gt 0) {
        Get-Job -Name $dnsKeepJobName -ErrorAction SilentlyContinue | Stop-Job -ErrorAction SilentlyContinue
        Get-Job -Name $dnsKeepJobName -ErrorAction SilentlyContinue | Remove-Job -ErrorAction SilentlyContinue
        $tunIndex = $script:tun.Index
        [void](Start-Job -Name $dnsKeepJobName -ArgumentList $tunIndex, $KeepDnsSeconds -ScriptBlock {
            param($index, $seconds)
            $deadline = (Get-Date).AddSeconds($seconds)
            while ((Get-Date) -lt $deadline) {
                try {
                    Set-DnsClientServerAddress -InterfaceIndex $index -ServerAddresses 127.0.0.1 -ErrorAction Stop
                }
                catch { }
                Start-Sleep -Seconds 10
            }
        })
        Write-Output ("  started a temporary re-assert loop (every 10 s for {0} s, job '{1}'):" -f
            $KeepDnsSeconds, $dnsKeepJobName)
        Write-Output '    the client re-applies its own resolver list every 30 s, so this keeps 127.0.0.1 listed'
    }
}

Show-DnsState 'After apply'
Invoke-Probe
Write-Output ''
Write-Output 'Expected: probe shows replies 6/6 on both rows, ping replies, and nslookup resolves.'
