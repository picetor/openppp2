[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$CertificateThumbprint,
    [string]$Configuration = "Release",
    [ValidateSet("x64", "ARM64")]
    [string]$Platform = "x64",
    [string]$TimestampUrl = "http://timestamp.digicert.com"
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$wfpRoot = Join-Path $repoRoot "windows\wfp"
$outRoot = Join-Path $repoRoot "bin\wfp\$Platform"
$msbuild = Get-Command msbuild.exe -ErrorAction SilentlyContinue
$inf2cat = Get-Command inf2cat.exe -ErrorAction SilentlyContinue
$signtool = Get-Command signtool.exe -ErrorAction SilentlyContinue

if ($null -eq $msbuild -or $null -eq $inf2cat -or $null -eq $signtool) {
    throw "WDK tools are required: msbuild.exe, inf2cat.exe and signtool.exe"
}

New-Item -ItemType Directory -Force -Path $outRoot | Out-Null

$buildArgs = @(
    (Join-Path $wfpRoot "OpenPpp2WfpNat66.vcxproj"),
    "/t:Build",
    "/p:Configuration=$Configuration",
    "/p:Platform=$Platform",
    "/p:OutDir=$outRoot\",
    "/nologo"
)
& $msbuild.Source @buildArgs
if ($LASTEXITCODE -ne 0) { throw "WFP driver build failed" }

$infPath = Join-Path $outRoot "OpenPpp2WfpNat66.inf"
Copy-Item (Join-Path $wfpRoot "OpenPpp2WfpNat66.inf") $infPath -Force
$targetOs = if ($Platform -eq "ARM64") { "10_ARM64" } else { "10_X64" }
$inf2catArgs = @("/driver:$outRoot", "/os:$targetOs")
& $inf2cat.Source @inf2catArgs
if ($LASTEXITCODE -ne 0) { throw "Inf2Cat failed" }

$cert = "Cert:\CurrentUser\My\$CertificateThumbprint"
if (-not (Test-Path $cert)) { throw "Signing certificate not found: $cert" }

& $signtool.Source sign /fd SHA256 /td SHA256 /tr $TimestampUrl /sha1 $CertificateThumbprint (Join-Path $outRoot "OpenPpp2WfpNat66.sys")
if ($LASTEXITCODE -ne 0) { throw "Driver signing failed" }

& $signtool.Source sign /fd SHA256 /td SHA256 /tr $TimestampUrl /sha1 $CertificateThumbprint (Join-Path $outRoot "OpenPpp2WfpNat66.cat")
if ($LASTEXITCODE -ne 0) { throw "Catalog signing failed" }

& $signtool.Source verify /kp /c (Join-Path $outRoot "OpenPpp2WfpNat66.cat") (Join-Path $outRoot "OpenPpp2WfpNat66.sys")
if ($LASTEXITCODE -ne 0) { throw "Kernel policy signature verification failed" }

Write-Host "Signed WFP package: $outRoot"
