$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$game = Join-Path $PSScriptRoot 'stock-stable/game/sonic3air_game'
$save = Join-Path $game 'savedata'
$importedRom = Join-Path $save 'Sonic_Knuckles_wSonic3.bin'
trap {
 if (Test-Path -LiteralPath $importedRom) { Remove-Item -LiteralPath $importedRom -Force }
 throw $_
}
$mods = Join-Path $save 'mods'
New-Item -ItemType Directory -Force $mods | Out-Null
Expand-Archive -LiteralPath (Join-Path $root 'DropInCoop_Local.zip') -DestinationPath $mods -Force
$rom = Join-Path $env:APPDATA 'Sonic3AIR/Sonic_Knuckles_wSonic3.bin'
$config = @{ StartPhase='3'; RomPath=$rom; WindowSize='960 x 540'; DevMode=@{Enabled=$true; EnforceDebugMode='0'} }
$config | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $game 'config.json') -Encoding utf8
$harness = Join-Path $mods 'PartyHarness'
New-Item -ItemType Directory -Force (Join-Path $harness 'scripts') | Out-Null
Copy-Item (Join-Path $PSScriptRoot 'harness/PartyHarness/mod.json') $harness -Force
'{"ActiveMods":["DropInCoop","PartyHarness"],"UseLegacyLoading":false}' | Set-Content (Join-Path $mods 'active-mods.json') -Encoding utf8
$reporter = @'
function void Checks.report(string name, u8 passed, string detail)
{
 u8[0x8fc000] = passed
 debugDumpToFile(stringformat("stock-%s.bin", name), 0x8fc000, 1)
 if (name == "party damage")
 {
  u16[0x8fc010] = ring_counter
  u16[0x8fc012] = ring_counter.player2
  u16[0x8fc014] = dropin.rings3
  u16[0x8fc016] = dropin.rings4
  debugDumpToFile("stock-damage-detail.bin", 0x8fc010, 8)
 }
}
'@
$results = @()
foreach ($count in @(3,4)) {
 $output = Join-Path $save 'output'
 if (Test-Path $output) { Get-ChildItem -LiteralPath $output -Filter 'stock-*.bin' | Remove-Item -Force }
 $text = (Get-Content (Join-Path $PSScriptRoot 'harness/PartyHarness/scripts/main.lemon') -Raw).Replace('__PLAYER_COUNT__', [string]$count)
 # Stock has no framebuffer capture binding. Keep that check explicitly outside this run.
 $text = [regex]::Replace($text, '(?m)^.*Checks.report\("party picture".*\r?\n', '')
 ($text + "`n" + $reporter) | Set-Content (Join-Path $harness 'scripts/main.lemon') -Encoding utf8
 $process = Start-Process (Join-Path $game 'Sonic3AIR.exe') -WorkingDirectory $game -PassThru
 try {
  $deadline = (Get-Date).AddSeconds(75)
  while ((Get-Date) -lt $deadline -and -not $process.HasExited -and -not (Test-Path (Join-Path $output 'stock-run complete.bin'))) { Start-Sleep -Milliseconds 500 }
 } finally { if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force }; Start-Sleep -Milliseconds 500 }
 $dest = Join-Path $PSScriptRoot "last_logs/stock$count"
 New-Item -ItemType Directory -Force $dest | Out-Null
 Copy-Item (Join-Path $save 'logfile.txt') $dest -Force
 if (Test-Path (Join-Path $output 'stock-damage-detail.bin')) { Copy-Item (Join-Path $output 'stock-damage-detail.bin') $dest -Force }
 foreach ($name in @('party actors','party control3','party control4','party jumping','party camera','party alive','party platforms','party score','party damage','party respawn','run complete')) {
  $file = Join-Path $output "stock-$name.bin"
  $ok = (Test-Path $file) -and [IO.File]::ReadAllBytes($file)[0] -eq 1
  $line = "$(if($ok){'PASS'}else{'FAIL'}) stock$count $name"
  $results += $line
  Write-Host $line
 }
}
$results | Set-Content (Join-Path $PSScriptRoot 'stock_stable_report.txt') -Encoding utf8
if (Test-Path -LiteralPath $importedRom) { Remove-Item -LiteralPath $importedRom -Force }
if (@($results | Where-Object {$_ -like 'FAIL*'}).Count) { exit 1 }
