<#
.SYNOPSIS
    Reports (and optionally removes) leftover TAP-Windows adapters from earlier
    openppp2 tunnel sessions.

.DESCRIPTION
    Every tunnel session can leave its own TAP-Windows adapter behind.  Each one
    keeps:
      * an IPv4 address in its own /24 plus a stale 0.0.0.0/0 route, and
      * an IPv6 ULA address (fd00::...)

    The stale default routes are harmless while the active tunnel adapter has the
    better metric, but the stale IPv6 addresses make Windows pick a bogus source
    address for IPv6 destinations, so applications that follow AAAA records
    stall instead of failing over to IPv4 quickly.

    The script only ever touches TAP-Windows adapters that are NOT up, and it
    never touches the active Wintun adapter.  Without -Apply it only reports.

.PARAMETER Apply
    Actually remove the leftover adapters (requires an elevated PowerShell).

.PARAMETER Keep
    Adapter names (aliases) to keep even if they are not up.

.EXAMPLE
    powershell -File tests/tools/Remove-StaleTunAdapters.ps1
    powershell -File tests/tools/Remove-StaleTunAdapters.ps1 -Apply
#>
[CmdletBinding()]
param(
    [switch] $Apply,
    [string[]] $Keep = @()
)

$ErrorActionPreference = 'Stop'

function Show-State([string] $Label) {
    Write-Output ""
    Write-Output "== $Label =="
    Get-NetAdapter -IncludeHidden -ErrorAction SilentlyContinue |
        Where-Object { $_.InterfaceDescription -match 'TAP-Windows|Wintun' } |
        Sort-Object ifIndex |
        Select-Object ifIndex, Name, InterfaceDescription, Status |
        Format-Table -AutoSize | Out-String | Write-Output

    Write-Output "IPv4 default routes:"
    Get-NetRoute -AddressFamily IPv4 -DestinationPrefix '0.0.0.0/0' -ErrorAction SilentlyContinue |
        Select-Object InterfaceIndex, InterfaceAlias, NextHop, RouteMetric |
        Format-Table -AutoSize | Out-String | Write-Output

    try {
        $sock = New-Object System.Net.Sockets.Socket(
            [System.Net.Sockets.AddressFamily]::InterNetworkV6,
            [System.Net.Sockets.SocketType]::Dgram,
            [System.Net.Sockets.ProtocolType]::Udp)
        $sock.Connect((New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Parse('2606:4700:4700::1111'), 80)))
        Write-Output ("IPv6 source chosen for a global destination: {0}" -f $sock.LocalEndPoint)
        $sock.Close()
    }
    catch {
        Write-Output ("IPv6 source probe failed: {0}" -f $_.Exception.Message)
    }
}

$isAdmin = ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()
    ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

Show-State 'Before'

$stale = @(Get-NetAdapter -IncludeHidden -ErrorAction SilentlyContinue |
    Where-Object {
        $_.InterfaceDescription -match 'TAP-Windows' -and
        $_.Status -ne 'Up' -and
        ($Keep -notcontains $_.Name)
    })

Write-Output ""
if ($stale.Count -eq 0) {
    Write-Output 'No leftover TAP-Windows adapters found.'
    return
}

Write-Output ("Leftover TAP-Windows adapters ({0}):" -f $stale.Count)
$stale | Select-Object ifIndex, Name, Status, MacAddress | Format-Table -AutoSize | Out-String | Write-Output

if (-not $Apply) {
    Write-Output 'Dry run only. Re-run with -Apply from an elevated PowerShell to remove them.'
    return
}

if (-not $isAdmin) {
    Write-Error 'Removing adapters requires an elevated PowerShell (run as administrator).'
}

foreach ($adapter in $stale) {
    Write-Output ("Removing '{0}' ({1}) ..." -f $adapter.Name, $adapter.InterfaceDescription)
    try {
        Remove-PnpDevice -InstanceId $adapter.PnPDeviceID -Confirm:$false -ErrorAction Stop
        Write-Output '  removed'
    }
    catch {
        Write-Output ("  failed: {0}" -f $_.Exception.Message)
    }
}

Show-State 'After'
Write-Output 'Remaining adapters should be the active tunnel adapter plus the physical NIC.'
