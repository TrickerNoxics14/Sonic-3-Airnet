# The local (offline) game stages on the STABLE engine fork (official 26.03.28 + netplay), the engine the release is built from.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File checks\run_local_stable_checks.ps1                   all local stages, view size Medium
#   ... -Only G,Z -ViewSize 2                                                                                  only the act scenes and the zip line, view size Large
#
# Stages: P (three/four players), Q (four-view split screen), F (two players, scripts), H (bosses), G (split screen in every act),
# QG (four-view split screen in every act, each view compared with the game's own picture of that camera), Z (zip line in the second view).
# Every stage that starts the game needs the engine build (checks\build_online_stable.ps1); nothing here builds or packages anything.
# The result is written to checks\local_stable_report_view<ViewSize>.txt (and, as always, to checks\online_stable_runtime_report.txt).
param(
	[string[]]$Only = @('P', 'Q', 'F', 'H', 'G', 'QG', 'Z'),
	[int]$ViewSize = 1			# 0 = Small, 1 = Medium, 2 = Large (Split Screen View Size)
)

$ErrorActionPreference = 'Stop'
$checksDir = $PSScriptRoot
$stableRoot = Join-Path (Split-Path -Parent $checksDir) 'sonic3air_online_stable_source'
$runner = Join-Path $checksDir 'run_checks.ps1'
$onlyList = @($Only | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })

& powershell -NoProfile -ExecutionPolicy Bypass -File $runner -RuntimeOnly -EngineRoot $stableRoot -Only ($onlyList -join ',') -ViewSize $ViewSize
$exitCode = $LASTEXITCODE

$report = Join-Path $checksDir 'online_stable_runtime_report.txt'
if (Test-Path -LiteralPath $report) {
	Copy-Item -LiteralPath $report -Destination (Join-Path $checksDir ('local_stable_report_view' + $ViewSize + '.txt')) -Force
}
exit $exitCode
