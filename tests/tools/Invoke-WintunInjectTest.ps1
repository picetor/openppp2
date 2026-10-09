<#
.SYNOPSIS
    Injects a hand-built IPv4/UDP packet into a Wintun adapter and reports whether
    the host network stack actually received it.

.DESCRIPTION
    The openppp2 TUN client delivers everything it receives from the tunnel by
    writing packets into the Wintun ring buffer from userspace.  When the host
    stack discards those packets (Windows Firewall allow rules cannot override
    such a drop, and no WFP filter at the inbound transport layer is involved),
    DNS answers and ICMP replies never reach the host even though the client
    logs a successful injection.

    This probe reproduces that path independently of the client:

      1. opens the Wintun adapter and starts a session,
      2. binds a UDP socket on the host to the tunnel address,
      3. builds a complete IPv4/UDP packet (correct IP and UDP checksums) from
         the tunnel gateway to that socket,
      4. injects it with WintunSendPacket,
      5. reports whether the socket received it, together with the WFP
         inbound-transport discard delta.

    The switches below let you bisect which header field matters (-Tos, -Id,
    -NoDf, -BadUdpChecksum), because the client's own packets use tos=0x68,
    id=0, DF=1 and a computed UDP checksum.

.PARAMETER AdapterName
    Wintun adapter name.  When omitted, a few candidates are tried.

.PARAMETER Count
    How many packets to inject (default 3).

.PARAMETER Quiet
    Only print the verdict line.

.EXAMPLE
    powershell -File tests/tools/Invoke-WintunInjectTest.ps1
    powershell -File tests/tools/Invoke-WintunInjectTest.ps1 -Tos 0 -Id 4321 -NoDf
    powershell -File tests/tools/Invoke-WintunInjectTest.ps1 -BadUdpChecksum
#>
[CmdletBinding()]
param(
    [string] $AdapterName,
    [string] $AdapterAddress,
    [string] $Gateway,
    [int] $Count = 3,
    [int] $TimeoutMs = 1500,
    [int] $Tos = 0x68,
    [int] $Id = 0,
    [switch] $NoDf,
    [switch] $BadUdpChecksum,
    [string] $PayloadText = 'openppp2-inject-probe',
    [switch] $Quiet
)

$ErrorActionPreference = 'Stop'

$dllCandidates = @(@(
        'C:\Users\2233\Desktop\TUI\Driver\x64\wintun.dll',
        (Join-Path $PSScriptRoot 'wintun.dll')
    ) | Where-Object { Test-Path $_ })
if (-not $dllCandidates) { Write-Output 'wintun.dll not found.'; return }
$dllPath = $dllCandidates[0]
# The runtime resolves "wintun.dll" through the application/current directory, so
# switch to the folder that ships it for the lifetime of this probe.
$dllDir = Split-Path $dllPath -Parent
[System.IO.Directory]::SetCurrentDirectory($dllDir)

if (-not ('WintunProbe' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class WintunProbe
{
    [DllImport("wintun.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr WintunOpenAdapter(string name);

    [DllImport("wintun.dll", SetLastError = true)]
    public static extern void WintunCloseAdapter(IntPtr adapter);

    [DllImport("wintun.dll", SetLastError = true)]
    public static extern IntPtr WintunStartSession(IntPtr adapter, uint capacity);

    [DllImport("wintun.dll", SetLastError = true)]
    public static extern void WintunEndSession(IntPtr session);

    [DllImport("wintun.dll", SetLastError = true)]
    public static extern IntPtr WintunAllocateSendPacket(IntPtr session, uint packetSize);

    [DllImport("wintun.dll", SetLastError = true)]
    public static extern void WintunSendPacket(IntPtr session, IntPtr packet);

    [DllImport("wintun.dll", SetLastError = true)]
    public static extern IntPtr WintunGetReadWaitEvent(IntPtr session);
}
'@
}

function Get-TunInfo {
    foreach ($cfg in (Get-NetIPConfiguration -ErrorAction SilentlyContinue)) {
        $a = Get-NetAdapter -IncludeHidden -InterfaceIndex $cfg.InterfaceIndex -ErrorAction SilentlyContinue
        if (-not $a) { continue }
        # The client renames the adapter description, so also accept its own name.
        if ($a.InterfaceDescription -notmatch 'Wintun|PPP PRIVATE NETWORK') { continue }
        if (-not ($cfg.IPv4Address -and $cfg.IPv4DefaultGateway)) { continue }
        if ($a.Status -ne 'Up') { continue }
        return [pscustomobject]@{
            Index   = $cfg.InterfaceIndex
            Name    = $cfg.InterfaceAlias
            Address = [string]$cfg.IPv4Address.IPAddress
            Gateway = [string]$cfg.IPv4DefaultGateway.NextHop
        }
    }
    return $null
}

$tun = Get-TunInfo
if (-not $tun) { Write-Output 'No active Wintun adapter found; start the TUN client first.'; return }
if (-not $AdapterAddress) { $AdapterAddress = $tun.Address }
if (-not $Gateway) { $Gateway = $tun.Gateway }

if (-not $AdapterName) {
    foreach ($candidate in @($tun.Name, 'PPP', 'PPP PRIVATE NETWORK 2', 'PPP PRIVATE NETWORK 2 Tunnel')) {
        $h = [WintunProbe]::WintunOpenAdapter($candidate)
        if ($h -ne [IntPtr]::Zero) {
            $AdapterName = $candidate
            [WintunProbe]::WintunCloseAdapter($h)
            break
        }
    }
}
if (-not $AdapterName) { Write-Output 'Could not open the Wintun adapter by name.'; return }

$quiet = [bool]$Quiet
if (-not $quiet) {
    Write-Output ("adapter        : {0} (ifIndex {1}) {2}  gw {3}" -f $AdapterName, $tun.Index, $AdapterAddress, $Gateway)
    Write-Output ("injected header: tos=0x{0:X2} id={1} DF={2} udp_csum={3}" -f $Tos, $Id, (-not $NoDf), $(if ($BadUdpChecksum) { 'broken' } else { 'valid' }))
}

$adapter = [WintunProbe]::WintunOpenAdapter($AdapterName)
if ($adapter -eq [IntPtr]::Zero) {
    Write-Output ("WintunOpenAdapter('{0}') failed, win32={1}" -f $AdapterName, [Runtime.InteropServices.Marshal]::GetLastWin32Error())
    return
}

# 1 MiB rings, the usual Wintun default.
$session = [WintunProbe]::WintunStartSession($adapter, 0x400000)
if ($session -eq [IntPtr]::Zero) {
    $err = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
    Write-Output ("WintunStartSession failed, win32={0}" -f $err)
    if ($err -eq 183) {
        Write-Output '  (ERROR_ALREADY_EXISTS: the client already holds the session; this probe needs the adapter to be idle)'
    }
    [WintunProbe]::WintunCloseAdapter($adapter)
    return
}

function Get-Discard {
    try { (Get-Counter -Counter '\WFP Classify\FWPM_LAYER_INBOUND_TRANSPORT_V4_DISCARD' -ErrorAction Stop).CounterSamples[0].CookedValue } catch { -1 }
}
function Add-Sum([byte[]]$d, [int]$off, [int]$len) {
    $s = 0
    for ($i = 0; $i -lt $len - 1; $i += 2) { $s += ([int]$d[$off + $i] -shl 8) -bor [int]$d[$off + $i + 1] }
    if ($len % 2) { $s += ([int]$d[$off + $len - 1] -shl 8) }
    while ($s -shr 16) { $s = ($s -band 0xFFFF) + ($s -shr 16) }
    return $s
}

$received = 0
$sent = 0
try {
    for ($i = 1; $i -le $Count; $i++) {
        $sink = New-Object System.Net.Sockets.UdpClient((New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Parse($AdapterAddress), 0)))
        $sink.Client.ReceiveTimeout = $TimeoutMs
        $dstPort = ([System.Net.IPEndPoint]$sink.Client.LocalEndPoint).Port
        $srcPort = 40000 + $i
        $payload = [System.Text.Encoding]::ASCII.GetBytes("$PayloadText-$i")

        $udpLen = 8 + $payload.Length
        $total = 20 + $udpLen
        $pkt = New-Object byte[] $total
        $pkt[0] = 0x45
        $pkt[1] = [byte]$Tos
        $pkt[2] = [byte](($total -shr 8) -band 0xFF); $pkt[3] = [byte]($total -band 0xFF)
        $pkt[4] = [byte](($Id -shr 8) -band 0xFF);   $pkt[5] = [byte]($Id -band 0xFF)
        if (-not $NoDf) { $pkt[6] = 0x40 }
        $pkt[8] = 64; $pkt[9] = 17
        $srcBytes = [System.Net.IPAddress]::Parse($Gateway).GetAddressBytes()
        $dstBytes = [System.Net.IPAddress]::Parse($AdapterAddress).GetAddressBytes()
        [Array]::Copy($srcBytes, 0, $pkt, 12, 4)
        [Array]::Copy($dstBytes, 0, $pkt, 16, 4)
        $ipSum = (-bnot (Add-Sum $pkt 0 20)) -band 0xFFFF
        $pkt[10] = [byte](($ipSum -shr 8) -band 0xFF); $pkt[11] = [byte]($ipSum -band 0xFF)

        $pkt[20] = [byte](($srcPort -shr 8) -band 0xFF); $pkt[21] = [byte]($srcPort -band 0xFF)
        $pkt[22] = [byte](($dstPort -shr 8) -band 0xFF); $pkt[23] = [byte]($dstPort -band 0xFF)
        $pkt[24] = [byte](($udpLen -shr 8) -band 0xFF);  $pkt[25] = [byte]($udpLen -band 0xFF)
        [Array]::Copy($payload, 0, $pkt, 28, $payload.Length)

        # UDP checksum over pseudo header + UDP header + payload.
        $pseudo = New-Object System.Collections.ArrayList
        foreach ($b in $srcBytes) { [void]$pseudo.Add($b) }
        foreach ($b in $dstBytes) { [void]$pseudo.Add($b) }
        [void]$pseudo.Add(0); [void]$pseudo.Add(17)
        [void]$pseudo.Add([byte](($udpLen -shr 8) -band 0xFF)); [void]$pseudo.Add([byte]($udpLen -band 0xFF))
        for ($k = 20; $k -lt $total; $k++) { [void]$pseudo.Add($pkt[$k]) }
        $pa = [byte[]]$pseudo.ToArray()
        $sum = Add-Sum $pa 0 $pa.Length
        $csum = (-bnot $sum) -band 0xFFFF
        if ($csum -eq 0) { $csum = 0xFFFF }
        if ($BadUdpChecksum) { $csum = ($csum -bxor 0x5555) -band 0xFFFF }
        $pkt[26] = [byte](($csum -shr 8) -band 0xFF); $pkt[27] = [byte]($csum -band 0xFF)

        $before = Get-Discard
        $buffer = [WintunProbe]::WintunAllocateSendPacket($session, [uint32]$total)
        if ($buffer -eq [IntPtr]::Zero) {
            Write-Output ("WintunAllocateSendPacket failed, win32={0}" -f [Runtime.InteropServices.Marshal]::GetLastWin32Error())
            break
        }
        [Runtime.InteropServices.Marshal]::Copy($pkt, 0, $buffer, $total)
        [WintunProbe]::WintunSendPacket($session, $buffer)
        $sent++

        $got = $false
        try {
            $ep = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0)
            $data = $sink.Receive([ref]$ep)
            if ($data -and $data.Length -gt 0) { $got = $true; $received++ }
        }
        catch { }
        Start-Sleep -Milliseconds 250
        $after = Get-Discard
        if (-not $quiet) {
            Write-Output ("  inject #{0}: dstPort={1} delivered={2} transport_discards=+{3}" -f $i, $dstPort, $got, ($after - $before))
        }
        $sink.Close()
    }
}
finally {
    [WintunProbe]::WintunEndSession($session)
    [WintunProbe]::WintunCloseAdapter($adapter)
}

Write-Output ("verdict: injected {0}, delivered {1}" -f $sent, $received)
if ($sent -gt 0 -and $received -eq $sent) {
    Write-Output '  INJECTION WORKS: the host stack receives packets written into the Wintun ring.'
}
elseif ($sent -gt 0) {
    Write-Output '  INJECTION BLOCKED: the host stack drops packets written into the Wintun ring.'
    Write-Output '  This is the root cause of missing DNS answers, ICMP replies and TCP handshakes'
    Write-Output '  on the tunnel path. Windows Firewall allow rules and third-party allow rules'
    Write-Output '  do not change it; compare field variants (-Tos, -Id, -NoDf, -BadUdpChecksum).'
}
