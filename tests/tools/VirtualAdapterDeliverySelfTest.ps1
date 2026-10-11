<#
.SYNOPSIS
Answers one question: can this machine deliver a packet from a virtual tunnel
adapter into the host IP stack?

.DESCRIPTION
Every packet the TUN client receives from the server reaches the host only
because the client writes it into a Wintun/TAP adapter from user space.  If the
host stack refuses those packets, DNS answers, ICMP replies and TCP handshakes
never arrive while the tunnel itself stays healthy, and no client change can
compensate: the last hop is inside Windows.

This script reproduces that hop without the client running.  It creates a fresh
Wintun adapter (so no client, route table or stale adapter state is involved),
assigns it an address, verifies the address is a local destination (self-ping),
then injects a UDP/IP packet into the adapter and checks whether a socket bound
to that address receives it.

Run it from an elevated PowerShell.  The test adapter is always removed again.

.PARAMETER AdapterName
Name of the temporary adapter.  Default DSH-TAPTEST.

.PARAMETER Address
IPv4 address and prefix length for the temporary adapter.  Default 10.194.0.1/24.

.PARAMETER Port
UDP port of the probe.  Default 45678.

.PARAMETER WintunDirectory
Directory containing wintun.dll.  Default: Driver\x64 next to this repository.

.EXAMPLE
powershell -ExecutionPolicy Bypass -File tests\tools\VirtualAdapterDeliverySelfTest.ps1

.OUTPUTS
Prints driver versions, the Huorong driver state, the self-ping result and
the injection verdict (RECEIVED / NOT RECEIVED), then removes the adapter.
#>
[CmdletBinding()]
param(
    [string] $AdapterName = 'DSH-TAPTEST',
    [string] $Address = '10.194.0.1',
    [int]    $PrefixLength = 24,
    [int]    $Port = 45678,
    [string] $WintunDirectory = ''
)

$ErrorActionPreference = 'Stop'
$script:adapterIndex = -1
$script:exitCode = 1

function Write-Section([string] $text) { Write-Host ''; Write-Host "=== $text ===" }

function Show-Environment {
    Write-Section 'environment'
    $sys = Join-Path $env:SystemRoot 'System32\drivers\wintun.sys'
    if (Test-Path $sys) {
        $v = (Get-Item $sys).VersionInfo
        Write-Host ("wintun.sys  : {0} ({1})" -f $v.FileVersion, (Get-Item $sys).LastWriteTime)
    } else { Write-Host 'wintun.sys  : not installed' }

    $dll = Join-Path $PSScriptRoot $WintunDirectory
    $candidates = @()
    if ($WintunDirectory) { $candidates += $WintunDirectory }
    $repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
    $candidates += (Join-Path $repoRoot 'Driver\x64\wintun.dll')
    foreach ($c in $candidates) {
        if ($c -and (Test-Path $c)) {
            Write-Host ("wintun.dll  : {0} ({1})" -f (Get-Item $c).VersionInfo.FileVersion, $c)
            $script:wintunDirectory = Split-Path $c -Parent
            break
        }
    }
    if (-not $script:wintunDirectory) { throw 'wintun.dll not found; pass -WintunDirectory' }

    $hips = @(Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -match 'Hips|Huorong|wsctrl|Sysdiag' })
    if ($hips.Count -gt 0) {
        Write-Host ("security    : {0} (a network filter/callout product is running)" -f (($hips | Select-Object -ExpandProperty ProcessName) -join ', '))
    } else { Write-Host 'security    : no Huorong-style process detected' }

    $bindings = @(Get-NetAdapterBinding -Name '*' -ErrorAction SilentlyContinue |
        Where-Object { $_.ComponentID -notlike 'ms_*' -and $_.Enabled } |
        Select-Object -ExpandProperty DisplayName -Unique)
    if ($bindings.Count -gt 0) { Write-Host ("filters     : {0}" -f ($bindings -join ', ')) }
}

$interop = @'
using System;
using System.Net;
using System.Runtime.InteropServices;
using System.Text;

public static class VirtualAdapterSelfTest
{
    [StructLayout(LayoutKind.Sequential)] public struct NET_LUID { public ulong Value; }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool SetDllDirectory(string path);
    [DllImport("wintun.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern IntPtr WintunCreateAdapter(string name, string tunnelType, IntPtr requestedGuid);
    [DllImport("wintun.dll", SetLastError = true)]
    private static extern void WintunGetAdapterLUID(IntPtr adapter, ref NET_LUID luid);
    [DllImport("wintun.dll", SetLastError = true)]
    private static extern IntPtr WintunStartSession(IntPtr adapter, uint capacity);
    [DllImport("wintun.dll", SetLastError = true)]
    private static extern void WintunEndSession(IntPtr session);
    [DllImport("wintun.dll", SetLastError = true)]
    private static extern void WintunCloseAdapter(IntPtr adapter);
    [DllImport("wintun.dll", SetLastError = true)]
    private static extern IntPtr WintunAllocateSendPacket(IntPtr session, uint packetSize);
    [DllImport("wintun.dll", SetLastError = true)]
    private static extern void WintunSendPacket(IntPtr session, IntPtr packet);
    [DllImport("iphlpapi.dll")]
    private static extern int ConvertInterfaceLuidToIndex(ref NET_LUID luid, out uint index);

    private static IntPtr adapter = IntPtr.Zero;
    private static IntPtr session = IntPtr.Zero;

    public static string Create(string directory, string name)
    {
        SetDllDirectory(directory);
        adapter = WintunCreateAdapter(name, name, IntPtr.Zero);
        if (adapter == IntPtr.Zero) { return "create-adapter-failed:" + Marshal.GetLastWin32Error(); }
        NET_LUID luid = new NET_LUID();
        WintunGetAdapterLUID(adapter, ref luid);
        uint index = 0;
        ConvertInterfaceLuidToIndex(ref luid, out index);
        session = WintunStartSession(adapter, 0x400000);
        if (session == IntPtr.Zero) { return "start-session-failed:" + Marshal.GetLastWin32Error(); }
        return "ok:" + index;
    }

    private static ushort Checksum(byte[] data, int offset, int length)
    {
        long sum = 0; int i = offset;
        while (i < offset + length - 1) { sum += (long)((data[i] << 8) | data[i + 1]); i += 2; }
        if (i < offset + length) { sum += (long)(data[i] << 8); }
        while ((sum >> 16) != 0) { sum = (sum & 0xFFFF) + (sum >> 16); }
        return (ushort)(~sum & 0xFFFF);
    }

    public static byte[] BuildUdp(string source, int sourcePort, string destination, int destinationPort, string text)
    {
        byte[] payload = Encoding.ASCII.GetBytes(text);
        int total = 20 + 8 + payload.Length;
        byte[] p = new byte[total];
        p[0] = 0x45; p[2] = (byte)(total >> 8); p[3] = (byte)(total & 0xFF);
        p[8] = 64; p[9] = 17;
        byte[] sourceBytes = IPAddress.Parse(source).GetAddressBytes();
        byte[] destinationBytes = IPAddress.Parse(destination).GetAddressBytes();
        Array.Copy(sourceBytes, 0, p, 12, 4);
        Array.Copy(destinationBytes, 0, p, 16, 4);
        ushort ip = Checksum(p, 0, 20);
        p[10] = (byte)(ip >> 8); p[11] = (byte)(ip & 0xFF);

        int u = 20;
        int udpLength = 8 + payload.Length;
        p[u] = (byte)(sourcePort >> 8); p[u + 1] = (byte)(sourcePort & 0xFF);
        p[u + 2] = (byte)(destinationPort >> 8); p[u + 3] = (byte)(destinationPort & 0xFF);
        p[u + 4] = (byte)(udpLength >> 8); p[u + 5] = (byte)(udpLength & 0xFF);
        Array.Copy(payload, 0, p, u + 8, payload.Length);

        // The UDP checksum covers the pseudo header as well (source, destination,
        // zero, protocol, UDP length).  An earlier version omitted it, so the host
        // dropped the probe with "INET: checksum is invalid" (seen with pktmon) and
        // the tool reported a false NOT RECEIVED.
        byte[] pseudo = new byte[12 + udpLength];
        Array.Copy(sourceBytes, 0, pseudo, 0, 4);
        Array.Copy(destinationBytes, 0, pseudo, 4, 4);
        pseudo[8] = 0; pseudo[9] = 17;
        pseudo[10] = (byte)(udpLength >> 8); pseudo[11] = (byte)(udpLength & 0xFF);
        Array.Copy(p, u, pseudo, 12, udpLength);
        ushort udp = Checksum(pseudo, 0, pseudo.Length);
        if (udp == 0) { udp = 0xFFFF; }
        p[u + 6] = (byte)(udp >> 8); p[u + 7] = (byte)(udp & 0xFF);
        return p;
    }

    public static string Inject(byte[] packet, int copies)
    {
        if (session == IntPtr.Zero) { return "no-session"; }
        for (int n = 0; n < copies; n++)
        {
            IntPtr buffer = WintunAllocateSendPacket(session, (uint)packet.Length);
            if (buffer == IntPtr.Zero) { return "allocate-failed:" + Marshal.GetLastWin32Error(); }
            Marshal.Copy(packet, 0, buffer, packet.Length);
            WintunSendPacket(session, buffer);
        }
        return "sent:" + copies;
    }

    public static void Close()
    {
        if (session != IntPtr.Zero) { WintunEndSession(session); session = IntPtr.Zero; }
        if (adapter != IntPtr.Zero) { WintunCloseAdapter(adapter); adapter = IntPtr.Zero; }
    }
}
'@

try {
    Show-Environment

    Write-Section 'create temporary adapter'
    Add-Type -TypeDefinition $interop -Language CSharp
    $created = [VirtualAdapterSelfTest]::Create($script:wintunDirectory, $AdapterName)
    Write-Host "result      : $created"
    if (-not $created.StartsWith('ok:')) { throw "adapter creation failed ($created)" }
    $script:adapterIndex = [int]$created.Substring(3)

    $adapter = Get-NetAdapter -InterfaceIndex $script:adapterIndex
    $mac = $adapter.MacAddress
    Write-Host ("adapter     : {0} ifIndex={1} mac={2} status={3}" -f $adapter.Name, $script:adapterIndex, $mac, $adapter.Status)

    Write-Section 'address assignment'
    New-NetIPAddress -InterfaceIndex $script:adapterIndex -IPAddress $Address -PrefixLength $PrefixLength -Confirm:$false | Out-Null
    Start-Sleep -Seconds 2
    $assigned = @(Get-NetIPAddress -InterfaceIndex $script:adapterIndex -AddressFamily IPv4 -ErrorAction SilentlyContinue |
        Where-Object { $_.IPAddress -eq $Address })
    if ($assigned.Count -eq 0) { throw "address $Address was not assigned to ifIndex $script:adapterIndex" }
    Write-Host "assigned    : $Address/$PrefixLength"

    Write-Section 'local delivery of the adapter address (self-ping)'
    $ping = New-Object System.Net.NetworkInformation.Ping
    $task = $ping.SendPingAsync($Address, 1500)
    if ($task.Wait(3000)) {
        Write-Host ("self-ping   : {0} {1} ms" -f $task.Result.Status, $task.Result.RoundtripTime)
        $selfPingOk = ($task.Result.Status -eq 'Success')
    } else { Write-Host 'self-ping   : no result'; $selfPingOk = $false }

    Write-Section 'injection: adapter -> host socket'
    $listener = New-Object System.Net.Sockets.UdpClient
    $listener.Client.Bind((New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, $Port)))
    Start-Sleep -Milliseconds 200

    $packet = [VirtualAdapterSelfTest]::BuildUdp('10.194.0.99', 53, $Address, $Port, 'virtual-adapter-delivery-test')
    $receive = $listener.ReceiveAsync()
    Write-Host ("inject      : {0}" -f [VirtualAdapterSelfTest]::Inject($packet, 1))
    $received = ([System.Threading.Tasks.Task]::WaitAny(@($receive), 2500) -ge 0)
    Write-Host ("verdict     : {0}" -f ($(if ($received) { 'RECEIVED (the host stack accepts packets from this adapter)' } else { 'NOT RECEIVED (the host stack discards them)' })))
    if ($received) { Write-Host ("payload     : {0}" -f [System.Text.Encoding]::ASCII.GetString($receive.Result.Buffer)) }
    $listener.Close()

    Write-Section 'summary'
    Write-Host ("injection   : {0}" -f $(if ($received) { 'ok (the host stack accepts packets from this adapter)' } else { 'broken (the host stack did not deliver them)' }))
    Write-Host ("self-ping   : {0}" -f $(if ($selfPingOk) { 'the adapter address is local' } else { 'the host routes packets for its own adapter address out of the interface (informational)' }))
    if (-not $received) {
        Write-Host ''
        Write-Host 'A bare adapter cannot deliver either, so the send ring, the driver or the host stack is at fault.'
        Write-Host 'Next steps:'
        Write-Host '  1. Run it again: a single run is not a verdict, delivery on the affected machines is intermittent.'
        Write-Host '  2. Compare drivers with --tun-driver=tap (TAP needs dadtransmits=0: without it the address stays'
        Write-Host '     Tentative on a tunnel adapter and the host drops every frame).'
        Write-Host '  3. If both drivers fail on a clean boot with no third-party network filters, replace wintun.sys/'
        Write-Host '     wintun.dll with a build carrying the upstream NBL/ring fixes; see'
        Write-Host '     docs/TUN_MODE_REMAINING_ISSUES_CN.md section 11.'
    }
    $script:exitCode = if ($received) { 0 } else { 2 }
}
finally {
    if ($script:adapterIndex -ge 0) {
        Remove-NetIPAddress -InterfaceIndex $script:adapterIndex -IPAddress $Address -Confirm:$false -ErrorAction SilentlyContinue | Out-Null
        [VirtualAdapterSelfTest]::Close()
        Start-Sleep -Seconds 2
        if (Get-NetAdapter -InterfaceIndex $script:adapterIndex -ErrorAction SilentlyContinue) {
            Write-Warning "temporary adapter $AdapterName (ifIndex $script:adapterIndex) is still present; remove it manually"
        } else { Write-Host "cleanup     : temporary adapter removed" }
    }
}

exit $script:exitCode
