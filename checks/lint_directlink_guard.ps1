# Static guard: the browser-only direct (WebRTC) connection code must not leak into the Windows / Android builds.
# Rules (all in oxygen_netcore / oxygen / sonic3air sources of the online source tree):
#   1. In DirectLink.h, everything that needs emscripten (the emscripten.h include, EM_JS, EM_ASM, calls of s3p2p_*) sits inside an #ifdef __EMSCRIPTEN__ block
#   2. s3p2p_* and RTCPeerConnection appear in no other C++ file
#   3. DirectLink.h is included only by oxygen_netcore/network/Sockets.cpp
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File checks\lint_directlink_guard.ps1            scan, exit 1 on a problem
#   powershell -NoProfile -ExecutionPolicy Bypass -File checks\lint_directlink_guard.ps1 -SelfTest  prove the scanner catches the bad forms
param([switch]$SelfTest, [string]$Source)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if (-not $Source) { $Source = Join-Path $root "sonic3air_online_stable_source\Oxygen" }

# Returns problem strings for the lines of DirectLink.h (rule 1)
function Find-UnguardedWebCode([string[]]$lines)
{
	$problems = @()
	$stack = New-Object System.Collections.Generic.List[bool]   # per open #if: true when it is an __EMSCRIPTEN__ block (or inside one)
	$n = 0
	foreach ($line in $lines)
	{
		$n++
		$t = $line.Trim()
		if ($t -match '^#\s*if(def|ndef)?\b')
		{
			$inWeb = ($stack.Count -gt 0 -and $stack[$stack.Count - 1]) -or ($t -match '^#\s*ifdef\s+__EMSCRIPTEN__\b') -or ($t -match '^#\s*if\s+defined\(__EMSCRIPTEN__\)')
			$stack.Add($inWeb); continue
		}
		if ($t -match '^#\s*else\b')
		{
			# the #else branch of an __EMSCRIPTEN__ block is the portable one
			if ($stack.Count -gt 0) { $outer = $stack.Count -gt 1 -and $stack[$stack.Count - 2]; $stack[$stack.Count - 1] = $outer }
			continue
		}
		if ($t -match '^#\s*endif\b') { if ($stack.Count -gt 0) { $stack.RemoveAt($stack.Count - 1) }; continue }
		$guarded = ($stack.Count -gt 0 -and $stack[$stack.Count - 1])
		if ($guarded) { continue }
		if ($t -match '^//') { continue }
		if ($t -match '#\s*include\s*<emscripten' -or $t -match '\bEM_JS\b' -or $t -match '\bEM_ASM' -or $t -match '\bs3p2p_\w+\s*\(')
		{
			$problems += ("DirectLink.h:" + $n + ": emscripten code outside #ifdef __EMSCRIPTEN__: " + $t)
		}
	}
	return $problems
}

# Returns problem strings for one other source file (rules 2 and 3); $name is the file name, $text its content
function Find-OutsideUse([string]$name, [string]$text)
{
	$problems = @()
	if ($text -match '\bs3p2p_\w+' -or $text -match 'RTCPeerConnection') { $problems += ($name + ": uses the WebRTC bridge outside DirectLink.h") }
	if ($text -match '#\s*include\s*[<"][^>"]*DirectLink\.h[>"]' -and $name -ne "Sockets.cpp") { $problems += ($name + ": includes DirectLink.h (only Sockets.cpp may)") }
	return $problems
}

if ($SelfTest)
{
	$bad = @('#include <emscripten.h>', 'EM_JS(int, s3p2p_x, (), {});', 'int a = s3p2p_state(0);')
	$good = @('#ifdef __EMSCRIPTEN__', '#include <emscripten.h>', 'EM_JS(int, s3p2p_x, (), {});', '#else', 'int a = 0;', '#endif')
	$ok = ((Find-UnguardedWebCode $bad).Count -eq 3) -and ((Find-UnguardedWebCode $good).Count -eq 0)
	# rule 2: the bridge or RTCPeerConnection in another file
	$ok = $ok -and ((Find-OutsideUse "Other.cpp" 'int a = s3p2p_state(0);').Count -eq 1)
	$ok = $ok -and ((Find-OutsideUse "Other.cpp" 'auto pc = new RTCPeerConnection();').Count -eq 1)
	$ok = $ok -and ((Find-OutsideUse "Other.cpp" 'int a = state(0); // nothing special').Count -eq 0)
	# rule 3: only Sockets.cpp may include DirectLink.h
	$ok = $ok -and ((Find-OutsideUse "NetplayManager.cpp" '#include "oxygen_netcore/network/DirectLink.h"').Count -eq 1)
	$ok = $ok -and ((Find-OutsideUse "Sockets.cpp" '#include "oxygen_netcore/network/DirectLink.h"').Count -eq 0)
	if ($ok) { Write-Host "PASS lint_directlink_guard self test"; exit 0 }
	Write-Host "FAIL lint_directlink_guard self test" -ForegroundColor Red; exit 1
}

$dl = Join-Path $Source "oxygenengine\source\oxygen_netcore\network\DirectLink.h"
if (-not (Test-Path $dl)) { Write-Host "FAIL lint_directlink_guard: $dl not found" -ForegroundColor Red; exit 1 }
$problems = @(Find-UnguardedWebCode (Get-Content $dl))

$files = Get-ChildItem -Path $Source -Recurse -Include *.cpp,*.h,*.hpp,*.mm -File | Where-Object { $_.FullName -notmatch '[\\/](third_party|thirdparty|external|_emscripten|build)[\\/]' }
foreach ($f in $files)
{
	if ($f.FullName -eq (Resolve-Path $dl).Path) { continue }
	$problems += @(Find-OutsideUse $f.Name (Get-Content $f.FullName -Raw))
}

if ($problems.Count -gt 0) { $problems | ForEach-Object { Write-Host "FAIL $_" -ForegroundColor Red }; exit 1 }
Write-Host ("PASS lint_directlink_guard: " + $files.Count + " source files scanned, WebRTC code only behind __EMSCRIPTEN__ in DirectLink.h, included only by Sockets.cpp")
