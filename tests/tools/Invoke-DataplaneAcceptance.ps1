[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$TunAddress,

    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$DnsServer,

    [string]$PingTarget = '1.1.1.1',
    [string]$DnsName = 'example.com',
    [ValidateRange(1, 1000)]
    [int]$PingCount = 100,
    [ValidateRange(1, 1000)]
    [int]$DnsCount = 100,
    [ValidateRange(1280, 1500)]
    [int]$ExpectedMtu = 1400,
    [string]$SocksProxy = '',
    [ValidateRange(0, 1000)]
    [int]$SocksCount = 50,
    [string]$HttpUrl = 'https://example.com/',
    [switch]$FullGate,
    [string]$SocksIpv4Url = '',
    [string]$SocksDomainUrl = '',
    [string]$HttpProxy = '',
    [string]$HttpConnectUrl = '',
    [ValidateRange(1, 1000)]
    [int]$HttpsCount = 100,
    [string]$Download10MbUrl = '',
    [string]$Download10MbSha256 = '',
    [string]$Download100MbUrl = '',
    [string]$Download100MbSha256 = '',
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$CoreLog,
    [string]$PythonExecutable = '',
    [string]$OutputDirectory = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Test-IsAdministrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Get-CoreLogCounters {
    param([string]$Path)
    $counters = [ordered]@{
        wintun_validate_fail = 0
        tun_rx = 0
        policy_selected = 0
        remote_tx = 0
        remote_rx = 0
        local_rx = 0
        wintun_allocated = 0
        wintun_submit_ok = 0
        wintun_submit_fail = 0
        duplicate_syn = 0
        static_echo_fallback = 0
    }
    if ([string]::IsNullOrWhiteSpace($Path) -or -not (Test-Path -LiteralPath $Path)) {
        return $counters
    }
    $counters.wintun_validate_fail = @(Select-String -LiteralPath $Path -SimpleMatch 'WINTUN_VALIDATE_FAIL').Count
    $counters.tun_rx = @(Select-String -LiteralPath $Path -SimpleMatch 'DATAPLANE TUN_RX').Count
    $counters.policy_selected = @(Select-String -LiteralPath $Path -SimpleMatch 'DATAPLANE POLICY_SELECTED').Count
    $counters.remote_tx = @(Select-String -LiteralPath $Path -SimpleMatch 'DATAPLANE REMOTE_TX').Count
    $counters.remote_rx = @(Select-String -LiteralPath $Path -SimpleMatch 'DATAPLANE REMOTE_RX').Count
    $counters.local_rx = @(Select-String -LiteralPath $Path -SimpleMatch 'DATAPLANE LOCAL_RX').Count
    $counters.wintun_allocated = @(Select-String -LiteralPath $Path -SimpleMatch 'WINTUN_ALLOCATED').Count
    $counters.wintun_submit_ok = @(Select-String -LiteralPath $Path -SimpleMatch 'WINTUN_SUBMIT_OK').Count
    $counters.wintun_submit_fail = @(Select-String -LiteralPath $Path -SimpleMatch 'WINTUN_SUBMIT_FAIL').Count
    $counters.duplicate_syn = @(Select-String -LiteralPath $Path -SimpleMatch 'repeated SYN on existing flow').Count
    $counters.static_echo_fallback = @(Select-String -LiteralPath $Path -SimpleMatch 'falling back to main tunnel').Count
    return $counters
}

function Get-CoreLogFlowIds {
    param(
        [string]$Path,
        [string]$Marker
    )
    if ([string]::IsNullOrWhiteSpace($Path) -or -not (Test-Path -LiteralPath $Path)) {
        return
    }
    Select-String -LiteralPath $Path -SimpleMatch $Marker | ForEach-Object {
        if ($_.Line -match 'flow_id=(\d+)') {
            [UInt64]$Matches[1]
        }
    }
}

function Invoke-CurlSeries {
    param(
        [string]$Name,
        [ValidateSet('direct', 'socks-ip', 'socks-domain', 'http-connect')]
        [string]$Mode,
        [string]$Url,
        [string]$Proxy,
        [int]$Count,
        [string]$Path
    )
    $succeeded = 0
    $failed = 0
    for ($index = 1; $index -le $Count; $index++) {
        $arguments = @('--connect-timeout', '10', '--max-time', '30', '--silent', '--show-error',
            '--output', 'NUL', '--write-out', '%{http_code}')
        if ($Mode -eq 'socks-ip') { $arguments += @('--proxy', "socks5://$Proxy") }
        elseif ($Mode -eq 'socks-domain') { $arguments += @('--proxy', "socks5h://$Proxy") }
        elseif ($Mode -eq 'http-connect') { $arguments += @('--proxy', "http://$Proxy") }
        $arguments += $Url
        $attempt = [Diagnostics.Stopwatch]::StartNew()
        $curlOutput = & curl.exe @arguments 2>&1
        $curlExitCode = $LASTEXITCODE
        $attempt.Stop()
        $httpCode = "$curlOutput".Trim()
        $ok = $curlExitCode -eq 0 -and $httpCode -match '^2\d\d$|^3\d\d$'
        if ($ok) { $succeeded++ } else { $failed++ }
        [ordered]@{
            gate = $Name
            attempt = $index
            ok = $ok
            exit_code = $curlExitCode
            http_code = $httpCode
            elapsed_ms = $attempt.ElapsedMilliseconds
        } | ConvertTo-Json -Compress | Add-Content -LiteralPath $Path -Encoding utf8
    }
    return [pscustomobject]@{ succeeded = $succeeded; failed = $failed; count = $Count }
}

function Invoke-DownloadSeries {
    param(
        [string]$Name,
        [string]$Url,
        [string]$ExpectedSha256,
        [long]$MinimumBytes,
        [int]$Count,
        [string]$Path,
        [string]$TemporaryPath
    )
    $succeeded = 0
    $failed = 0
    for ($index = 1; $index -le $Count; $index++) {
        $attempt = [Diagnostics.Stopwatch]::StartNew()
        $curlOutput = & curl.exe --connect-timeout 10 --max-time 600 --silent --show-error `
            --output $TemporaryPath --write-out '%{http_code}' $Url 2>&1
        $curlExitCode = $LASTEXITCODE
        $attempt.Stop()
        $actualBytes = if (Test-Path -LiteralPath $TemporaryPath) {
            (Get-Item -LiteralPath $TemporaryPath).Length
        } else { 0 }
        $actualSha256 = if ($curlExitCode -eq 0 -and $actualBytes -ge $MinimumBytes) {
            (Get-FileHash -LiteralPath $TemporaryPath -Algorithm SHA256).Hash.ToLowerInvariant()
        } else { '' }
        $httpCode = "$curlOutput".Trim()
        $ok = $curlExitCode -eq 0 -and $httpCode -match '^2\d\d$' -and
            $actualBytes -ge $MinimumBytes -and $actualSha256 -eq $ExpectedSha256.ToLowerInvariant()
        if ($ok) { $succeeded++ } else { $failed++ }
        [ordered]@{
            gate = $Name
            attempt = $index
            ok = $ok
            exit_code = $curlExitCode
            http_code = $httpCode
            elapsed_ms = $attempt.ElapsedMilliseconds
            bytes = $actualBytes
            sha256 = $actualSha256
        } | ConvertTo-Json -Compress | Add-Content -LiteralPath $Path -Encoding utf8
        if (Test-Path -LiteralPath $TemporaryPath) { Remove-Item -LiteralPath $TemporaryPath -Force }
    }
    return [pscustomobject]@{ succeeded = $succeeded; failed = $failed; count = $Count }
}

if (-not (Test-IsAdministrator)) {
    throw 'Run this acceptance test from an elevated PowerShell session.'
}

if ($FullGate) {
    foreach ($requiredValue in @($SocksProxy, $SocksIpv4Url, $SocksDomainUrl, $HttpProxy,
        $HttpConnectUrl, $Download10MbUrl, $Download10MbSha256,
        $Download100MbUrl, $Download100MbSha256, $PythonExecutable)) {
        if ([string]::IsNullOrWhiteSpace($requiredValue)) {
            throw 'FullGate requires SOCKS/HTTP targets, both download URLs and SHA256 values, and PythonExecutable.'
        }
    }
    $socksIpv4Uri = [Uri]$SocksIpv4Url
    $parsedIp = $null
    if (-not [Net.IPAddress]::TryParse($socksIpv4Uri.DnsSafeHost, [ref]$parsedIp) -or
        $parsedIp.AddressFamily -ne [Net.Sockets.AddressFamily]::InterNetwork) {
        throw 'SocksIpv4Url must use a numeric IPv4 address so this gate cannot silently become a domain test.'
    }
    $socksDomainUri = [Uri]$SocksDomainUrl
    $domainAsIp = $null
    if ([Net.IPAddress]::TryParse($socksDomainUri.DnsSafeHost, [ref]$domainAsIp)) {
        throw 'SocksDomainUrl must use a DNS hostname so this gate cannot silently become a numeric-IP test.'
    }
    if (([Uri]$HttpUrl).Scheme -ne 'https' -or ([Uri]$HttpConnectUrl).Scheme -ne 'https') {
        throw 'FullGate requires HTTPS URLs for both the short-request and HTTP CONNECT gates.'
    }
    if ($Download10MbSha256 -notmatch '^[0-9a-fA-F]{64}$' -or
        $Download100MbSha256 -notmatch '^[0-9a-fA-F]{64}$') {
        throw 'FullGate download SHA256 values must contain exactly 64 hexadecimal characters.'
    }
}

$CoreLog = [IO.Path]::GetFullPath($CoreLog)
if (-not (Test-Path -LiteralPath $CoreLog -PathType Leaf)) {
    throw "Core log does not exist: $CoreLog. Start the instrumented core with --dataplane-trace=yes first."
}
$appProbeConfigured = -not [string]::IsNullOrWhiteSpace($PythonExecutable)
if ($appProbeConfigured -and $null -eq (Get-Command -Name $PythonExecutable -ErrorAction SilentlyContinue)) {
    throw "Python executable not found: $PythonExecutable"
}
$toolDirectory = $PSScriptRoot
$appProbeScript = Join-Path $toolDirectory 'dataplane_app_probe.py'
$analyzerScript = Join-Path $toolDirectory 'analyze_pktmon_pcap.py'
$secretScannerScript = Join-Path $toolDirectory 'scan_log_secrets.py'
$performanceScript = Join-Path $toolDirectory 'summarize_dataplane_performance.py'
if ($appProbeConfigured -and
    (-not (Test-Path -LiteralPath $appProbeScript -PathType Leaf) -or
     -not (Test-Path -LiteralPath $analyzerScript -PathType Leaf) -or
     -not (Test-Path -LiteralPath $secretScannerScript -PathType Leaf) -or
     -not (Test-Path -LiteralPath $performanceScript -PathType Leaf))) {
    throw "Dataplane Python tools are missing from: $toolDirectory"
}

if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $OutputDirectory = Join-Path (Get-Location) "artifacts/dataplane-acceptance-$stamp"
}
$outputPath = [IO.Path]::GetFullPath($OutputDirectory)
if ((Test-Path -LiteralPath $outputPath) -and
    @(Get-ChildItem -LiteralPath $outputPath -Force -ErrorAction Stop).Count -gt 0) {
    throw "Output directory must be empty: $outputPath"
}
New-Item -ItemType Directory -Path $outputPath -Force | Out-Null

$startWallUtc = [DateTime]::UtcNow
$startMonotonicMs = [Environment]::TickCount64
$stopwatch = [Diagnostics.Stopwatch]::StartNew()
$etlPath = Join-Path $outputPath 'pktmon.etl'
$pcapPath = Join-Path $outputPath 'pktmon.pcapng'
$pingPath = Join-Path $outputPath 'ping.txt'
$dnsPath = Join-Path $outputPath 'dns.jsonl'
$socksPath = Join-Path $outputPath 'socks.jsonl'
$httpPath = Join-Path $outputPath 'http.jsonl'
$downloadPath = Join-Path $outputPath 'downloads.jsonl'
$interfacePath = Join-Path $outputPath 'interfaces.txt'
$routePath = Join-Path $outputPath 'routes.txt'
$summaryPath = Join-Path $outputPath 'summary.json'
$appProbePath = Join-Path $outputPath 'app-completed.jsonl'
$analysisPath = Join-Path $outputPath 'packet-analysis.json'
$secretScanPath = Join-Path $outputPath 'secret-scan.json'
$performancePath = Join-Path $outputPath 'performance-summary.json'

$dnsSucceeded = 0
$dnsFailed = 0
$socksSucceeded = 0
$socksFailed = 0
$fullGateFailed = 0
$fullGateResults = [ordered]@{}
$pingExitCode = -1
$pingReplies = 0
$pktmonStarted = $false
$pktmonConverted = $false
$appProbeExitCode = -1
$appCompletedRecords = 0
$fullyObservedMatches = 0
$appCorrelationGatePassed = $false
$secretScanPassed = $false
$performanceSummaryPassed = $false
$baselineCoreLogInfo = Get-Item -LiteralPath $CoreLog
$baselineCoreLogLength = $baselineCoreLogInfo.Length
$baselineCoreLogCreationUtc = $baselineCoreLogInfo.CreationTimeUtc
$baselineLogCounters = Get-CoreLogCounters -Path $CoreLog
$baselineFlowCounts = [ordered]@{
    tun_rx = @(Get-CoreLogFlowIds -Path $CoreLog -Marker 'DATAPLANE TUN_RX').Count
    policy_selected = @(Get-CoreLogFlowIds -Path $CoreLog -Marker 'DATAPLANE POLICY_SELECTED').Count
    remote_tx = @(Get-CoreLogFlowIds -Path $CoreLog -Marker 'DATAPLANE REMOTE_TX').Count
    remote_rx = @(Get-CoreLogFlowIds -Path $CoreLog -Marker 'DATAPLANE REMOTE_RX').Count
    local_rx = @(Get-CoreLogFlowIds -Path $CoreLog -Marker 'DATAPLANE LOCAL_RX').Count
    packet_built = @(Get-CoreLogFlowIds -Path $CoreLog -Marker 'DATAPLANE PACKET_BUILT').Count
    wintun_allocated = @(Get-CoreLogFlowIds -Path $CoreLog -Marker 'DATAPLANE WINTUN_ALLOCATED').Count
    wintun_submitted = @(Get-CoreLogFlowIds -Path $CoreLog -Marker 'DATAPLANE WINTUN_SUBMITTED').Count
}

$tunInterface = Get-NetIPAddress -IPAddress $TunAddress -ErrorAction SilentlyContinue |
    Select-Object -First 1
$tunInterfaceIndex = if ($null -eq $tunInterface) { -1 } else { [int]$tunInterface.InterfaceIndex }
$effectiveMtu = -1
if ($tunInterfaceIndex -ge 0) {
    $tunIpInterface = Get-NetIPInterface -InterfaceIndex $tunInterfaceIndex -AddressFamily IPv4 -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($null -ne $tunIpInterface) {
        $effectiveMtu = [int]$tunIpInterface.NlMtu
    }
}

Get-NetIPInterface |
    Sort-Object InterfaceIndex, AddressFamily |
    Format-Table InterfaceIndex, InterfaceAlias, AddressFamily, ConnectionState, InterfaceMetric, NlMtu -AutoSize |
    Out-String -Width 240 |
    Set-Content -LiteralPath $interfacePath -Encoding utf8

Get-NetRoute |
    Sort-Object InterfaceIndex, AddressFamily, DestinationPrefix, RouteMetric |
    Format-Table InterfaceIndex, AddressFamily, DestinationPrefix, NextHop, RouteMetric, State -AutoSize |
    Out-String -Width 300 |
    Set-Content -LiteralPath $routePath -Encoding utf8

try {
    & pktmon.exe start --capture --pkt-size 0 --file-name $etlPath | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "pktmon start failed with exit code $LASTEXITCODE"
    }
    $pktmonStarted = $true

    $pingOutput = & ping.exe -n $PingCount -S $TunAddress $PingTarget 2>&1
    $pingExitCode = $LASTEXITCODE
    $pingReplies = @($pingOutput | Where-Object { "$_" -match '(?i)TTL[=<]\d+' }).Count
    $pingOutput | Set-Content -LiteralPath $pingPath -Encoding utf8

    for ($index = 1; $index -le $DnsCount; $index++) {
        $attempt = [Diagnostics.Stopwatch]::StartNew()
        try {
            $answer = Resolve-DnsName -Name $DnsName -Server $DnsServer -DnsOnly -QuickTimeout -ErrorAction Stop
            $attempt.Stop()
            $dnsSucceeded++
            [ordered]@{
                attempt = $index
                ok = $true
                elapsed_ms = $attempt.ElapsedMilliseconds
                result_count = @($answer).Count
                error = $null
            } | ConvertTo-Json -Compress | Add-Content -LiteralPath $dnsPath -Encoding utf8
        }
        catch {
            $attempt.Stop()
            $dnsFailed++
            [ordered]@{
                attempt = $index
                ok = $false
                elapsed_ms = $attempt.ElapsedMilliseconds
                result_count = 0
                error = $_.Exception.Message
            } | ConvertTo-Json -Compress | Add-Content -LiteralPath $dnsPath -Encoding utf8
        }
    }

    if ($appProbeConfigured) {
        & $PythonExecutable $appProbeScript `
            --tun-address $TunAddress `
            --ping-target $PingTarget `
            --dns-server $DnsServer `
            --dns-name $DnsName `
            --icmp-count $PingCount `
            --dns-count $DnsCount `
            --output $appProbePath
        $appProbeExitCode = $LASTEXITCODE
    }

    if (-not [string]::IsNullOrWhiteSpace($SocksProxy) -and -not $FullGate) {
        $basicSocks = Invoke-CurlSeries -Name 'socks-domain' -Mode 'socks-domain' `
            -Url $HttpUrl -Proxy $SocksProxy -Count $SocksCount -Path $socksPath
        $socksSucceeded = $basicSocks.succeeded
        $socksFailed = $basicSocks.failed
    }

    if ($FullGate) {
        $fullGateResults.socks_ipv4 = Invoke-CurlSeries -Name 'socks-ipv4' -Mode 'socks-ip' `
            -Url $SocksIpv4Url -Proxy $SocksProxy -Count 50 -Path $socksPath
        $fullGateResults.socks_domain = Invoke-CurlSeries -Name 'socks-domain' -Mode 'socks-domain' `
            -Url $SocksDomainUrl -Proxy $SocksProxy -Count 50 -Path $socksPath
        $fullGateResults.http_connect = Invoke-CurlSeries -Name 'http-connect' -Mode 'http-connect' `
            -Url $HttpConnectUrl -Proxy $HttpProxy -Count 50 -Path $httpPath
        $fullGateResults.https_short = Invoke-CurlSeries -Name 'https-short' -Mode 'direct' `
            -Url $HttpUrl -Proxy '' -Count $HttpsCount -Path $httpPath
        $fullGateResults.download_10mb = Invoke-DownloadSeries -Name 'download-10mb' `
            -Url $Download10MbUrl -ExpectedSha256 $Download10MbSha256 -MinimumBytes 10000000 -Count 20 `
            -Path $downloadPath -TemporaryPath (Join-Path $outputPath 'download-10mb.tmp')
        $fullGateResults.download_100mb = Invoke-DownloadSeries -Name 'download-100mb' `
            -Url $Download100MbUrl -ExpectedSha256 $Download100MbSha256 -MinimumBytes 100000000 -Count 5 `
            -Path $downloadPath -TemporaryPath (Join-Path $outputPath 'download-100mb.tmp')
        $fullGateFailed = [int]($fullGateResults.Values |
            Measure-Object -Property failed -Sum).Sum
        $socksSucceeded = $fullGateResults.socks_ipv4.succeeded + $fullGateResults.socks_domain.succeeded
        $socksFailed = $fullGateResults.socks_ipv4.failed + $fullGateResults.socks_domain.failed
    }
}
finally {
    if ($pktmonStarted) {
        & pktmon.exe stop | Out-Null
        $pktmonStarted = $false
        if (Test-Path -LiteralPath $etlPath) {
            & pktmon.exe etl2pcap $etlPath --out $pcapPath | Out-Null
            $pktmonConverted = $LASTEXITCODE -eq 0 -and (Test-Path -LiteralPath $pcapPath)
        }
    }

    $stopwatch.Stop()
    if (-not [string]::IsNullOrWhiteSpace($CoreLog)) {
        [Threading.Thread]::Sleep(1500)
    }
    $coreLogRotatedDuringRun = $false
    $coreLogMissingDuringRun = -not (Test-Path -LiteralPath $CoreLog -PathType Leaf)
    $coreLogTruncatedDuringRun = $coreLogMissingDuringRun
    $coreLogReplacedDuringRun = $coreLogMissingDuringRun
    if (-not $coreLogMissingDuringRun) {
        $currentCoreLogInfo = Get-Item -LiteralPath $CoreLog
        $coreLogTruncatedDuringRun = $currentCoreLogInfo.Length -lt $baselineCoreLogLength
        $coreLogReplacedDuringRun = $currentCoreLogInfo.CreationTimeUtc -ne $baselineCoreLogCreationUtc
    }
    $rotatedLog = "$CoreLog.1"
    if (Test-Path -LiteralPath $rotatedLog) {
        $coreLogRotatedDuringRun = (Get-Item -LiteralPath $rotatedLog).LastWriteTimeUtc -ge $startWallUtc
    }
    if ($appProbeConfigured -and $pktmonConverted -and (Test-Path -LiteralPath $appProbePath)) {
        & $PythonExecutable $analyzerScript $pcapPath `
            --core-log $CoreLog --app-log $appProbePath --output $analysisPath | Out-Null
        if ($LASTEXITCODE -eq 0 -and (Test-Path -LiteralPath $analysisPath)) {
            $packetAnalysis = Get-Content -LiteralPath $analysisPath -Raw | ConvertFrom-Json
            $appCompletedRecords = @(
                Get-Content -LiteralPath $appProbePath | ForEach-Object { $_ | ConvertFrom-Json } |
                    Where-Object { $_.stage -eq 'APP_COMPLETED' -and $_.ok }
            ).Count
            $fullyObservedMatches = [int]$packetAnalysis.core_os_correlation.fully_observed_matches
            $appCorrelationGatePassed = $appProbeExitCode -eq 0 -and
                $appCompletedRecords -gt 0 -and
                $fullyObservedMatches -eq $appCompletedRecords
        }
    }
    if ($appProbeConfigured -and -not $coreLogRotatedDuringRun -and
        -not $coreLogTruncatedDuringRun -and -not $coreLogReplacedDuringRun) {
        & $PythonExecutable $secretScannerScript $CoreLog `
            --start-byte $baselineCoreLogLength --output $secretScanPath | Out-Null
        $secretScanPassed = $LASTEXITCODE -eq 0 -and (Test-Path -LiteralPath $secretScanPath)
    }
    if ($appProbeConfigured) {
        & $PythonExecutable $performanceScript `
            $dnsPath $socksPath $httpPath $downloadPath $appProbePath `
            --output $performancePath | Out-Null
        $performanceSummaryPassed = $LASTEXITCODE -eq 0 -and (Test-Path -LiteralPath $performancePath)
    }
    $finalLogCounters = Get-CoreLogCounters -Path $CoreLog
    $logCounters = [ordered]@{}
    foreach ($name in $finalLogCounters.Keys) {
        $delta = [long]$finalLogCounters[$name] - [long]$baselineLogCounters[$name]
        $logCounters[$name] = [Math]::Max(0, $delta)
    }
    $tunRxFlowIds = @(Get-CoreLogFlowIds -Path $CoreLog -Marker 'DATAPLANE TUN_RX' |
        Select-Object -Skip $baselineFlowCounts.tun_rx)
    $policyFlowIds = @(Get-CoreLogFlowIds -Path $CoreLog -Marker 'DATAPLANE POLICY_SELECTED' |
        Select-Object -Skip $baselineFlowCounts.policy_selected)
    $remoteTxFlowIds = @(Get-CoreLogFlowIds -Path $CoreLog -Marker 'DATAPLANE REMOTE_TX' |
        Select-Object -Skip $baselineFlowCounts.remote_tx)
    $remoteRxFlowIds = @(Get-CoreLogFlowIds -Path $CoreLog -Marker 'DATAPLANE REMOTE_RX' |
        Select-Object -Skip $baselineFlowCounts.remote_rx)
    $localRxFlowIds = @(Get-CoreLogFlowIds -Path $CoreLog -Marker 'DATAPLANE LOCAL_RX' |
        Select-Object -Skip $baselineFlowCounts.local_rx)
    $packetBuiltFlowIds = @(Get-CoreLogFlowIds -Path $CoreLog -Marker 'DATAPLANE PACKET_BUILT' |
        Select-Object -Skip $baselineFlowCounts.packet_built)
    $allocatedFlowIds = @(Get-CoreLogFlowIds -Path $CoreLog -Marker 'DATAPLANE WINTUN_ALLOCATED' |
        Select-Object -Skip $baselineFlowCounts.wintun_allocated)
    $submittedFlowIds = @(Get-CoreLogFlowIds -Path $CoreLog -Marker 'DATAPLANE WINTUN_SUBMITTED' |
        Select-Object -Skip $baselineFlowCounts.wintun_submitted)
    $tunRxUnique = @($tunRxFlowIds | Sort-Object -Unique)
    $policyUnique = @($policyFlowIds | Sort-Object -Unique)
    $remoteTxUnique = @($remoteTxFlowIds | Sort-Object -Unique)
    $remoteRxUnique = @($remoteRxFlowIds | Sort-Object -Unique)
    $localRxUnique = @($localRxFlowIds | Sort-Object -Unique)
    $injectionFlowIds = @($remoteRxFlowIds + $localRxFlowIds)
    $injectionUnique = @($injectionFlowIds | Sort-Object -Unique)
    $packetBuiltUnique = @($packetBuiltFlowIds | Sort-Object -Unique)
    $allocatedUnique = @($allocatedFlowIds | Sort-Object -Unique)
    $submittedUnique = @($submittedFlowIds | Sort-Object -Unique)
    $requestStageSetsValid = $tunRxFlowIds.Count -gt 0 -and
        $policyFlowIds.Count -gt 0 -and
        $remoteTxFlowIds.Count -gt 0 -and
        $tunRxUnique.Count -eq $tunRxFlowIds.Count -and
        $policyUnique.Count -eq $policyFlowIds.Count -and
        $remoteTxUnique.Count -eq $remoteTxFlowIds.Count -and
        @($policyUnique | Where-Object { $tunRxUnique -notcontains $_ }).Count -eq 0 -and
        @($remoteTxUnique | Where-Object { $policyUnique -notcontains $_ }).Count -eq 0
    $flowStagesMatch = $injectionFlowIds.Count -gt 0 -and
        $remoteRxUnique.Count -eq $remoteRxFlowIds.Count -and
        $localRxUnique.Count -eq $localRxFlowIds.Count -and
        $injectionUnique.Count -eq $injectionFlowIds.Count -and
        $packetBuiltUnique.Count -eq $packetBuiltFlowIds.Count -and
        $allocatedUnique.Count -eq $allocatedFlowIds.Count -and
        $submittedUnique.Count -eq $submittedFlowIds.Count -and
        @((Compare-Object -ReferenceObject $injectionUnique -DifferenceObject $packetBuiltUnique)).Count -eq 0 -and
        @((Compare-Object -ReferenceObject $packetBuiltUnique -DifferenceObject $allocatedUnique)).Count -eq 0 -and
        @((Compare-Object -ReferenceObject $allocatedUnique -DifferenceObject $submittedUnique)).Count -eq 0

    $logGatePassed = (
        (Test-Path -LiteralPath $CoreLog) -and
        -not $coreLogRotatedDuringRun -and
        -not $coreLogTruncatedDuringRun -and
        -not $coreLogReplacedDuringRun -and
        $logCounters.remote_rx -gt 0 -and
        $logCounters.policy_selected -gt 0 -and
        $logCounters.remote_tx -gt 0 -and
        $requestStageSetsValid -and
        $logCounters.wintun_allocated -gt 0 -and
        $logCounters.wintun_submit_ok -gt 0 -and
        $logCounters.wintun_allocated -eq $logCounters.wintun_submit_ok -and
        $flowStagesMatch -and
        $logCounters.wintun_validate_fail -eq 0 -and
        $logCounters.wintun_submit_fail -eq 0 -and
        $logCounters.duplicate_syn -eq 0)
    $minimumPingReplies = [Math]::Max(1, $PingCount - [Math]::Ceiling($PingCount * 0.01))
    $summary = [ordered]@{
        started_utc = $startWallUtc.ToString('o')
        finished_utc = [DateTime]::UtcNow.ToString('o')
        started_monotonic_ms = $startMonotonicMs
        finished_monotonic_ms = [Environment]::TickCount64
        elapsed_ms = $stopwatch.ElapsedMilliseconds
        tun_address = $TunAddress
        tun_interface_index = $tunInterfaceIndex
        expected_mtu = $ExpectedMtu
        effective_mtu = $effectiveMtu
        dns_server = $DnsServer
        ping_target = $PingTarget
        ping_count = $PingCount
        ping_exit_code = $pingExitCode
        ping_replies = $pingReplies
        minimum_ping_replies = $minimumPingReplies
        dns_name = $DnsName
        dns_count = $DnsCount
        dns_succeeded = $dnsSucceeded
        dns_failed = $dnsFailed
        socks_proxy_configured = -not [string]::IsNullOrWhiteSpace($SocksProxy)
        socks_count = if ([string]::IsNullOrWhiteSpace($SocksProxy)) { 0 } elseif ($FullGate) { 100 } else { $SocksCount }
        socks_succeeded = $socksSucceeded
        socks_failed = $socksFailed
        full_gate_requested = [bool]$FullGate
        full_gate_results = $fullGateResults
        full_gate_failed = $fullGateFailed
        pktmon_converted = $pktmonConverted
        app_probe_configured = $appProbeConfigured
        app_probe_exit_code = $appProbeExitCode
        app_completed_records = $appCompletedRecords
        fully_observed_matches = $fullyObservedMatches
        app_correlation_gate_passed = $appCorrelationGatePassed
        secret_scan_configured = $appProbeConfigured
        secret_scan_passed = $secretScanPassed
        performance_summary_passed = $performanceSummaryPassed
        core_log_missing_during_run = $coreLogMissingDuringRun
        core_log_rotated_during_run = $coreLogRotatedDuringRun
        core_log_truncated_during_run = $coreLogTruncatedDuringRun
        core_log_replaced_during_run = $coreLogReplacedDuringRun
        flow_stage_counts = [ordered]@{
            tun_rx = $tunRxFlowIds.Count
            policy_selected = $policyFlowIds.Count
            remote_tx = $remoteTxFlowIds.Count
            remote_rx = $remoteRxFlowIds.Count
            local_rx = $localRxFlowIds.Count
            packet_built = $packetBuiltFlowIds.Count
            wintun_allocated = $allocatedFlowIds.Count
            wintun_submitted = $submittedFlowIds.Count
        }
        flow_stages_match = $flowStagesMatch
        request_stage_sets_valid = $requestStageSetsValid
        log_gate_passed = $logGatePassed
        gate_scope = if ($FullGate) { 'automated_extended_runtime' } else { 'automated_base_only' }
        manual_gates_remaining = @(
            'Review pktmon/WFP evidence for relevant drops',
            $(if (-not $appProbeConfigured) { 'Run with -PythonExecutable to correlate core, OS and APP_COMPLETED' }),
            $(if (-not $appProbeConfigured) { 'Run the credential scanner against the test-window core log' }),
            $(if (-not $appProbeConfigured) { 'Generate P50/P95/P99 latency and throughput evidence' }),
            $(if (-not $FullGate) { 'Run SOCKS IPv4/domain, HTTP CONNECT, HTTPS and checksum download gates with -FullGate' }),
            'Run restart, fault-injection, physical-switch and 24-hour gates'
        ) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
        log_counters = $logCounters
        passed = $pingExitCode -eq 0 -and $pingReplies -ge $minimumPingReplies -and `
            $dnsFailed -eq 0 -and $socksFailed -eq 0 -and $pktmonConverted -and `
            $effectiveMtu -eq $ExpectedMtu -and $logGatePassed -and
            (-not $appProbeConfigured -or $appCorrelationGatePassed) -and
            (-not $appProbeConfigured -or $secretScanPassed) -and
            (-not $appProbeConfigured -or $performanceSummaryPassed) -and
            (-not $FullGate -or $fullGateFailed -eq 0)
    }
    $summary | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $summaryPath -Encoding utf8
}

$result = Get-Content -LiteralPath $summaryPath -Raw | ConvertFrom-Json
Write-Host "Automated gate evidence ($($result.gate_scope)): $outputPath"
Write-Host "DNS: $($result.dns_succeeded)/$($result.dns_count), SOCKS failures: $($result.socks_failed), ping replies: $($result.ping_replies)/$($result.ping_count), MTU: $($result.effective_mtu)"
Write-Warning 'A passing result does not close the remediation: WFP/OS/application correlation and the remaining runtime gates still require review.'
if (-not $result.passed) {
    exit 1
}
