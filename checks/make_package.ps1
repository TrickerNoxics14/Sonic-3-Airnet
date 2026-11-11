# Builds the release package from the current engine build and the current Drop-In Co-op scripts:
#   Sonic3AIR_Online\   (folder)
#   Sonic3AIR_Online.zip
# Excluded on purpose: saves, compiled script caches, save states, developer test mods and the ROM.

param([string]$Version = "")		# x.y.z: written into the package as version.txt (release.ps1 passes it); without it the package is an unversioned development build, and its updater stays off
$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$game = Join-Path $root "sonic3air_online_stable_source\Oxygen\sonic3air"
$bin = Join-Path $game "bin\Release_x64"
$out = Join-Path $root "Sonic3AIR_Online"
$zipPath = Join-Path $root "Sonic3AIR_Online.zip"

if (-not (Test-Path (Join-Path $bin "Sonic3AIR.exe")))
{
	throw "No stable online fork build found in $bin. Run checks\run_online_backend_checks.ps1 first."
}

$resolvedOut = [IO.Path]::GetFullPath($out)
$expectedOut = [IO.Path]::GetFullPath((Join-Path $root 'Sonic3AIR_Online'))
if ($resolvedOut -ne $expectedOut -or (Split-Path -Parent $resolvedOut) -ne [IO.Path]::GetFullPath($root)) {
	throw 'Package output must be the Sonic3AIR_Online directory inside the workspace.'
}
if (Test-Path -LiteralPath $resolvedOut) { Remove-Item -LiteralPath $resolvedOut -Recurse -Force }
New-Item -ItemType Directory -Path $out | Out-Null

# Engine
Copy-Item (Join-Path $bin "Sonic3AIR.exe") $out
Copy-Item (Join-Path $bin "discord_game_sdk.dll") $out

# Game data and scripts (the same as in the game folder, without saves)
Copy-Item (Join-Path $game "data") (Join-Path $out "data") -Recurse
Copy-Item (Join-Path $game "scripts") (Join-Path $out "scripts") -Recurse
# Engine data (icon, fonts, shaders, engine sprites) lives in a separate source folder and is merged into data\.
# Without it the game stops at start with "Could not find engine data".
$engineData = Join-Path $root "sonic3air_online_stable_source\Oxygen\oxygenengine\data"
if (-not (Test-Path -LiteralPath (Join-Path $engineData "oxygen_icon.png"))) { throw "Engine data folder not found or incomplete: $engineData" }
Copy-Item (Join-Path $engineData "*") (Join-Path $out "data") -Recurse -Force
if (-not (Test-Path -LiteralPath (Join-Path $out "data\oxygen_icon.png"))) { throw "Package is missing data\oxygen_icon.png (engine data)" }
Copy-Item (Join-Path $game "oxygenproject.json") $out

# Settings for players: normal start, no developer save state
$config = Get-Content (Join-Path $game "config.json") -Raw
$config = $config.Replace('"StartPhase": "3"', '"StartPhase": "2"')
$config = [regex]::Replace($config, '(?m)^(\s*)"LoadSaveState": "[^"]*",', '$1//"LoadSaveState": "state_01",')
$config = [regex]::Replace($config, '(?m)^(\s*)"LoadLevel": ', '$1//"LoadLevel": ')
Set-Content -Path (Join-Path $out "config.json") -Value $config -Encoding utf8

# Readme and the co-op mod, which the game installs by itself at start
Copy-Item (Join-Path $root "checks\package_files\README_ONLINE.txt") $out
# The in-game updater (Options > Check for updates) starts this script to install a downloaded update. It lies in the game folder, so what runs is what was installed earlier, never what was downloaded.
Copy-Item (Join-Path $root "checks\package_files\update_helper.ps1") $out
if ($Version -ne "") {
	if ($Version -notmatch '^\d{1,5}\.\d{1,5}\.\d{1,5}$') { throw "Version must look like 1.2.3, got: $Version" }
	Set-Content -LiteralPath (Join-Path $out "version.txt") -Value $Version -Encoding ascii -NoNewline
}
# The optional relay server: how to run one, and its source (a program for somebody who hosts a server; nothing in the game starts it, and no relay is deployed by us)
Copy-Item (Join-Path $root "checks\package_files\DEPLOY_RELAY.txt") $out
$relaySourceOut = Join-Path $out "relay_server_source"
New-Item -ItemType Directory -Path $relaySourceOut | Out-Null
$oxygenRoot = Join-Path $root "sonic3air_online_stable_source\Oxygen"
foreach ($relayFile in @("oxygenserver\source\relay\s3air_relay.cpp", "oxygenserver\source\relay\RelayCore.h", "oxygenengine\source\oxygen_netcore\network\RelayProtocol.h")) {
	$relayPath = Join-Path $oxygenRoot $relayFile
	if (-not (Test-Path -LiteralPath $relayPath)) { throw "Relay source file not found: $relayPath" }
	Copy-Item $relayPath $relaySourceOut
}
# The engine is GPL-3.0 software: ship its license text with every binary copy.
$licenseSource = Join-Path $root "sonic3air_online_stable_source\LICENSE"
if (-not (Test-Path -LiteralPath $licenseSource)) { throw "Engine LICENSE not found: $licenseSource (a GPL binary must not be packaged without it)" }
Copy-Item $licenseSource (Join-Path $out "LICENSE.txt")
$bundled = Join-Path $out "bundled_mods"
New-Item -ItemType Directory -Path $bundled | Out-Null
Copy-Item (Join-Path $root "DropInCoop") (Join-Path $bundled "DropInCoop") -Recurse

# The developer test hooks (environment variables) are honoured only if this marker file lies next to the executable. It must never be shipped.
$markerFiles = @(Get-ChildItem -LiteralPath $out -File -Recurse -Filter "testhooks.enabled" -ErrorAction SilentlyContinue)
if ($markerFiles.Count -gt 0) { throw "Refusing to package the developer test-hooks marker: $($markerFiles[0].FullName)" }

# Keep player data and developer fixtures out of the redistributable.
$forbiddenFiles = @(Get-ChildItem -LiteralPath $out -File -Recurse | Where-Object {
	$_.FullName -match '(?i)[\\/](saves|states|ChecksHarness|ActHarness|BossHarness|NetPartyHarness|PartyHarness|TeleportHarness)([\\/]|$)' -or
	$_.Name -match '(?i)sonic_knuckles|sonic_3_and|_harness' -or
	$_.Extension -match '(?i)^\.(md|gen|smd)$' -or
	($_.Extension -eq '.bin' -and $_.Length -gt 512KB)
})
if ($forbiddenFiles.Count -gt 0) { throw "Refusing to package a save, ROM, or test fixture: $($forbiddenFiles[0].FullName)" }

# Warn (not fail) when the exe embeds the builder's Windows user folder, e.g. through the debug-symbol path.
$exeBytes = [IO.File]::ReadAllBytes((Join-Path $out "Sonic3AIR.exe"))
$exeText = [Text.Encoding]::ASCII.GetString($exeBytes)
$leak = [regex]::Matches($exeText, '[A-Za-z]:\\Users\\[^\\\x00]{1,40}') | ForEach-Object { $_.Value } | Sort-Object -Unique
if ($leak) { Write-Warning ("Sonic3AIR.exe embeds a personal path ({0} distinct). Rebuild with /PDBALTPATH and /pathmap before public release." -f @($leak).Count) }

if (Test-Path $zipPath) { Remove-Item $zipPath -Force }
Compress-Archive -Path $out -DestinationPath $zipPath

Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::OpenRead($zipPath)
try {
	$markerEntries = @($archive.Entries | Where-Object { $_.FullName -match '(?i)(^|/)testhooks\.enabled$' })
	if ($markerEntries.Count -gt 0) { throw "Refusing to package the developer test-hooks marker: $($markerEntries[0].FullName)" }
	$forbiddenEntries = @($archive.Entries | Where-Object { $_.FullName -match '(?i)(^|/)(saves|states|ChecksHarness|ActHarness|BossHarness|NetPartyHarness|PartyHarness|TeleportHarness)(/|$)|sonic_knuckles|sonic_3_and|\.(md|gen|smd)$' -or ($_.FullName -match '\.bin$' -and $_.Length -gt 512KB) })
	if ($forbiddenEntries.Count -gt 0) { throw "Refusing to package a save, ROM, or test fixture: $($forbiddenEntries[0].FullName)" }
}
finally { $archive.Dispose() }

Write-Output ("Package written: " + $zipPath + " (" + [math]::Round((Get-Item $zipPath).Length / 1MB, 1) + " MB)")
