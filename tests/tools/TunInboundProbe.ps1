<#
.SYNOPSIS
    Checks whether the host really receives the DNS/ICMP replies that the
    openppp2 client injects into the TUN adapter.

.DESCRIPTION
    Run this while a TUN client is connected.

    The client answers tunnel DNS queries and ICMP echo requests by injecting
    packets into the Wintun/TAP adapter from userspace.  Those packets belong to
    no process, so a program-scoped Windows Firewall rule cannot match them.  If
    the active profile blocks inbound UDP/ICMP, Windows discards them at
    FWPM_LAYER_INBOUND_TRANSPORT_V4 and the host sees no DNS answer and no ping
    reply even though the tunnel itself carries traffic.

    This script sends UDP DNS queries to the tunnel gateway (which the client
    must answer through the adapter) and to the client's loopback DNS proxy
    (which bypasses the firewall), then reports both the reply counts and the
    WFP inbound-transport discard delta.  Expected healthy result:

        tunnel gateway : replies 6/6   discards +0
        loopback proxy : replies 6/6   discards +0

    A blocked tunnel shows 0 replies and a rising discard counter.

.PARAMETER TunGateway
    Tunnel gateway (the TUN adapter's IPv4 next hop).  Detected automatically
    when omitted.

.PARAMETER Count
    Datagrams sent to each DNS endpoint.

.EXAMPLE
    pwsh -File tests/tools/TunInboundProbe.ps1
#>
[CmdletBinding()]
param(
    [string] $TunGateway,
    [string] $LoopbackDns = '127.0.0.1',
    [int]    $Count = 6
)

$ErrorActionPreference = 'Stop'
$discardCounter = '\WFP Classify\FWPM_LAYER_INBOUND_TRANSPORT_V4_DISCARD'

# Windows Firewall enforces its inbound rules at the ALE layer, so a discard
# counted at the inbound transport layer means some other WFP provider filtered
# the packet.  These drivers are known to register transport-layer filters and
# cannot be overridden by an INetFwRule allow rule.
$knownWfpDrivers = @{
    'hrwfpdrv'  = 'Huorong network firewall (WFP)'
    'hrndis6'   = 'Huorong NDIS filter'
    'sysdiag'   = 'Huorong core'
    '360Box'    = '360 Total Security'
    '360netmon' = '360 network monitor'
    'klif'      = 'Kaspersky'
    'klflt'     = 'Kaspersky'
    'ehdrv'     = 'ESET'
    'aswNdis'   = 'Avast'
    'bdfndisf'  = 'Bitdefender'
    'gwdrv'     = 'GlassWire'
    'nllf'      = 'NetLimiter'
    'npcap'     = 'Npcap'
    'npf'       = 'WinPcap/Npcap'
    'WinDivert' = 'WinDivert'
}

function Get-WfpFilterSuspects {
    $found = New-Object System.Collections.ArrayList
    $drivers = Get-CimInstance Win32_SystemDriver -ErrorAction SilentlyContinue |
        Where-Object { $_.State -eq 'Running' }

    foreach ($d in $drivers) {
        if ($knownWfpDrivers.ContainsKey($d.Name)) {
            [void]$found.Add(('{0} - {1}' -f $d.Name, $knownWfpDrivers[$d.Name]))
        }
        elseif ($d.DisplayName -match '(?i)\bWFP\b|firewall|网络防火墙|NDIS filter') {
            [void]$found.Add(('{0} - {1}' -f $d.Name, $d.DisplayName))
        }
    }

    $services = Get-Service -ErrorAction SilentlyContinue |
        Where-Object { $_.Status -eq 'Running' -and $_.DisplayName -match '(?i)火绒|huorong|360|kaspersky|eset|avast|bitdefender|glasswire|netlimiter' }
    foreach ($s in $services) {
        [void]$found.Add(('service {0} - {1}' -f $s.Name, $s.DisplayName))
    }

    return $found
}

function Get-DiscardCount {
    try { return [int64](Get-Counter -Counter $discardCounter).CounterSamples[0].CookedValue }
    catch { return -1 }
}

function Find-TunGateway {
    # The tunnel adapter is the one carrying the VPN; identify it by the driver
    # description first and by a PPP-like alias as a fallback.
    $candidates = @()
    foreach ($cfg in (Get-NetIPConfiguration -ErrorAction SilentlyContinue)) {
        $desc = ''
        $adapter = Get-NetAdapter -InterfaceIndex $cfg.InterfaceIndex -ErrorAction SilentlyContinue
        if ($adapter) { $desc = [string]$adapter.InterfaceDescription }
        if ($desc -match 'PPP PRIVATE NETWORK|TAP-Windows|Wintun' -or
            $cfg.InterfaceAlias -match '^(PPP|ppp)') {
            $gw = $cfg.IPv4DefaultGateway.NextHop
            if ($gw) { $candidates += [string]$gw }
        }
    }
    return ($candidates | Select-Object -First 1)
}

function New-DnsQuery([string] $Name, [int] $Id) {
    $ms = New-Object System.IO.MemoryStream
    $bw = New-Object System.IO.BinaryWriter($ms)
    $bw.Write([byte]($Id -shr 8))
    $bw.Write([byte]($Id -band 0xFF))
    $bw.Write([byte]0x01)   # RD = 1
    $bw.Write([byte]0x00)
    $bw.Write([byte]0x00); $bw.Write([byte]0x01)   # QDCOUNT = 1
    $bw.Write([byte]0x00); $bw.Write([byte]0x00)   # ANCOUNT = 0
    $bw.Write([byte]0x00); $bw.Write([byte]0x00)   # NSCOUNT = 0
    $bw.Write([byte]0x00); $bw.Write([byte]0x00)   # ARCOUNT = 0
    foreach ($label in $Name.Split('.')) {
        $bw.Write([byte]$label.Length)
        $bw.Write([System.Text.Encoding]::ASCII.GetBytes($label))
    }
    $bw.Write([byte]0)                             # root label
    $bw.Write([byte]0); $bw.Write([byte]1)         # QTYPE = A
    $bw.Write([byte]0); $bw.Write([byte]1)         # QCLASS = IN
    $bw.Flush()
    return $ms.ToArray()
}

function Test-DnsPath([string] $Server, [int] $Rounds) {
    $replies = 0
    for ($i = 0; $i -lt $Rounds; $i++) {
        $name = 'tun-probe-{0}-{1}.example.com' -f $i, ([guid]::NewGuid().ToString('N').Substring(0, 6))
        $query = New-DnsQuery $name (6000 + $i)
        $sock = New-Object System.Net.Sockets.UdpClient(
            (New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0)))
        $sock.Client.ReceiveTimeout = 3000
        try {
            [void]$sock.Send($query, $query.Length,
                (New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Parse($Server), 53)))
            [void]$sock.Receive([ref](New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0)))
            $replies++
        }
        catch { }
        finally { $sock.Close() }
    }
    return $replies
}

if (-not $TunGateway) {
    $TunGateway = Find-TunGateway
}
if (-not $TunGateway) {
    Write-Error 'Cannot detect the tunnel gateway; pass -TunGateway explicitly.'
}

Write-Output ("tunnel gateway : {0}" -f $TunGateway)
Write-Output ("loopback DNS   : {0}" -f $LoopbackDns)
Write-Output ''

$tunnelReplies = -1
$tunnelDiscards = -1

foreach ($path in @(
        @{ Label = 'tunnel gateway (injected into the TUN)'; Server = $TunGateway; Tunnel = $true },
        @{ Label = 'loopback proxy (bypasses the firewall)'; Server = $LoopbackDns; Tunnel = $false })) {

    $before = Get-DiscardCount
    $replies = Test-DnsPath $path.Server $Count
    Start-Sleep -Milliseconds 800
    $after = Get-DiscardCount

    $delta = if ($before -ge 0 -and $after -ge 0) { $after - $before } else { 'n/a' }
    Write-Output ("{0,-42} replies {1}/{2}   WFP inbound-transport discards +{3}" -f
        $path.Label, $replies, $Count, $delta)

    if ($path.Tunnel) {
        $tunnelReplies = $replies
        if ($delta -is [int64] -or $delta -is [int]) { $tunnelDiscards = $delta }
    }
}

Write-Output ''
# Windows Firewall enforces its inbound rules at the ALE layer, so a discard that
# appears on the inbound transport layer while the ALE layer stays flat is not
# Windows Firewall rule processing - and no INetFwRule allow rule will change it.
$layerCounters = [ordered]@{
    IPPACKET  = '\WFP Classify\FWPM_LAYER_INBOUND_IPPACKET_V4_DISCARD'
    TRANSPORT = '\WFP Classify\FWPM_LAYER_INBOUND_TRANSPORT_V4_DISCARD'
    ALE       = '\WFP Classify\FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V4_DISCARD'
}
function Get-LayerDiscards {
    $h = @{}
    foreach ($key in $layerCounters.Keys) {
        try { $h[$key] = (Get-Counter -Counter $layerCounters[$key] -ErrorAction Stop).CounterSamples[0].CookedValue }
        catch { $h[$key] = -1 }
    }
    return $h
}
$beforeLayers = Get-LayerDiscards
$null = Test-DnsPath $TunGateway $Count
Start-Sleep -Milliseconds 800
$afterLayers = Get-LayerDiscards
Write-Output ("WFP discard growth for {0} queries to the tunnel gateway, by layer:" -f $Count)
foreach ($key in $layerCounters.Keys) {
    $growth = if ($beforeLayers[$key] -ge 0 -and $afterLayers[$key] -ge 0) { $afterLayers[$key] - $beforeLayers[$key] } else { 'n/a' }
    Write-Output ("  {0,-10} +{1}" -f $key, $growth)
}
Write-Output '  transport-only growth with a flat ALE layer = not Windows Firewall rule processing'

Write-Output ''
$suspects = @(Get-WfpFilterSuspects)
if ($suspects.Count -gt 0) {
    Write-Output 'Third-party WFP / NDIS filter drivers that can block injected packets:'
    foreach ($s in $suspects) { Write-Output ("  - {0}" -f $s) }
    Write-Output ''
}

if ($tunnelReplies -eq $Count -and $tunnelDiscards -le 2) {
    Write-Output 'Result: healthy - the host receives the packets the client injects into the TUN.'
}
elseif ($suspects.Count -gt 0) {
    Write-Output 'Result: the host stack discards the packets the client injects into the TUN.'
    Write-Output '        A third-party WFP/NDIS filter is only one possibility - check the per-layer'
    Write-Output '        growth above first. Transport-only growth with a flat ALE layer means'
    Write-Output '        Windows Firewall rule processing is not what dropped them, so an inbound'
    Write-Output '        allow rule will not help. On the machine this probe was written for the'
    Write-Output '        injected packets had valid IP/UDP checksums, no WFP filter was registered'
    Write-Output '        on that layer, and the product listed above was not even bound to the'
    Write-Output '        tunnel adapter.'
    Write-Output '        Next steps: compare --tun-driver=wintun with --tun-driver=tap, and read the'
    Write-Output '        client log line "TUN injection probe" (the client now self-tests this path).'
    Write-Output '        Note: DNS and web browsing need not depend on it - pointing the TUN resolver'
    Write-Output '        at the loopback proxy (127.0.0.1:53) keeps name resolution working.'
}
else {
    Write-Output 'Result: the host stack discards the packets the client injects into the TUN,'
    Write-Output '        and no third-party WFP/NDIS filter is present.'
    Write-Output '        An inbound allow rule is harmless and worth trying when the drop really is'
    Write-Output '        Windows Firewall policy:'
    Write-Output '        New-NetFirewallRule -Direction Inbound -Action Allow -Protocol UDP -InterfaceAlias PPP -LocalAddress <tunnel subnet>'
    Write-Output '        Then compare --tun-driver=wintun with --tun-driver=tap.'
}
