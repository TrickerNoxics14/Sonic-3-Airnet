# Static guard for the hidden-tab rules of the browser build (see CLAUDE_HANDOFF.md, "Task 5"). A browser stops animation frames of a hidden tab, so a lockstep game stalls everybody.
# Rules (sources of the online source tree):
#   1. The game's main loop is driven by animation frames in exactly one place (emscripten_set_main_loop in rmxmedia SystemManager.cpp); a second animation-frame loop would stall in a hidden tab
#      without the worker ticks that only know about the first one
#   2. NetplayManager.cpp has the worker tick (s3air_background_tick, visibilitychange, a Worker) and the exported function the page calls, both behind PLATFORM_WEB
#   3. Game::shouldPauseOnFocusLoss does not pause an online game in the browser build (it asks NetplayManager::isGameSessionRunning inside PLATFORM_WEB): a pause by one player stops everybody
#   4. The PlayerAwayPacket is handled only behind PLATFORM_WEB in NetplayHost.cpp / NetplayClient.cpp (Windows and Android behave as before)
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File checks\lint_hidden_tab_guard.ps1            scan, exit 1 on a problem
#   powershell -NoProfile -ExecutionPolicy Bypass -File checks\lint_hidden_tab_guard.ps1 -SelfTest  prove that each rule catches its bad form
param([switch]$SelfTest, [string]$Source)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if (-not $Source) { $Source = Join-Path $root "sonic3air_online_stable_source" }

function Read-Text([string]$path) { if (-not (Test-Path -LiteralPath $path)) { return $null }; return [IO.File]::ReadAllText($path) }

# $files: hashtable name -> text. Returns problem strings.
function Find-Problems($files)
{
	$problems = @()
	# 1
	$loops = @($files.Keys | Where-Object { $_ -like "src:*" -and $files[$_] -match 'emscripten_set_main_loop' -and $_ -ne "src:SystemManager.cpp" })
	foreach ($l in $loops) { $problems += "rule 1: $l starts an animation-frame main loop (only SystemManager.cpp may; hidden tabs need the worker ticks)" }
	if ($files["src:SystemManager.cpp"] -notmatch 'emscripten_set_main_loop') { $problems += "rule 1: SystemManager.cpp no longer starts the main loop where this guard expects it" }
	# 2
	$nm = $files["src:NetplayManager.cpp"]
	foreach ($needle in @('s3air_background_tick', 'visibilitychange', 'new Worker', 's3air_netplay_tab_changed')) { if ($nm -notmatch [regex]::Escape($needle)) { $problems += "rule 2: NetplayManager.cpp lacks '$needle'" } }
	if ($nm -notmatch '(?s)#ifdef PLATFORM_WEB\s*//[^\n]*\n(//[^\n]*\n)*EM_JS\(void, s3web_tab_glue_install') { $problems += "rule 2: the tab glue is not directly behind #ifdef PLATFORM_WEB" }
	# 3
	$g = $files["src:Game.cpp"]
	if ($g -notmatch '(?s)bool Game::shouldPauseOnFocusLoss\(\) const\s*\{\s*#ifdef PLATFORM_WEB[^}]*isGameSessionRunning\(\)[^}]*return false;[^}]*#endif') { $problems += "rule 3: Game::shouldPauseOnFocusLoss does not exempt a running online game in the browser build" }
	# 4
	foreach ($name in @("src:NetplayHost.cpp", "src:NetplayClient.cpp")) {
		$t = $files[$name]
		$m = [regex]::Matches($t, 'PlayerAwayPacket::PACKET_TYPE')
		foreach ($x in $m) {
			$before = $t.Substring(0, $x.Index)
			$open = ([regex]::Matches($before, '(?m)^\s*#\s*ifdef\s+PLATFORM_WEB')).Count - ([regex]::Matches($before, '(?m)^\s*#\s*endif')).Count
			if ($open -le 0) { $problems += "rule 4: $name handles PlayerAwayPacket outside #ifdef PLATFORM_WEB" }
		}
	}
	return $problems
}

function Load-Files([string]$src)
{
	$files = @{}
	foreach ($f in Get-ChildItem -LiteralPath $src -Recurse -Include *.cpp -File | Where-Object { $_.FullName -notmatch '\\external\\|\\build\\_(android|emscripten)' }) { $files["src:" + $f.Name] = [IO.File]::ReadAllText($f.FullName) }
	return $files
}

if ($SelfTest) {
	$good = Load-Files $Source
	if ((Find-Problems $good).Count -ne 0) { Write-Host "FAIL self test: the real sources already have problems: $((Find-Problems $good) -join '; ')"; exit 1 }
	$cases = @(
		@{ name = "second animation-frame loop"; mutate = { param($f) $f["src:Other.cpp"] = "void x() { emscripten_set_main_loop(a, 0, 1); }" } },
		@{ name = "worker tick removed"; mutate = { param($f) $f["src:NetplayManager.cpp"] = $f["src:NetplayManager.cpp"].Replace("s3air_background_tick", "s3air_other") } },
		@{ name = "focus pause not exempted"; mutate = { param($f) $f["src:Game.cpp"] = $f["src:Game.cpp"].Replace("isGameSessionRunning()", "isSomethingElse()") } },
		@{ name = "away packet handled for every platform"; mutate = { param($f) $f["src:NetplayHost.cpp"] = $f["src:NetplayHost.cpp"].Replace("#ifdef PLATFORM_WEB", "#if 1") } }
	)
	$bad = 0
	foreach ($c in $cases) {
		$copy = @{}; foreach ($k in $good.Keys) { $copy[$k] = $good[$k] }
		& $c.mutate $copy
		if ((Find-Problems $copy).Count -eq 0) { Write-Host "FAIL self test: '$($c.name)' was not caught"; $bad++ }
	}
	if ($bad -gt 0) { exit 1 }
	Write-Host "PASS lint_hidden_tab_guard self test ($($cases.Count) bad forms caught)"
	exit 0
}

$files = Load-Files $Source
$problems = Find-Problems $files
if ($problems.Count -gt 0) { $problems | ForEach-Object { Write-Host "FAIL $_" }; exit 1 }
Write-Host "PASS lint_hidden_tab_guard: one animation-frame loop, worker ticks and tab glue behind PLATFORM_WEB, online game not paused by focus loss, PlayerAwayPacket behind PLATFORM_WEB"
