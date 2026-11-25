# Static guard for the hidden-tab rules of the browser build (see CLAUDE_HANDOFF.md, "Task 5"). A browser stops animation frames of a hidden tab, so a lockstep game stalls everybody.
# Files are keyed by their path relative to the online source tree, comments are removed before anything is looked at, and the preprocessor is followed properly (#if / #ifdef / #else / #elif / #endif, nested).
# Rules:
#   1. The game's main loop is driven by animation frames in exactly one file, librmx/source/rmxmedia/framework/SystemManager.cpp. Any other emscripten_set_main_loop, emscripten_request_animation_frame*
#      or requestAnimationFrame in a .cpp, .h, .c, .js or .html file (the web shell included) would stall in a hidden tab without the worker ticks that only know about the first one.
#      Nothing else may call the main loop either (a setInterval / setTimeout driver, a second caller of s3air_background_tick): SystemManager.cpp / .h and NetplayManager.cpp only
#   2. NetplayManager.cpp has, outside comments, the worker tick: the exported s3air_background_tick that calls mainLoop, the page glue (new Worker, visibilitychange, a call of _s3air_background_tick,
#      s3air_netplay_tab_changed), and the glue (EM_JS s3web_tab_glue_install) sits in a branch that is only compiled for PLATFORM_WEB
#   3. Game::shouldPauseOnFocusLoss returns false for a running online game (NetplayManager::isGameSessionRunning) in a branch that is only compiled for PLATFORM_WEB
#   4. Every use of PlayerAwayPacket in NetplayHost.cpp and NetplayClient.cpp sits in a branch that is only compiled for PLATFORM_WEB (an #else branch of it does not count)
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File checks\lint_hidden_tab_guard.ps1            scan, exit 1 on a problem
#   powershell -NoProfile -ExecutionPolicy Bypass -File checks\lint_hidden_tab_guard.ps1 -SelfTest  prove that each rule catches its bad forms (including the holes of an earlier version of this script)
param([switch]$SelfTest, [string]$Source)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if (-not $Source) { $Source = Join-Path $root "sonic3air_online_stable_source" }

$LoopFile = "librmx/source/rmxmedia/framework/SystemManager.cpp"
$ManagerFile = "Oxygen/oxygenengine/source/oxygen/network/netplay/NetplayManager.cpp"
$GameFile = "Oxygen/sonic3air/source/sonic3air/Game.cpp"
$LoopHeader = "librmx/source/rmxmedia/framework/SystemManager.h"
$MainLoopAllowed = @($LoopFile, $LoopHeader, $ManagerFile)
$AwayFiles = @("Oxygen/oxygenengine/source/oxygen/network/netplay/NetplayHost.cpp", "Oxygen/oxygenengine/source/oxygen/network/netplay/NetplayClient.cpp")

# Removes comments, keeps the line structure (so line numbers stay right) and leaves string contents alone (the JavaScript in EM_JS bodies is code that matters)
function Strip-Comments([string]$text)
{
	$sb = New-Object System.Text.StringBuilder
	$i = 0; $n = $text.Length; $inStr = $false; $quote = ''
	while ($i -lt $n) {
		$c = $text[$i]; $d = if ($i + 1 -lt $n) { $text[$i + 1] } else { [char]0 }
		if ($inStr) {
			[void]$sb.Append($c)
			if ($c -eq '\' -and $i + 1 -lt $n) { [void]$sb.Append($d); $i += 2; continue }
			if ($c -eq $quote -or $c -eq "`n") { $inStr = $false }
			$i++; continue
		}
		if ($c -eq '/' -and $d -eq '/') { while ($i -lt $n -and $text[$i] -ne "`n") { $i++ }; continue }
		if ($c -eq '/' -and $d -eq '*') { $i += 2; while ($i + 1 -lt $n -and -not ($text[$i] -eq '*' -and $text[$i + 1] -eq '/')) { if ($text[$i] -eq "`n") { [void]$sb.Append("`n") }; $i++ }; $i += 2; continue }
		if ($c -eq '"') { $inStr = $true; $quote = '"' }
		[void]$sb.Append($c); $i++
	}
	return $sb.ToString()
}

# For every line of the (comment free) text: is it compiled only for PLATFORM_WEB? Follows nested #if/#ifdef/#ifndef/#elif/#else/#endif.
# Each open #if is "web" (the active branch needs PLATFORM_WEB), "native" (the active branch excludes it) or "other" (anything else, which says nothing).
function Get-WebOnlyMap([string[]]$lines)
{
	$stack = New-Object System.Collections.Generic.List[string]
	$map = New-Object 'bool[]' $lines.Count
	for ($k = 0; $k -lt $lines.Count; $k++) {
		$t = $lines[$k].Trim()
		if ($t -match '^#\s*ifdef\s+PLATFORM_WEB\b' -or $t -match '^#\s*if\s+defined\s*\(?\s*PLATFORM_WEB\s*\)?\s*$') { $stack.Add("web") }
		elseif ($t -match '^#\s*ifndef\s+PLATFORM_WEB\b' -or $t -match '^#\s*if\s+!\s*defined\s*\(?\s*PLATFORM_WEB\s*\)?\s*$') { $stack.Add("native") }
		elseif ($t -match '^#\s*if') { $stack.Add("other") }
		elseif ($t -match '^#\s*else\b') { if ($stack.Count -gt 0) { $top = $stack[$stack.Count - 1]; $stack[$stack.Count - 1] = if ($top -eq "web") { "native" } elseif ($top -eq "native") { "web" } else { "other" } } }
		elseif ($t -match '^#\s*elif') { if ($stack.Count -gt 0) { $stack[$stack.Count - 1] = "other" } }   # unknown condition: counts for nothing
		elseif ($t -match '^#\s*endif\b') { if ($stack.Count -gt 0) { $stack.RemoveAt($stack.Count - 1) } }
		$map[$k] = $stack.Contains("web")
	}
	return $map
}

# $files: hashtable relative path -> raw text. Returns problem strings.
function Find-Problems($files)
{
	$problems = @()
	# (comments are only removed where it matters: files that mention an animation-frame call at all, and the key files; the rest cannot break a rule)
	$clean = @{}
	$key = @($LoopFile, $ManagerFile, $GameFile) + $AwayFiles
	foreach ($k in $files.Keys) { if ($key -contains $k -or $files[$k] -match 'emscripten_set_main_loop|emscripten_request_animation_frame|requestAnimationFrame|mainLoop\s*\(|s3air_background_tick') { $clean[$k] = Strip-Comments $files[$k] } }

	# 1
	foreach ($k in $clean.Keys) {
		if ($k -eq $LoopFile) { continue }
		if ($clean[$k] -match 'emscripten_set_main_loop|emscripten_request_animation_frame|requestAnimationFrame') { $problems += "rule 1: $k drives something from animation frames (only $LoopFile may; a hidden tab needs the worker ticks)" }
	}
	if (-not $clean.ContainsKey($LoopFile) -or $clean[$LoopFile] -notmatch 'emscripten_set_main_loop') { $problems += "rule 1: $LoopFile no longer starts the main loop where this guard expects it" }
	foreach ($k in $clean.Keys) {
		if ($MainLoopAllowed -contains $k) { continue }
		if ($clean[$k] -match 'mainLoop\s*\(|s3air_background_tick') { $problems += "rule 1: $k calls the main loop (only $LoopFile, $LoopHeader and $ManagerFile may; a second driver is not covered by the hidden-tab rules)" }
	}

	# 2
	if (-not $clean.ContainsKey($ManagerFile)) { $problems += "rule 2: $ManagerFile not found" }
	else {
		$nm = $clean[$ManagerFile]
		foreach ($needle in @('s3air_background_tick', 'visibilitychange', 'new Worker', 's3air_netplay_tab_changed', '_s3air_background_tick')) { if ($nm -notmatch [regex]::Escape($needle)) { $problems += "rule 2: $ManagerFile has no '$needle' outside comments" } }
		if ($nm -notmatch '(?s)extern\s+"C"\s+EMSCRIPTEN_KEEPALIVE\s+void\s+s3air_background_tick\s*\(\s*\)\s*\{[^}]*mainLoop\s*\(') { $problems += "rule 2: s3air_background_tick does not call mainLoop" }
		$lines = $nm -split "`n"; $map = Get-WebOnlyMap $lines
		for ($k = 0; $k -lt $lines.Count; $k++) {
			if ($lines[$k] -match 'EM_JS\s*\(\s*void\s*,\s*s3web_tab_glue_install' -and -not $map[$k]) { $problems += "rule 2: the tab glue (line $($k + 1)) is not only compiled for PLATFORM_WEB" }
			if ($lines[$k] -match 'extern\s+"C"\s+EMSCRIPTEN_KEEPALIVE' -and -not $map[$k]) { $problems += "rule 2: an exported tab function (line $($k + 1)) is not only compiled for PLATFORM_WEB" }
		}
	}

	# 3
	if (-not $clean.ContainsKey($GameFile)) { $problems += "rule 3: $GameFile not found" }
	else {
		$lines = $clean[$GameFile] -split "`n"; $map = Get-WebOnlyMap $lines
		$start = -1
		for ($k = 0; $k -lt $lines.Count; $k++) { if ($lines[$k] -match 'bool\s+Game::shouldPauseOnFocusLoss\s*\(\s*\)\s*const') { $start = $k; break } }
		if ($start -lt 0) { $problems += "rule 3: Game::shouldPauseOnFocusLoss not found" }
		else {
			$ok = $false
			for ($k = $start; $k -lt [Math]::Min($lines.Count, $start + 14); $k++) {
				if ($lines[$k] -match 'isGameSessionRunning\s*\(\s*\)' -and $map[$k]) {
					for ($j = $k; $j -lt [Math]::Min($lines.Count, $k + 4); $j++) { if ($lines[$j] -match '\breturn\s+false\s*;' -and $map[$j]) { $ok = $true } }
				}
			}
			if (-not $ok) { $problems += "rule 3: Game::shouldPauseOnFocusLoss does not return false for a running online game inside a PLATFORM_WEB-only branch" }
		}
	}

	# 4
	foreach ($name in $AwayFiles) {
		if (-not $clean.ContainsKey($name)) { $problems += "rule 4: $name not found"; continue }
		$lines = $clean[$name] -split "`n"; $map = Get-WebOnlyMap $lines
		for ($k = 0; $k -lt $lines.Count; $k++) { if ($lines[$k] -match 'PlayerAwayPacket' -and -not $map[$k]) { $problems += "rule 4: $name line $($k + 1) uses PlayerAwayPacket outside a PLATFORM_WEB-only branch" } }
	}
	return $problems
}

function Load-Files([string]$src)
{
	$files = @{}
	$src = (Resolve-Path -LiteralPath $src).Path
	# (the extension filter is explicit: Windows PowerShell 5.1 ignores -Include together with -LiteralPath and would read every file, pictures included)
	$extensions = @('.cpp', '.h', '.c', '.js', '.html')
	foreach ($f in Get-ChildItem -LiteralPath $src -Recurse -File | Where-Object { $extensions -contains $_.Extension.ToLowerInvariant() -and $_.FullName -notmatch '\\external\\|\\build\\_(android|vstudio)|\\bin\\|\\obj\\|\\lib\\' }) {
		$rel = $f.FullName.Substring($src.Length + 1).Replace('\', '/')
		$files[$rel] = [IO.File]::ReadAllText($f.FullName)
	}
	return $files
}

if ($SelfTest) {
	$good = Load-Files $Source
	$real = Find-Problems $good
	if ($real.Count -ne 0) { Write-Host "FAIL self test: the real sources already have problems: $($real -join '; ')"; exit 1 }
	$web = "Oxygen/sonic3air/build/_emscripten/dist/sonic3air_web.html"
	# the scan reads source files only (a pictures-and-data scan would be slow and a sign that the file filter is broken), and it does include the web shell and the key files
	$stray = @($good.Keys | Where-Object { $_ -notmatch '\.(cpp|h|c|js|html)$' })
	if ($stray.Count -ne 0) { Write-Host "FAIL self test: the scan read $($stray.Count) files that are not source files, e.g. $($stray[0])"; exit 1 }
	foreach ($must in @($web, $LoopFile, $LoopHeader, $ManagerFile, $GameFile) + $AwayFiles) { if (-not $good.ContainsKey($must)) { Write-Host "FAIL self test: the scan did not load $must"; exit 1 } }
	$extraJs ="Oxygen/sonic3air/build/_emscripten/dist/extra.js"
	# Every bad form names the rule that has to catch it (a form caught by some other rule would not prove anything about its own rule)
	$cases = @(
		@{ rule = 1; name = "second animation-frame loop in a .cpp"; mutate = { param($f) $f["Other/Loop.cpp"] = "void x() { emscripten_set_main_loop(a, 0, 1); }" } },
		@{ rule = 1; name = "requestAnimationFrame in the web shell"; mutate = { param($f) $f[$web] = $f[$web] + "<script>function t(){ requestAnimationFrame(t); }</script>" } },
		@{ rule = 1; name = "requestAnimationFrame in a .js file"; mutate = { param($f) $f[$extraJs] = "requestAnimationFrame(function(){});" } },
		@{ rule = 1; name = "emscripten_request_animation_frame_loop in a header"; mutate = { param($f) $f["Oxygen/oxygenengine/source/oxygen/Loop.h"] = "inline void y() { emscripten_request_animation_frame_loop(cb, 0); }" } },
		@{ rule = 1; name = "a second animation-frame loop in a file with the same name as the real one, in another folder (files are keyed by path)"; mutate = { param($f) $f["Other/SystemManager.cpp"] = "void q() { emscripten_set_main_loop(a, 0, 1); }" } },
		@{ rule = 1; name = "the animation-frame loop only in a comment of the expected file (no loop left)"; mutate = { param($f) $f[$LoopFile] = "// emscripten_set_main_loop`n" + $f[$LoopFile].Replace("emscripten_set_main_loop", "emscripten_other") } },
		@{ rule = 1; name = "a setInterval driver of the main loop in a .js file"; mutate = { param($f) $f[$extraJs] = "setInterval(function(){ Module._s3air_background_tick(); }, 16);" } },
		@{ rule = 1; name = "a second caller of the main loop in a header"; mutate = { param($f) $f["Oxygen/oxygenengine/source/oxygen/Driver.h"] = "inline void d() { FTX::System->mainLoop(); }" } },
		@{ rule = 2; name = "worker tick removed"; mutate = { param($f) $f[$ManagerFile] = $f[$ManagerFile].Replace("s3air_background_tick", "s3air_other") } },
		@{ rule = 2; name = "visibilitychange only named in a comment"; mutate = { param($f) $m = $f[$ManagerFile]; $f[$ManagerFile] = $m.Replace("visibilitychange", "visibilitychangX") + "`n// visibilitychange" } },
		@{ rule = 2; name = "new Worker only named in a block comment"; mutate = { param($f) $m = $f[$ManagerFile]; $f[$ManagerFile] = $m.Replace("new Worker", "new Wrkr") + "`n/* new Worker */" } },
		@{ rule = 2; name = "tick function no longer calls mainLoop"; mutate = { param($f) $f[$ManagerFile] = $f[$ManagerFile].Replace("FTX::System->mainLoop()", "FTX::System->other()") } },
		@{ rule = 2; name = "tab glue in a branch for the other platforms (#ifndef)"; mutate = { param($f) $f[$ManagerFile] = $f[$ManagerFile].Replace("#ifdef PLATFORM_WEB`n// ---- Browser build: the tab", "#ifndef PLATFORM_WEB`n// ---- Browser build: the tab").Replace("#ifdef PLATFORM_WEB`r`n// ---- Browser build: the tab", "#ifndef PLATFORM_WEB`r`n// ---- Browser build: the tab") } },
		@{ rule = 2; name = "an exported tab function in the #else branch of PLATFORM_WEB"; mutate = { param($f) $f[$ManagerFile] = $f[$ManagerFile] + "`n#ifdef PLATFORM_WEB`n#else`nextern `"C`" EMSCRIPTEN_KEEPALIVE void s3air_other() {}`n#endif`n" } },
		@{ rule = 3; name = "focus pause not exempted"; mutate = { param($f) $f[$GameFile] = $f[$GameFile].Replace("isGameSessionRunning()", "isSomethingElse()") } },
		@{ rule = 3; name = "focus pause exemption only in a comment"; mutate = { param($f) $f[$GameFile] = $f[$GameFile].Replace("isGameSessionRunning()", "isSomethingElse() /* isGameSessionRunning() */") } },
		@{ rule = 3; name = "focus pause exemption in the branch for the other platforms (#ifndef)"; mutate = { param($f) $f[$GameFile] = $f[$GameFile].Replace("#ifdef PLATFORM_WEB`r`n	// An online game cannot", "#ifndef PLATFORM_WEB`r`n	// An online game cannot").Replace("#ifdef PLATFORM_WEB`n	// An online game cannot", "#ifndef PLATFORM_WEB`n	// An online game cannot") } },
		@{ rule = 3; name = "a file with the same name as the real one in another folder does not stand in for it"; mutate = { param($f) $f["Other/Game.cpp"] = "// harmless"; $f[$GameFile] = $f[$GameFile].Replace("isGameSessionRunning()", "isSomethingElse()") } },
		@{ rule = 4; name = "away packet handled for every platform (#if 1)"; mutate = { param($f) $f[$AwayFiles[0]] = $f[$AwayFiles[0]].Replace("#ifdef PLATFORM_WEB", "#if 1") } },
		@{ rule = 4; name = "away packet in the #else branch of PLATFORM_WEB"; mutate = { param($f) $f[$AwayFiles[1]] = $f[$AwayFiles[1]] + "`n#ifdef PLATFORM_WEB`n#else`nvoid z() { PlayerAwayPacket p; }`n#endif`n" } },
		@{ rule = 4; name = "away packet in an #elif branch of PLATFORM_WEB"; mutate = { param($f) $f[$AwayFiles[1]] = $f[$AwayFiles[1]] + "`n#ifdef PLATFORM_WEB`n#elif defined(OTHER)`nvoid y() { PlayerAwayPacket p; }`n#endif`n" } },
		@{ rule = 4; name = "away packet right after a closed web branch and an unrelated #if/#endif pair (the pairs must not be counted against each other)"; mutate = { param($f) $f[$AwayFiles[1]] = $f[$AwayFiles[1]] + "`n#ifdef PLATFORM_WEB`n#endif`n#if 1`n#endif`nvoid w() { PlayerAwayPacket q; }`n" } },
		@{ rule = 4; name = "away packet in a native-only branch (#ifndef)"; mutate = { param($f) $f[$AwayFiles[0]] = $f[$AwayFiles[0]] + "`n#ifndef PLATFORM_WEB`nvoid v() { PlayerAwayPacket p; }`n#endif`n" } },
		@{ rule = 4; name = "away packet used by another path in the second file (any use counts, not only PlayerAwayPacket::PACKET_TYPE)"; mutate = { param($f) $f[$AwayFiles[1]] = $f[$AwayFiles[1]] + "`nvoid u() { PlayerAwayPacket p; p.mAway = 1; }`n" } }
	)
	$bad = 0
	foreach ($c in $cases) {
		$copy = @{}; foreach ($k in $good.Keys) { $copy[$k] = $good[$k] }
		& $c.mutate $copy
		$changed = ($copy.Count -ne $good.Count); foreach ($k in $good.Keys) { if ($copy[$k] -ne $good[$k]) { $changed = $true } }
		if (-not $changed) { Write-Host "FAIL self test: '$($c.name)' did not change anything (the text it looks for is gone from the real source?)"; $bad++; continue }
		$found = @(Find-Problems $copy)
		if ($found.Count -eq 0) { Write-Host "FAIL self test: '$($c.name)' was not caught"; $bad++ }
		elseif (@($found | Where-Object { $_ -like "rule $($c.rule):*" }).Count -eq 0) { Write-Host "FAIL self test: '$($c.name)' was caught, but not by rule $($c.rule): $($found -join '; ')"; $bad++ }
	}
	# good forms must stay accepted
	$goodForms = @(
		@{ name = "an unrelated nested #if inside a PLATFORM_WEB branch"; mutate = { param($f) $f[$AwayFiles[1]] = $f[$AwayFiles[1]] + "`n#ifdef PLATFORM_WEB`n#if 1`nvoid ok() { PlayerAwayPacket p; }`n#endif`n#endif`n" } },
		@{ name = "the #else branch of #ifndef PLATFORM_WEB"; mutate = { param($f) $f[$AwayFiles[1]] = $f[$AwayFiles[1]] + "`n#ifndef PLATFORM_WEB`n#else`nvoid ok() { PlayerAwayPacket p; }`n#endif`n" } },
		@{ name = "words in comments: PlayerAwayPacket outside the branch, requestAnimationFrame, mainLoop() and s3air_background_tick in other files"; mutate = { param($f) $f[$AwayFiles[1]] = $f[$AwayFiles[1]] + "`n// PlayerAwayPacket`n/* PlayerAwayPacket */`n"; $f["Other/Note.cpp"] = "// requestAnimationFrame, emscripten_set_main_loop, mainLoop(), s3air_background_tick`n/* mainLoop() */`nvoid n() {}`n"; $f[$extraJs] = "// requestAnimationFrame mainLoop() _s3air_background_tick`n" } }
	)
	foreach ($g in $goodForms) {
		$copy = @{}; foreach ($k in $good.Keys) { $copy[$k] = $good[$k] }
		& $g.mutate $copy
		$noise = @(Find-Problems $copy)
		if ($noise.Count -ne 0) { Write-Host "FAIL self test: a good form was refused ($($g.name)): $($noise -join '; ')"; $bad++ }
	}
	if ($bad -gt 0) { exit 1 }
	Write-Host "PASS lint_hidden_tab_guard self test ($($cases.Count) bad forms caught by the rule they belong to, $($goodForms.Count) good forms stay accepted)"
	exit 0
}

$files = Load-Files $Source
$problems = Find-Problems $files
if ($problems.Count -gt 0) { $problems | ForEach-Object { Write-Host "FAIL $_" }; exit 1 }
Write-Host "PASS lint_hidden_tab_guard: $($files.Count) source files scanned, one animation-frame loop, worker ticks and tab glue behind PLATFORM_WEB, online game not paused by focus loss, PlayerAwayPacket behind PLATFORM_WEB"
