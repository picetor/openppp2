[CmdletBinding()]
param(
    [ValidateSet('before', 'failed', 'after', 'manual')]
    [string]$Phase = 'failed',

    [string]$AppDir = $PSScriptRoot,

    [string]$PanelUrl = 'http://127.0.0.1:19999/',

    [int]$RpcPort = 39100,

    [ValidateRange(50, 5000)]
    [int]$LogTailLines = 500
)

$ErrorActionPreference = 'Continue'
$startedAt = Get-Date
$timestamp = $startedAt.ToString('yyyyMMdd-HHmmss')
$reportDir = Join-Path $AppDir 'diagnostics'
$reportPath = Join-Path $reportDir ("incident-{0}-{1}.txt" -f $Phase, $timestamp)
$script:ReportBuilder = New-Object System.Text.StringBuilder
$script:ReportPath = $reportPath
$script:SectionNumber = 0
$script:TotalSections = 11

function Protect-Text {
    param([AllowNull()][string]$Text)

    if ($null -eq $Text) { return '' }
    $safe = $Text
    $safe = [regex]::Replace(
        $safe,
        '(?i)((?:password|passwd|token|secret|authorization|api[_-]?key|access[_-]?token)\s*[:=]\s*)("[^"]*"|''[^'']*''|[^,\s;]+)',
        '$1"<REDACTED>"'
    )
    $safe = [regex]::Replace($safe, '(?i)Bearer\s+[A-Za-z0-9._~+/-]+=*', 'Bearer <REDACTED>')
    $safe = [regex]::Replace($safe, '(?i)(://)[^/@\s:]+:[^/@\s]+@', '$1<REDACTED>@')
    return $safe
}

function Add-Section {
    param(
        [Parameter(Mandatory = $true)][string]$Title,
        [Parameter(Mandatory = $true)][scriptblock]$Action
    )

    $script:SectionNumber++
    Write-Host ("[{0}/{1}] START: {2}" -f $script:SectionNumber, $script:TotalSections, $Title)
    [void]$script:ReportBuilder.AppendLine('')
    [void]$script:ReportBuilder.AppendLine(('=' * 90))
    [void]$script:ReportBuilder.AppendLine($Title)
    [void]$script:ReportBuilder.AppendLine(('=' * 90))
    Save-PartialReport
    $sectionStartedAt = Get-Date
    try {
        $captured = (& $Action 2>&1 | Out-String -Width 260)
        [void]$script:ReportBuilder.AppendLine((Protect-Text $captured))
    }
    catch {
        [void]$script:ReportBuilder.AppendLine(('采集失败: ' + (Protect-Text $_.Exception.ToString())))
    }
    $elapsed = [math]::Round(((Get-Date) - $sectionStartedAt).TotalSeconds, 1)
    [void]$script:ReportBuilder.AppendLine(("阶段耗时秒: {0}" -f $elapsed))
    Save-PartialReport
    Write-Host ("[{0}/{1}] DONE ({2}s)" -f $script:SectionNumber, $script:TotalSections, $elapsed)
}

function Save-PartialReport {
    try {
        $text = Protect-Text ($script:ReportBuilder.ToString())
        [System.IO.File]::WriteAllText($script:ReportPath, $text, [System.Text.UTF8Encoding]::new($true))
    }
    catch {
        Write-Host ("REPORT WRITE ERROR: {0}" -f $_.Exception.Message) -ForegroundColor Red
    }
}

function Test-LoopbackTcpPort {
    param([int]$Port, [int]$TimeoutMs = 2500)

    $client = New-Object System.Net.Sockets.TcpClient
    try {
        $async = $client.BeginConnect('127.0.0.1', $Port, $null, $null)
        if (-not $async.AsyncWaitHandle.WaitOne($TimeoutMs, $false)) {
            return "127.0.0.1:$Port TCP connect=TIMEOUT (${TimeoutMs}ms)"
        }
        $client.EndConnect($async)
        return "127.0.0.1:$Port TCP connect=CONNECTED"
    }
    catch {
        return ("127.0.0.1:{0} TCP connect=FAILED ({1})" -f $Port, $_.Exception.Message)
    }
    finally {
        $client.Close()
    }
}

try {
    New-Item -ItemType Directory -Path $reportDir -Force -ErrorAction Stop | Out-Null
}
catch {
    throw "无法创建报告目录 '$reportDir': $($_.Exception.Message)"
}

Write-Host 'VPN incident collection starting. Progress is printed by section; Ctrl+C leaves a partial report.' -ForegroundColor Cyan

[void]$script:ReportBuilder.AppendLine('TUI / VPN 故障现场只读采集报告')
[void]$script:ReportBuilder.AppendLine(('阶段: ' + $Phase))
[void]$script:ReportBuilder.AppendLine(('开始时间: ' + $startedAt.ToString('o')))
[void]$script:ReportBuilder.AppendLine(('电脑: ' + $env:COMPUTERNAME))
[void]$script:ReportBuilder.AppendLine(('用户: ' + $env:USERDOMAIN + '\' + $env:USERNAME))
[void]$script:ReportBuilder.AppendLine(('TUI 目录: ' + $AppDir))
[void]$script:ReportBuilder.AppendLine(('面板地址: ' + $PanelUrl))
[void]$script:ReportBuilder.AppendLine(('RPC TCP 端口: ' + $RpcPort))
[void]$script:ReportBuilder.AppendLine('本脚本只读取状态并对本机面板发起 GET 请求；不会重启程序或修改 DNS、路由、网卡、防火墙。')

Add-Section '1. Windows、PowerShell 与采集时间' {
    Get-Date -Format 'yyyy-MM-dd HH:mm:ss.fff zzz'
    $PSVersionTable | Format-List
    Get-CimInstance Win32_OperatingSystem |
        Select-Object Caption, Version, BuildNumber, LastBootUpTime, OSArchitecture |
        Format-List
}

Add-Section '2. TUI / VPN 相关进程（不采集命令行参数）' {
    $processes = Get-CimInstance Win32_Process -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '(?i)ppp|openppp|wintun|wireguard|tun' }
    if (-not $processes) { '未找到名称匹配 ppp/openppp/wintun/wireguard/tun 的进程。' }
    $processDetails = foreach ($item in $processes) {
        $p = Get-Process -Id $item.ProcessId -ErrorAction SilentlyContinue
        [pscustomobject]@{
            ProcessId = $item.ProcessId
            Name = $item.Name
            ExecutablePath = $item.ExecutablePath
            StartTime = if ($p) { $p.StartTime } else { $null }
            Responding = if ($p) { $p.Responding } else { $null }
            CPUSeconds = if ($p) { $p.CPU } else { $null }
            WorkingSetMB = if ($p) { [math]::Round($p.WorkingSet64 / 1MB, 1) } else { $null }
        }
    }
    $processDetails | Format-List
}

Add-Section '3. 面板与 RPC 端口监听、连接和 PID' {
    $ports = @(19999, $RpcPort) | Select-Object -Unique
    '--- netstat matching rows ---'
    $matchingRows = @(netstat.exe -ano -p tcp 2>&1 | Select-String -Pattern ':(19999|39100)\s')
    if ($matchingRows.Count -eq 0) { 'netstat 没有找到 19999/39100 端口。' }
    foreach ($row in $matchingRows) {
        $row
        $parts = ($row.ToString().Trim() -split '\s+')
        if ($parts.Count -ge 5 -and $parts[-1] -match '^\d+$') {
            $owner = Get-Process -Id ([int]$parts[-1]) -ErrorAction SilentlyContinue
            if ($owner) { "PID $($owner.Id) => $($owner.ProcessName) | $($owner.Path)" }
        }
    }
}

Add-Section '4. 本机环回 TCP 与网页 GET 探测' {
    Test-LoopbackTcpPort -Port 19999
    Test-LoopbackTcpPort -Port $RpcPort
    $curl = Get-Command curl.exe -ErrorAction SilentlyContinue
    if (-not $curl) {
        '找不到 curl.exe；已完成 TCP 探测，未执行 HTTP 探测。'
    }
    else {
        $base = $PanelUrl.TrimEnd('/')
        $urls = @($PanelUrl, ($base + '/session.js')) | Select-Object -Unique
        foreach ($url in $urls) {
            "--- GET $url ---"
            $curlOutput = & $curl.Source --noproxy '*' --connect-timeout 2 --max-time 5 -sS -o NUL -w "http_code=%{http_code} remote_ip=%{remote_ip} time_connect=%{time_connect}s time_total=%{time_total}s`n" $url 2>&1
            $curlExit = $LASTEXITCODE
            "curl_exit_code=$curlExit"
            $curlOutput
        }
    }
}

Add-Section '5. 网卡清单与状态' {
    '--- Get-NetAdapter ---'
    Get-NetAdapter -IncludeHidden -ErrorAction SilentlyContinue |
        Sort-Object ifIndex |
        Select-Object ifIndex, Name, InterfaceDescription, Status, MacAddress, LinkSpeed |
        Format-Table -AutoSize
}

Add-Section '6. IP 地址、网关与活动接口配置' {
    '--- Get-NetIPConfiguration ---'
    Get-NetIPConfiguration -All -ErrorAction SilentlyContinue |
        Format-List InterfaceAlias, InterfaceIndex, IPv4Address, IPv6Address, IPv4DefaultGateway, IPv6DefaultGateway, DNSServer
}

Add-Section '7. 关键路由（避免倾倒数千条分流路由）' {
    $prefixes = @(
        '0.0.0.0/0', '0.0.0.0/1', '128.0.0.0/1', '127.0.0.0/8',
        '1.1.1.1/32', '8.8.8.8/32', '::/0', '::/1', '8000::/1', '::1/128'
    )
    '--- Selected route prefixes ---'
    Get-NetRoute -DestinationPrefix $prefixes -ErrorAction SilentlyContinue |
        Sort-Object AddressFamily, DestinationPrefix, RouteMetric |
        Select-Object DestinationPrefix, NextHop, InterfaceAlias, InterfaceIndex, RouteMetric, State, PolicyStore |
        Format-Table -AutoSize
    '--- Best route lookup ---'
    foreach ($remoteAddress in @('127.0.0.1', '1.1.1.1', '8.8.8.8')) {
        "Destination: $remoteAddress"
        Find-NetRoute -RemoteIPAddress $remoteAddress -ErrorAction SilentlyContinue |
            Select-Object DestinationPrefix, NextHop, InterfaceAlias, InterfaceIndex, RouteMetric |
            Format-Table -AutoSize
    }
}

Add-Section '8. DNS、代理和防火墙配置摘要' {
    '--- DNS servers by interface ---'
    Get-DnsClientServerAddress -ErrorAction SilentlyContinue |
        Select-Object InterfaceAlias, InterfaceIndex, AddressFamily, ServerAddresses |
        Format-Table -Wrap -AutoSize
    '--- DNS client global settings ---'
    Get-DnsClientGlobalSetting -ErrorAction SilentlyContinue | Format-List
    '--- WinHTTP proxy ---'
    netsh.exe winhttp show proxy
    '--- Current-user Internet proxy values ---'
    Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Internet Settings' -ErrorAction SilentlyContinue |
        Select-Object ProxyEnable, ProxyServer, AutoConfigURL, MigrateProxy |
        Format-List
    '--- Firewall profile status ---'
    Get-NetFirewallProfile -ErrorAction SilentlyContinue |
        Select-Object Name, Enabled, DefaultInboundAction, DefaultOutboundAction |
        Format-Table -AutoSize
}

Add-Section '9. 最近两小时相关 Windows 系统 / 应用事件' {
    $since = (Get-Date).AddHours(-2)
    $pattern = '(?i)ppp|openppp|tun|tap|wintun|wireguard|tcpip|ndis|network|dns|dhcp'
    foreach ($logName in @('System', 'Application')) {
        "--- $logName ---"
        try {
            Get-WinEvent -FilterHashtable @{ LogName = $logName; StartTime = $since; Level = 1, 2, 3 } -MaxEvents 250 -ErrorAction Stop |
                Where-Object { $_.ProviderName -match $pattern -or $_.Message -match $pattern } |
                Select-Object -First 60 TimeCreated, Id, LevelDisplayName, ProviderName, ProcessId, Message |
                Format-List
        }
        catch { "读取 $logName 事件失败或没有匹配事件: $($_.Exception.Message)" }
    }
}

Add-Section '10. TUI 目录中的日志文件清单与最新日志尾部' {
    $searchRoots = @($AppDir)
    foreach ($directoryName in @('log', 'logs', 'logfiles')) {
        $candidate = Join-Path $AppDir $directoryName
        if (Test-Path -LiteralPath $candidate -PathType Container) { $searchRoots += $candidate }
    }
    $files = @()
    foreach ($root in ($searchRoots | Select-Object -Unique)) {
        $files += Get-ChildItem -LiteralPath $root -File -ErrorAction SilentlyContinue |
            Where-Object { $_.Extension -in @('.log', '.txt') -and $_.Name -match '(?i)log|core|error|host|incident|failed' -and $_.Length -le 50MB }
    }
    $files = @($files | Sort-Object FullName -Unique | Sort-Object LastWriteTime -Descending)
    if (-not $files) { '未在 TUI 根目录及 log/logs/logfiles 子目录找到匹配的 .log/.txt 文件。' }
    else {
        $files | Select-Object -First 12 FullName, Length, LastWriteTime | Format-Table -AutoSize
        foreach ($file in ($files | Select-Object -First 8)) {
            "--- $($file.FullName) (最后 $LogTailLines 行) ---"
            try {
                $tail = Get-Content -LiteralPath $file.FullName -Tail $LogTailLines -ErrorAction Stop
                $tail -join "`r`n"
            }
            catch { "读取失败: $($_.Exception.Message)" }
        }
    }
}

Add-Section '11. 最新 incident / failed JSON（最多 3 个，单文件最多 2 MB）' {
    $jsonFiles = Get-ChildItem -LiteralPath $AppDir -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Extension -eq '.json' -and $_.Name -match '(?i)^(incident|failed)[-_]' } |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 3
    if (-not $jsonFiles) { 'TUI 根目录没有 incident-*.json 或 failed-*.json。' }
    foreach ($file in $jsonFiles) {
        "--- $($file.FullName), $($file.Length) bytes, $($file.LastWriteTime) ---"
        if ($file.Length -gt 2MB) {
            '文件超过 2 MB，为避免报告过大，只记录元数据；请单独附上该 JSON。'
            continue
        }
        try { Get-Content -LiteralPath $file.FullName -Raw -ErrorAction Stop }
        catch { "读取失败: $($_.Exception.Message)" }
    }
}

$finishedAt = Get-Date
[void]$script:ReportBuilder.AppendLine('')
[void]$script:ReportBuilder.AppendLine(('采集结束时间: ' + $finishedAt.ToString('o')))
[void]$script:ReportBuilder.AppendLine(('采集耗时秒: ' + [math]::Round(($finishedAt - $startedAt).TotalSeconds, 1)))

$reportText = Protect-Text ($script:ReportBuilder.ToString())
[System.IO.File]::WriteAllText($reportPath, $reportText, [System.Text.UTF8Encoding]::new($true))
Write-Host ''
Write-Host '采集完成，报告路径：' -ForegroundColor Green
Write-Host $reportPath -ForegroundColor Cyan
Write-Host '报告可能包含局域网 IP、网卡信息和日志内容；发送前请快速检查是否有未识别的敏感内容。'
