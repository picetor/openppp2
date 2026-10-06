<#
.SYNOPSIS
Captures Windows network state without changing adapters, routes, or DNS.

.EXAMPLE
.\tools\diagnostics\windows-network-snapshot.ps1 -Label before -OutputDirectory "$env:USERPROFILE\Desktop\TUI\netdiag" -VpnEndpoint 23.166.168.33

.EXAMPLE
.\tools\diagnostics\windows-network-snapshot.ps1 -Label failed -OutputDirectory "$env:USERPROFILE\Desktop\TUI\netdiag" -VpnEndpoint 23.166.168.33
#>
[CmdletBinding()]
param(
    [Parameter()]
    [ValidatePattern('^[A-Za-z0-9_.-]+$')]
    [string]$Label = 'snapshot',

    [Parameter()]
    [string]$OutputDirectory = (Join-Path $PSScriptRoot 'captures'),

    [Parameter()]
    [string]$VpnEndpoint,

    [Parameter()]
    [ValidateRange(1, 65535)]
    [int]$VpnPort = 20000,

    [Parameter()]
    [string]$DnsServer = '223.5.5.5',

    [Parameter()]
    [string]$TunGateway = '192.168.12.1',

    [Parameter()]
    [switch]$SkipProbes
)

$ErrorActionPreference = 'Stop'

function Get-PropertyValue {
    param(
        [Parameter(Mandatory = $true)] $Object,
        [Parameter(Mandatory = $true)] [string] $Name
    )

    $property = $Object.PSObject.Properties[$Name]
    if ($null -ne $property) { return $property.Value }
    return $null
}

function Convert-Route {
    param([Parameter(Mandatory = $true)] $Route)

    [pscustomobject]@{
        Family          = [string]$Route.AddressFamily
        Destination     = [string]$Route.DestinationPrefix
        InterfaceIndex  = [int]$Route.InterfaceIndex
        InterfaceAlias  = [string]$Route.InterfaceAlias
        NextHop         = [string]$Route.NextHop
        RouteMetric     = [int]$Route.RouteMetric
        State           = [string]$Route.State
        Protocol        = [string]$Route.Protocol
    }
}

function Test-TcpPort {
    param(
        [Parameter(Mandatory = $true)] [string] $ComputerName,
        [Parameter(Mandatory = $true)] [int] $Port,
        [int] $TimeoutMilliseconds = 1800
    )

    $client = New-Object System.Net.Sockets.TcpClient
    try {
        $pending = $client.BeginConnect($ComputerName, $Port, $null, $null)
        if (-not $pending.AsyncWaitHandle.WaitOne($TimeoutMilliseconds, $false)) {
            return [pscustomobject]@{
                ComputerName = $ComputerName
                Port         = $Port
                Connected    = $false
                Result       = 'timeout'
            }
        }

        try {
            $client.EndConnect($pending)
            return [pscustomobject]@{
                ComputerName = $ComputerName
                Port         = $Port
                Connected    = $true
                Result       = 'connected'
            }
        }
        catch {
            return [pscustomobject]@{
                ComputerName = $ComputerName
                Port         = $Port
                Connected    = $false
                Result       = $_.Exception.GetBaseException().Message
            }
        }
    }
    catch {
        return [pscustomobject]@{
            ComputerName = $ComputerName
            Port         = $Port
            Connected    = $false
            Result       = $_.Exception.GetBaseException().Message
        }
    }
    finally {
        $client.Close()
    }
}

function Get-LocalPortState {
    param([Parameter(Mandatory = $true)] [int] $Port)

    try {
        $client = New-Object System.Net.Sockets.TcpClient
        try {
            $pending = $client.BeginConnect('127.0.0.1', $Port, $null, $null)
            if (-not $pending.AsyncWaitHandle.WaitOne(700, $false)) {
                return [pscustomobject]@{ Port = $Port; Listening = $false }
            }
            try {
                $client.EndConnect($pending)
                return [pscustomobject]@{ Port = $Port; Listening = $true }
            }
            catch {
                return [pscustomobject]@{ Port = $Port; Listening = $false }
            }
        }
        finally {
            $client.Close()
        }
    }
    catch {
        return [pscustomobject]@{ Port = $Port; Listening = $false }
    }
}

$null = New-Item -ItemType Directory -Path $OutputDirectory -Force
$capturedAt = Get-Date
$capture = [ordered]@{
    Label          = $Label
    CapturedAt     = $capturedAt.ToString('o')
    PowerShell     = $PSVersionTable.PSVersion.ToString()
    VpnEndpoint    = if ([string]::IsNullOrWhiteSpace($VpnEndpoint)) { $null } else { $VpnEndpoint }
    VpnPort        = if ([string]::IsNullOrWhiteSpace($VpnEndpoint)) { $null } else { $VpnPort }
    Errors         = [System.Collections.Generic.List[string]]::new()
}

$adapters = @()
try {
    $adapters = @(Get-NetAdapter -IncludeHidden -ErrorAction Stop | ForEach-Object {
        [pscustomobject]@{
            InterfaceIndex      = [int]$_.ifIndex
            Name                = [string]$_.Name
            Status              = [string]$_.Status
            InterfaceDescription = [string]$_.InterfaceDescription
            LinkSpeed           = [string]$_.LinkSpeed
        }
    })
    $capture.Adapters = $adapters
}
catch {
    $capture.Errors.Add("Get-NetAdapter: $($_.Exception.GetBaseException().Message)")
    $capture.Adapters = @()
}

try {
    $capture.IpConfiguration = @(Get-NetIPConfiguration -All -ErrorAction Stop | ForEach-Object {
        [pscustomobject]@{
            InterfaceIndex       = [int]$_.InterfaceIndex
            InterfaceAlias       = [string]$_.InterfaceAlias
            IPv4Addresses        = @($_.IPv4Address | ForEach-Object { [string]$_.IPAddress })
            IPv6Addresses        = @($_.IPv6Address | ForEach-Object { [string]$_.IPAddress })
            IPv4DefaultGateways  = @($_.IPv4DefaultGateway | ForEach-Object { [string]$_.NextHop })
            IPv6DefaultGateways  = @($_.IPv6DefaultGateway | ForEach-Object { [string]$_.NextHop })
        }
    })
}
catch {
    $capture.Errors.Add("Get-NetIPConfiguration: $($_.Exception.GetBaseException().Message)")
    $capture.IpConfiguration = @()
}

$allRoutes = @()
try {
    $allRoutes = @(Get-NetRoute -ErrorAction Stop)
    $capture.DefaultRoutes = @($allRoutes | Where-Object {
        $_.DestinationPrefix -in @('0.0.0.0/0', '::/0')
    } | ForEach-Object { Convert-Route $_ })

    $capture.RouteCountsByInterface = @($allRoutes | Group-Object -Property InterfaceIndex | ForEach-Object {
        $first = $_.Group | Select-Object -First 1
        [pscustomobject]@{
            InterfaceIndex = [int]$_.Name
            InterfaceAlias = [string]$first.InterfaceAlias
            RouteCount     = [int]$_.Count
        }
    } | Sort-Object InterfaceIndex)

    $tunIndexes = @($adapters | Where-Object {
        ($_.Name + ' ' + $_.InterfaceDescription) -match '(?i)TAP|TUN|Wintun|OpenPPP|PPP'
    } | ForEach-Object { $_.InterfaceIndex })
    $tunRoutes = @($allRoutes | Where-Object {
        ($tunIndexes -contains [int]$_.InterfaceIndex) -or
        ($_.NextHop -eq $TunGateway)
    })
    $capture.TunRouteCount = $tunRoutes.Count
    $capture.TunRouteSamples = @($tunRoutes | Select-Object -First 60 | ForEach-Object { Convert-Route $_ })
}
catch {
    $capture.Errors.Add("Get-NetRoute: $($_.Exception.GetBaseException().Message)")
    $capture.DefaultRoutes = @()
    $capture.RouteCountsByInterface = @()
    $capture.TunRouteCount = 0
    $capture.TunRouteSamples = @()
}

try {
    $capture.DnsServers = @(Get-DnsClientServerAddress -ErrorAction Stop | ForEach-Object {
        [pscustomobject]@{
            InterfaceIndex = [int]$_.InterfaceIndex
            InterfaceAlias = [string]$_.InterfaceAlias
            AddressFamily  = [string]$_.AddressFamily
            Servers        = @($_.ServerAddresses | ForEach-Object { [string]$_ })
        }
    } | Where-Object { $_.Servers.Count -gt 0 })
}
catch {
    $capture.Errors.Add("Get-DnsClientServerAddress: $($_.Exception.GetBaseException().Message)")
    $capture.DnsServers = @()
}

try {
    $capture.CoreProcesses = @(Get-CimInstance Win32_Process -ErrorAction Stop | Where-Object {
        $_.Name -in @('ppp.exe', 'ppp-web.exe', 'ppp-tui.exe')
    } | ForEach-Object {
        $fileVersion = $null
        if (-not [string]::IsNullOrWhiteSpace($_.ExecutablePath) -and (Test-Path -LiteralPath $_.ExecutablePath)) {
            $fileVersion = (Get-Item -LiteralPath $_.ExecutablePath).VersionInfo.FileVersion
        }
        [pscustomobject]@{
            Name           = [string]$_.Name
            ProcessId      = [int]$_.ProcessId
            ParentProcessId = [int]$_.ParentProcessId
            ExecutablePath = [string]$_.ExecutablePath
            FileVersion    = [string]$fileVersion
        }
    })
}
catch {
    $capture.Errors.Add("Process query: $($_.Exception.GetBaseException().Message)")
    $capture.CoreProcesses = @()
}

$capture.LocalServices = @((Get-LocalPortState 19999), (Get-LocalPortState 39100))

if (-not $SkipProbes) {
    if (-not [string]::IsNullOrWhiteSpace($VpnEndpoint)) {
        try {
            $route = Find-NetRoute -RemoteIPAddress $VpnEndpoint -ErrorAction Stop | Select-Object -First 1
            if ($null -ne $route) {
                $capture.VpnEndpointRoute = [pscustomobject]@{
                    InterfaceIndex = [int](Get-PropertyValue $route 'InterfaceIndex')
                    InterfaceAlias = [string](Get-PropertyValue $route 'InterfaceAlias')
                    SourceAddress  = [string](Get-PropertyValue $route 'IPAddress')
                    NextHop        = [string](Get-PropertyValue $route 'NextHop')
                }
            }
        }
        catch {
            $capture.Errors.Add("Find-NetRoute endpoint: $($_.Exception.GetBaseException().Message)")
        }

        $capture.VpnEndpointProbe = Test-TcpPort -ComputerName $VpnEndpoint -Port $VpnPort
    }

    if (-not [string]::IsNullOrWhiteSpace($DnsServer)) {
        try {
            $answers = @(Resolve-DnsName -Name 'www.qq.com' -Type A -Server $DnsServer -DnsOnly -ErrorAction Stop |
                Where-Object { $_.IPAddress } | ForEach-Object { [string]$_.IPAddress })
            $capture.DomesticDnsProbe = [pscustomobject]@{
                Server  = $DnsServer
                Name    = 'www.qq.com'
                Success = ($answers.Count -gt 0)
                Answers = $answers
                Error   = $null
            }
        }
        catch {
            $capture.DomesticDnsProbe = [pscustomobject]@{
                Server  = $DnsServer
                Name    = 'www.qq.com'
                Success = $false
                Answers = @()
                Error   = $_.Exception.GetBaseException().Message
            }
        }
    }
}
else {
    $capture.ProbesSkipped = $true
}

$safeLabel = $Label
$stamp = $capturedAt.ToString('yyyyMMdd-HHmmss-fff')
$outputPath = Join-Path $OutputDirectory "$safeLabel-$stamp.json"
$capture | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $outputPath -Encoding UTF8

Write-Output "Network snapshot saved: $outputPath"
Write-Output "Label=$Label CapturedAt=$($capture.CapturedAt)"
Write-Output "DefaultRoutes=$(@($capture.DefaultRoutes).Count) TunRouteCount=$($capture.TunRouteCount)"
Write-Output "Local19999=$($capture.LocalServices[0].Listening) Local39100=$($capture.LocalServices[1].Listening)"
if ($capture.Contains('VpnEndpointProbe')) {
    Write-Output "VpnEndpoint=$VpnEndpoint`:$VpnPort TcpConnected=$($capture.VpnEndpointProbe.Connected) Result=$($capture.VpnEndpointProbe.Result)"
}
if ($capture.Contains('DomesticDnsProbe')) {
    Write-Output "DomesticDns=$($capture.DomesticDnsProbe.Success) Server=$DnsServer Answers=$(@($capture.DomesticDnsProbe.Answers) -join ',')"
}
if ($capture.Errors.Count -gt 0) {
    Write-Output "CaptureWarnings=$($capture.Errors.Count) (see Errors in JSON)"
}
