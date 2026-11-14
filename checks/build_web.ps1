<#
  Clean web (WebAssembly) build of Sonic 3 AirNet, single player. See checks\package_files\WEB_BUILD.md.

  -EmsdkDir   folder of the official Emscripten SDK (https://github.com/emscripten-core/emsdk), already installed and activated
  -OutDir     result folder (default: web_build in the workspace root, untracked, contains NO ROM)
  -PackExe    a Windows build of this fork (Sonic3AIR.exe). Its "-pack" option creates the data packages, and one short start
              creates the compiled script file. Default: the Release_x64 build inside the source tree.
  -Jobs       parallel compile jobs (default 4; more can run out of memory in clang)
#>
param(
	[Parameter(Mandatory = $true)][string]$EmsdkDir,
	[string]$OutDir,
	[string]$PackExe,
	[int]$Jobs = 4
)
$ErrorActionPreference = "Stop"
$PSNativeCommandUseErrorActionPreference = $false
$root = Split-Path -Parent $PSScriptRoot
$oxy  = Join-Path $root "sonic3air_online_stable_source\Oxygen"
$cmakeSrc = Join-Path $oxy "sonic3air\build\_emscripten"
if (-not $OutDir) { $OutDir = Join-Path $root "web_build" }
if (-not $PackExe) { $PackExe = Join-Path $oxy "sonic3air\bin\Release_x64\Sonic3AIR.exe" }
$work = Join-Path $root "web_work"
if (-not (Test-Path (Join-Path $EmsdkDir "emsdk_env.ps1"))) { throw "Not an emsdk folder: $EmsdkDir" }
if (-not (Test-Path $PackExe)) { throw "Windows build for packing not found: $PackExe (build the Windows game first, or pass -PackExe)" }

# Clean start: nothing from an earlier build is reused
foreach ($d in @($work, $OutDir)) { if (Test-Path $d) { Remove-Item $d -Recurse -Force } }
New-Item -ItemType Directory -Force (Join-Path $work "cmake"), $OutDir | Out-Null

$env:EMSDK_QUIET = "1"
$ErrorActionPreference = "Continue"
. (Join-Path $EmsdkDir "emsdk_env.ps1") *> $null
$ErrorActionPreference = "Stop"
$sw = [Diagnostics.Stopwatch]::StartNew()

# 1. compile and link (wasm + js)
Push-Location (Join-Path $work "cmake")
emcmake cmake -G Ninja -DCMAKE_BUILD_TYPE=Release "-DWEB_OUTPUT_DIR=$OutDir" $cmakeSrc
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }
# The Windows clang / wasm-opt binaries of the SDK sometimes crash at random (out of memory / access violation) on this kind of machine: re-running ninja continues where it stopped
$ok = $false
for ($try = 1; $try -le 4 -and -not $ok; $try++) { ninja -j $Jobs; $ok = ($LASTEXITCODE -eq 0) }
if (-not $ok) { throw "web build failed (an 'out of memory' crash of clang can be retried with -Jobs 2)" }
Pop-Location

# 2. data packages, made by the engine's own packer from a scratch copy (so the source tree stays untouched)
$pack = Join-Path $work "pack"
$air  = Join-Path $pack "sonic3air"
New-Item -ItemType Directory -Force (Join-Path $air "saves"), (Join-Path $pack "oxygenengine") | Out-Null
robocopy (Join-Path $oxy "sonic3air\data") (Join-Path $air "data") /E /NFL /NDL /NJH /NJS /NP | Out-Null
robocopy (Join-Path $oxy "sonic3air\scripts") (Join-Path $air "scripts") /E /NFL /NDL /NJH /NJS /NP | Out-Null
robocopy (Join-Path $oxy "oxygenengine\data") (Join-Path $pack "oxygenengine\data") /E /NFL /NDL /NJH /NJS /NP | Out-Null
Copy-Item $PackExe $air
$dll = Join-Path (Split-Path $PackExe) "discord_game_sdk.dll"
if (Test-Path $dll) { Copy-Item $dll $air }
Copy-Item (Join-Path $oxy "sonic3air\_master_image_template\config.json") (Join-Path $air "config.json")
Push-Location $air
& ".\Sonic3AIR.exe" -pack
# compiled scripts: one start of the game writes saves\scripts.bin (it is stopped as soon as the file exists)
$p = Start-Process ".\Sonic3AIR.exe" -PassThru
$t = 0
while (-not (Test-Path "saves\scripts.bin") -and $t -lt 90) { Start-Sleep 1; $t++ }
Start-Sleep 2
Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
if (-not (Test-Path "saves\scripts.bin")) { throw "compiled scripts were not written" }
# the remastered music (audioremaster.bin, 130 MB) is NOT part of the web build
$packager = Join-Path $EmsdkDir "upstream\emscripten\tools\file_packager.py"
& $env:EMSDK_PYTHON $packager (Join-Path $OutDir "sonic3air.data") "--js-output=$(Join-Path $OutDir 'loader.js')" --preload `
	config.json@/sonic3air/config.json audiodata.bin@/sonic3air/data/audiodata.bin enginedata.bin@/sonic3air/data/enginedata.bin `
	gamedata.bin@/sonic3air/data/gamedata.bin data/metadata.json@/sonic3air/data/metadata.json data/font@/sonic3air/data/font `
	saves/scripts.bin@/sonic3air/data/scripts.bin --use-preload-cache
if ($LASTEXITCODE -ne 0) { throw "file_packager failed" }
Pop-Location

# 3. the page
Copy-Item (Join-Path $cmakeSrc "dist\*") $OutDir -Force
Remove-Item -Recurse -Force $work

# guard: the output must never contain a ROM
$rom = Get-ChildItem $OutDir -Recurse -File | Where-Object { $_.Length -eq 4194304 }
if ($rom) { throw "A 4 MB file (ROM?) is in the output: $($rom.FullName)" }
"Web build done in $([int]$sw.Elapsed.TotalSeconds) s -> $OutDir"
Get-ChildItem $OutDir -File | Select-Object Name, Length | Format-Table -AutoSize
