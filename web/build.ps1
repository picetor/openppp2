param([switch]$SkipCore)
$ErrorActionPreference='Stop'
$repo=Split-Path $PSScriptRoot -Parent
$vswhere="${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$msbuild=& $vswhere -latest -products '*' -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if(!$msbuild){throw 'MSBuild not found'}
if(!$SkipCore){& $msbuild "$repo\ppp.vcxproj" /t:Build /p:Configuration=Release /p:Platform=x64 /p:BuildCoreLibrary=true /p:PreferredToolArchitecture=x64 /m:1 /v:minimal;if($LASTEXITCODE){throw 'Core build failed'}}
& $msbuild "$PSScriptRoot\ppp-web.vcxproj" /t:Build /p:Configuration=Release /p:Platform=x64 /p:PreferredToolArchitecture=x64 /m:1 /v:minimal
if($LASTEXITCODE){throw 'Web host build failed'}
New-Item -ItemType Directory -Path "$PSScriptRoot\dist\web" -Force | Out-Null
Copy-Item -LiteralPath "$PSScriptRoot\index.html","$PSScriptRoot\style.css","$PSScriptRoot\app.js" -Destination "$PSScriptRoot\dist\web" -Force
Write-Host "Built $PSScriptRoot\dist\ppp-web.exe"
