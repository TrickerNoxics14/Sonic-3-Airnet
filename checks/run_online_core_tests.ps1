$ErrorActionPreference = "Stop"
$workspaceRoot = Split-Path -Parent $PSScriptRoot
$sourceRoot = Join-Path $workspaceRoot "sonic3air_online_stable_source"
$testSource = Join-Path $PSScriptRoot "stable_online_core_tests.cpp"
$testDir = Join-Path $PSScriptRoot "last_logs"
$testExe = Join-Path $testDir "stable_online_core_tests.exe"
$batchFile = Join-Path $testDir "run_stable_online_core_tests.cmd"
$vsDevCmd = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat"
$engineInclude = Join-Path $sourceRoot "Oxygen\oxygenengine\source"
$rmxInclude = Join-Path $sourceRoot "librmx\source"
$relayInclude = Join-Path $sourceRoot "Oxygen\oxygenserver\source\relay"
$sonic3airInclude = Join-Path $sourceRoot "Oxygen\sonic3air\source"		# sonic3air/client/SelfUpdateCore.h (the in-game updater logic)
$protocolInclude = Join-Path $engineInclude "oxygen_netcore\network"		# RelayProtocol.h, which the relay's own files include by plain name
$netcoreLib = Join-Path $sourceRoot "Oxygen\oxygenengine\build\_vstudio\lib\Release_x64"
$rmxLib = Join-Path $sourceRoot "librmx\lib\x64"
$zlibLib = Join-Path $sourceRoot "framework\external\zlib\lib\Release_x64"

foreach ($path in @($testSource, $vsDevCmd, $engineInclude, $relayInclude, $rmxInclude, $netcoreLib, $rmxLib, $zlibLib)) {
    if (-not (Test-Path -LiteralPath $path)) { throw "Required test input is missing: $path" }
}
New-Item -ItemType Directory -Path $testDir -Force | Out-Null

$batch = @"
@echo off
call "$vsDevCmd" -arch=x64 -host_arch=x64 >nul 2>&1
if errorlevel 1 exit /b 1
cl.exe /nologo /EHsc /std:c++17 /I"$engineInclude" /I"$rmxInclude" /I"$relayInclude" /I"$protocolInclude" /I"$sonic3airInclude" "$testSource" /Fe:"$testExe" /link /LTCG /LIBPATH:"$netcoreLib" /LIBPATH:"$rmxLib" /LIBPATH:"$zlibLib" oxygen_netcore.lib rmxbase.lib zlib.lib ws2_32.lib user32.lib
if errorlevel 1 exit /b %errorlevel%
"$testExe"
exit /b %errorlevel%
"@
Set-Content -LiteralPath $batchFile -Value $batch -Encoding ASCII

& "$env:WINDIR\System32\cmd.exe" /d /c $batchFile
if ($LASTEXITCODE -ne 0) { throw "Online core tests failed with exit code $LASTEXITCODE" }
