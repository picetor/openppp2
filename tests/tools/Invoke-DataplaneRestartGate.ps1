[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string]$StartScript,

    [Parameter(Mandatory = $true)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string]$StopScript,

    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$TunAddress,

    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$DnsServer,

    [string]$PingTarget = '1.1.1.1',
    [string]$DnsName = 'example.com',
    [ValidateRange(1280, 1500)]
    [int]$ExpectedMtu = 1400,
    [ValidateRange(1, 100)]
    [int]$Cycles = 20,
    [ValidateRange(1, 300)]
    [int]$StartTimeoutSeconds = 30,
    [ValidateRange(1, 300)]
    [int]$StopTimeoutSeconds = 30,
    [string]$OutputDirectory = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Test-IsAdministrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Wait-TunAddress {
    param([bool]$Present, [int]$TimeoutSeconds)
    $timer = [Diagnostics.Stopwatch]::StartNew()
    do {
        $found = $null -ne (Get-NetIPAddress -IPAddress $TunAddress -ErrorAction SilentlyContinue |
            Select-Object -First 1)
        if ($found -eq $Present) { return $true }
        Start-Sleep -Milliseconds 250
    } while ($timer.Elapsed.TotalSeconds -lt $TimeoutSeconds)
    return $false
}

function Get-TunState {
    $address = Get-NetIPAddress -IPAddress $TunAddress -ErrorAction Stop | Select-Object -First 1
    $index = [int]$address.InterfaceIndex
    $ipv4 = Get-NetIPInterface -InterfaceIndex $index -AddressFamily IPv4 -ErrorAction Stop |
        Select-Object -First 1
    $ipv6 = Get-NetIPInterface -InterfaceIndex $index -AddressFamily IPv6 -ErrorAction SilentlyContinue |
        Select-Object -First 1
    $dns = @(Get-DnsClientServerAddress -InterfaceIndex $index -ErrorAction Stop |
        ForEach-Object { $_.ServerAddresses } | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
        Sort-Object -Unique)
    $routes = @(Get-NetRoute -InterfaceIndex $index -ErrorAction Stop |
        ForEach-Object {
            '{0}|{1}|{2}|{3}' -f $_.AddressFamily, $_.DestinationPrefix, $_.NextHop, $_.RouteMetric
        } | Sort-Object -Unique)
    return [ordered]@{
        interface_index = $index
        interface_alias = $address.InterfaceAlias
        ipv4_mtu = [int]$ipv4.NlMtu
        ipv6_mtu = if ($null -eq $ipv6) { -1 } else { [int]$ipv6.NlMtu }
        dns_servers = $dns
        route_signature = $routes
    }
}

function Invoke-Hook {
    param([string]$Path, [string]$Name)
    try {
        & $Path
        if (-not $?) { throw "$Name hook returned failure." }
    }
    catch {
        throw "$Name hook failed: $($_.Exception.Message)"
    }
}

if (-not (Test-IsAdministrator)) {
    throw 'Run this restart gate from an elevated PowerShell session.'
}

$StartScript = [IO.Path]::GetFullPath($StartScript)
$StopScript = [IO.Path]::GetFullPath($StopScript)
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $OutputDirectory = Join-Path (Get-Location) "artifacts/dataplane-restart-$stamp"
}
$outputPath = [IO.Path]::GetFullPath($OutputDirectory)
if ((Test-Path -LiteralPath $outputPath) -and
    @(Get-ChildItem -LiteralPath $outputPath -Force -ErrorAction Stop).Count -gt 0) {
    throw "Output directory must be empty: $outputPath"
}
New-Item -ItemType Directory -Path $outputPath -Force | Out-Null
$cyclesPath = Join-Path $outputPath 'cycles.jsonl'
$summaryPath = Join-Path $outputPath 'summary.json'
$baselineRoutes = $null
$failed = 0

for ($cycle = 1; $cycle -le $Cycles; $cycle++) {
    $record = [ordered]@{
        cycle = $cycle
        started_utc = [DateTime]::UtcNow.ToString('o')
        start_ok = $false
        state_ok = $false
        ping_ok = $false
        dns_ok = $false
        stop_ok = $false
        cleanup_ok = $false
        error = $null
    }
    $interfaceIndex = -1
    try {
        Invoke-Hook -Path $StartScript -Name 'start'
        $record.start_ok = Wait-TunAddress -Present $true -TimeoutSeconds $StartTimeoutSeconds
        if (-not $record.start_ok) { throw 'TUN address did not appear before the startup timeout.' }
        $state = Get-TunState
        $interfaceIndex = [int]$state.interface_index
        if ($null -eq $baselineRoutes) { $baselineRoutes = @($state.route_signature) }
        $routesMatch = @(Compare-Object -ReferenceObject $baselineRoutes -DifferenceObject @($state.route_signature)).Count -eq 0
        $record.state = $state
        $record.state_ok = $state.ipv4_mtu -eq $ExpectedMtu -and
            $state.ipv6_mtu -eq $ExpectedMtu -and
            @($state.dns_servers | Where-Object { $_ -eq $DnsServer }).Count -gt 0 -and $routesMatch
        $record.state_ok = $record.state_ok -and @($state.route_signature).Count -gt 0
        $pingOutput = & ping.exe -n 1 -w 3000 -S $TunAddress $PingTarget 2>&1
        $record.ping_ok = $LASTEXITCODE -eq 0 -and
            @($pingOutput | Where-Object { "$_" -match '(?i)TTL[=<]\d+' }).Count -eq 1
        try {
            Resolve-DnsName -Name $DnsName -Server $DnsServer -DnsOnly -QuickTimeout -ErrorAction Stop | Out-Null
            $record.dns_ok = $true
        }
        catch { $record.dns_ok = $false }
    }
    catch {
        $record.error = $_.Exception.Message
    }
    finally {
        try {
            Invoke-Hook -Path $StopScript -Name 'stop'
            $record.stop_ok = Wait-TunAddress -Present $false -TimeoutSeconds $StopTimeoutSeconds
            $routesRemain = $false
            $dnsRemains = $false
            if ($interfaceIndex -ge 0) {
                $routesRemain = @(Get-NetRoute -InterfaceIndex $interfaceIndex -ErrorAction SilentlyContinue).Count -gt 0
                $dnsRemains = @(Get-DnsClientServerAddress -InterfaceIndex $interfaceIndex -ErrorAction SilentlyContinue |
                    ForEach-Object { $_.ServerAddresses } | Where-Object { $_ -eq $DnsServer }).Count -gt 0
            }
            $record.cleanup_ok = $record.stop_ok -and -not $routesRemain -and -not $dnsRemains
        }
        catch {
            $record.stop_ok = $false
            $record.cleanup_ok = $false
            if ($null -eq $record.error) { $record.error = $_.Exception.Message }
        }
    }
    $record.passed = $record.start_ok -and $record.state_ok -and $record.ping_ok -and
        $record.dns_ok -and $record.stop_ok -and $record.cleanup_ok
    if (-not $record.passed) { $failed++ }
    $record.finished_utc = [DateTime]::UtcNow.ToString('o')
    $record | ConvertTo-Json -Depth 5 -Compress | Add-Content -LiteralPath $cyclesPath -Encoding utf8
}

$summary = [ordered]@{
    gate_scope = 'restart_20_cycle'
    requested_cycles = $Cycles
    passed_cycles = $Cycles - $failed
    failed_cycles = $failed
    expected_mtu = $ExpectedMtu
    tun_address = $TunAddress
    dns_server = $DnsServer
    passed = $failed -eq 0 -and $Cycles -eq 20
    note = 'This gate covers restart consistency and cleanup only; it does not replace WFP, fault-injection, physical-switch, or 24-hour gates.'
}
$summary | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $summaryPath -Encoding utf8
Write-Host "Restart-gate evidence: $outputPath"
if (-not $summary.passed) { exit 1 }
