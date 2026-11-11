# Builds the relay server program (a single C++ file, no libraries) into checks\last_logs\relay\s3air_relay.exe.
# The relay is a separate program that someone runs on a server; it is not part of the game package (see checks\package_files\DEPLOY_RELAY.txt).
$ErrorActionPreference = "Stop"
$workspaceRoot = Split-Path -Parent $PSScriptRoot
$relaySource = Join-Path $workspaceRoot "sonic3air_online_stable_source\Oxygen\oxygenserver\source\relay"
$protocolFolder = Join-Path $workspaceRoot "sonic3air_online_stable_source\Oxygen\oxygenengine\source\oxygen_netcore\network"
$outDir = Join-Path $PSScriptRoot "last_logs\relay"
$outExe = Join-Path $outDir "s3air_relay.exe"
$vsDevCmd = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat"

foreach ($path in @((Join-Path $relaySource "s3air_relay.cpp"), (Join-Path $protocolFolder "RelayProtocol.h"), $vsDevCmd)) {
    if (-not (Test-Path -LiteralPath $path)) { throw "Required file is missing: $path" }
}
New-Item -ItemType Directory -Path $outDir -Force | Out-Null
if (Test-Path -LiteralPath $outExe) { Remove-Item -LiteralPath $outExe -Force }

$batchFile = Join-Path $outDir "build_relay.cmd"
$batch = @"
@echo off
call "$vsDevCmd" -arch=x64 -host_arch=x64 >nul 2>&1
if errorlevel 1 exit /b 1
cd /d "$outDir"
cl.exe /nologo /EHsc /O2 /W3 /std:c++17 /I"$protocolFolder" /I"$relaySource" "$relaySource\s3air_relay.cpp" /Fe:"$outExe" /Fo:"$outDir\\" ws2_32.lib
exit /b %errorlevel%
"@
Set-Content -LiteralPath $batchFile -Value $batch -Encoding ASCII
& "$env:WINDIR\System32\cmd.exe" /d /c $batchFile
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $outExe)) { throw "Relay build failed (exit $LASTEXITCODE)" }
Write-Output "PASS relay server built: $outExe"
