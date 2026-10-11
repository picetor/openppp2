<#
.SYNOPSIS
Talks to the OpenPPP2 core control API (--rpc-listen), see docs/AI_CONTROL_API_CN.md.

.DESCRIPTION
Wire format is a four byte big endian length followed by UTF-8 JSON.  Every
connection authenticates first with `hello`; when the core has no token
configured an empty token authenticates.  This helper does that handshake on
the same socket and then issues the requested method.

.PARAMETER Method
API method, e.g. describe_api, get_health, run_diagnostics, get_snapshot,
get_logs, get_outbounds, get_settings, set_log_level, switch_server, shutdown.

.PARAMETER ParamsJson
Method parameters as JSON (default {}).  Note that powershell -File strips
quotes from arguments, so prefer the -ParamsFile option for anything complex.

.PARAMETER ParamsFile
Read the parameters from a file instead (avoids argument quoting problems).

.PARAMETER Endpoint
host:port of the core API, default 127.0.0.1:39100.

.EXAMPLE
powershell -File tools/core-api.ps1 -Method get_health
.EXAMPLE
powershell -File tools/core-api.ps1 -Method run_diagnostics -ParamsJson '{"scope":"quick"}'
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string] $Method,
    [string] $ParamsJson = '{}',
    [string] $ParamsFile,
    [string] $Endpoint = '127.0.0.1:39100',
    [string] $Token = '',
    [int]    $TimeoutMs = 15000
)
$ErrorActionPreference = 'Stop'

if ($ParamsFile) {
    if (-not (Test-Path $ParamsFile)) { throw "params file not found: $ParamsFile" }
    $ParamsJson = Get-Content $ParamsFile -Raw
}

$parts = $Endpoint.Split(':')
$hostName = $parts[0]
$port = if ($parts.Count -gt 1) { [int]$parts[1] } else { 39100 }

$client = New-Object System.Net.Sockets.TcpClient
$client.Connect($hostName, $port)
$stream = $client.GetStream()
$stream.ReadTimeout = $TimeoutMs

function Send-Frame([string] $json) {
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($json)
    $frame = New-Object byte[] ($bytes.Length + 4)
    $frame[0] = [byte](($bytes.Length -shr 24) -band 0xFF)
    $frame[1] = [byte](($bytes.Length -shr 16) -band 0xFF)
    $frame[2] = [byte](($bytes.Length -shr 8) -band 0xFF)
    $frame[3] = [byte]($bytes.Length -band 0xFF)
    [Array]::Copy($bytes, 0, $frame, 4, $bytes.Length)
    $stream.Write($frame, 0, $frame.Length)
    $stream.Flush()
}

function Read-Frame {
    $header = New-Object byte[] 4
    $read = 0
    while ($read -lt 4) { $n = $stream.Read($header, $read, 4 - $read); if ($n -le 0) { return $null }; $read += $n }
    $length = ($header[0] -shl 24) -bor ($header[1] -shl 16) -bor ($header[2] -shl 8) -bor $header[3]
    $body = New-Object byte[] $length
    $got = 0
    while ($got -lt $length) { $n = $stream.Read($body, $got, $length - $got); if ($n -le 0) { break }; $got += $n }
    return [System.Text.Encoding]::UTF8.GetString($body, 0, $got)
}

# Every session authenticates first; an empty token is fine when the core has none.
Send-Frame ('{"id":1,"method":"hello","params":{"token":"' + $Token + '"}}')
$hello = Read-Frame
if (-not $hello) { $client.Close(); throw 'no response to hello' }

if ($Method -eq 'hello') {
    $client.Close()
    Write-Output $hello
    exit 0
}

Send-Frame ('{"id":2,"method":"' + $Method + '","params":' + $ParamsJson + '}')
$response = Read-Frame
$client.Close()
Write-Output $response
