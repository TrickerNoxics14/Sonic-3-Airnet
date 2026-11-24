# Static guard: a button that opens a folder must work on every platform that shows it (the browser build had a right-click menu whose "Open mods folder" did nothing,
# because PlatformFunctions::openDirectoryExternal only does something on Windows, Mac and Linux).
# Rules (sources of the online source tree):
#   1. PlatformFunctions::openDirectoryExternal( is called only from the files listed in $allowed (a new caller must be added there on purpose, after checking where it runs)
#   2. ImGuiFileBrowser.cpp calls it only inside #if defined(OPEN_EXTERNAL_TEXT) (the platforms that have a file manager)
#   3. ModsStartPage.cpp adds its "Open mods folder" entry only inside an #if for Windows / Mac / Linux, never for the web, Android or iOS
#   4. ApplicationContextMenu.cpp calls it only in the EXTERNAL case of chooseDirectoryOpenAction (the other platforms get the game's own file browser or no menu),
#      and checks isDirectoryOpenAllowedNow (the game's own file browser must not open over a running level); its "Open log file" entry (openFileExternal) stays inside #if 0
#   5. PlatformFunctions.cpp: canOpenDirectoryExternal() names exactly the platforms that openDirectoryExternal() has a branch for
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File checks\lint_open_folder_guard.ps1            scan, exit 1 on a problem
#   powershell -NoProfile -ExecutionPolicy Bypass -File checks\lint_open_folder_guard.ps1 -SelfTest  prove the scanner catches the bad forms
param([switch]$SelfTest, [string]$Source)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if (-not $Source) { $Source = Join-Path $root "sonic3air_online_stable_source\Oxygen" }
$allowed = @("ImGuiFileBrowser.cpp", "ModsStartPage.cpp", "ApplicationContextMenu.cpp")

# Rules 2 and 3: every line that matches $callRegex must sit inside an #if branch whose condition matches $guardRegex (and none that matches $forbidRegex)
function Find-UnguardedLines([string]$label, [string]$text, [string]$callRegex, [string]$guardRegex, [string]$forbidRegex)
{
	$problems = @()
	$stack = New-Object System.Collections.Generic.List[string]
	$n = 0
	foreach ($line in ($text -split '\r?\n'))
	{
		$n++
		$t = $line.Trim()
		if ($t -match '^#\s*ifndef\b') { $stack.Add(''); continue }
		if ($t -match '^#\s*if(def)?\b(.*)$') { $stack.Add($Matches[2]); continue }
		if ($t -match '^#\s*elif\b(.*)$') { if ($stack.Count -gt 0) { $stack[$stack.Count - 1] = $Matches[1] }; continue }
		if ($t -match '^#\s*else\b') { if ($stack.Count -gt 0) { $stack[$stack.Count - 1] = '' }; continue }
		if ($t -match '^#\s*endif\b') { if ($stack.Count -gt 0) { $stack.RemoveAt($stack.Count - 1) }; continue }
		if ($t -match '^//') { continue }
		if ($t -notmatch $callRegex) { continue }
		$guarded = $false; $forbidden = $false
		foreach ($cond in $stack) { if ($cond -match $guardRegex) { $guarded = $true }; if ($forbidRegex -and $cond -match $forbidRegex) { $forbidden = $true } }
		if (-not $guarded -or $forbidden) { $problems += ($label + ":" + $n + ": not inside a platform guard for " + $guardRegex + ": " + $t) }
	}
	return $problems
}

# Rule 4: the call sits right behind "case DirectoryOpenAction::EXTERNAL"
function Find-MenuCallOutsideExternalCase([string]$text)
{
	$problems = @()
	if ($text -notmatch 'chooseDirectoryOpenAction\(') { $problems += "ApplicationContextMenu.cpp: does not decide with chooseDirectoryOpenAction" }
	if ($text -notmatch 'isDirectoryOpenAllowedNow\(') { $problems += "ApplicationContextMenu.cpp: does not check isDirectoryOpenAllowedNow (the game's own file browser must not open over a running level)" }
	$lines = $text -split '\r?\n'
	for ($i = 0; $i -lt $lines.Count; ++$i)
	{
		if ($lines[$i].Trim() -match '^//' -or $lines[$i] -notmatch '\bopenDirectoryExternal\(') { continue }
		$from = [Math]::Max(0, $i - 3)
		$near = ($lines[$from..$i] -join "`n")
		if ($near -notmatch 'case\s+DirectoryOpenAction::EXTERNAL') { $problems += ("ApplicationContextMenu.cpp:" + ($i + 1) + ": openDirectoryExternal outside the EXTERNAL case: " + $lines[$i].Trim()) }
	}
	return $problems
}

# Rule 5: the platforms named in canOpenDirectoryExternal() and in openDirectoryExternal() are the same
function Find-CapabilityMismatch([string]$text)
{
	$platforms = { param($body) @([regex]::Matches($body, 'PLATFORM_[A-Z0-9]+') | ForEach-Object { $_.Value } | Sort-Object -Unique) }
	$a = $text.IndexOf('bool PlatformFunctions::canOpenDirectoryExternal()')
	$b = $text.IndexOf('void PlatformFunctions::openDirectoryExternal(')
	if ($a -lt 0 -or $b -lt 0) { return @("PlatformFunctions.cpp: canOpenDirectoryExternal or openDirectoryExternal not found") }
	$endB = $text.IndexOf('void PlatformFunctions::', $b + 10)
	if ($endB -lt 0) { $endB = $text.Length }
	if ($a -gt $b) { return @("PlatformFunctions.cpp: canOpenDirectoryExternal must come before openDirectoryExternal") }
	$can = & $platforms $text.Substring($a, $b - $a)
	$open = & $platforms $text.Substring($b, $endB - $b)
	if (($can -join ',') -ne ($open -join ',')) { return @("PlatformFunctions.cpp: canOpenDirectoryExternal names (" + ($can -join ',') + ") but openDirectoryExternal has branches for (" + ($open -join ',') + ")") }
	return @()
}

if ($SelfTest)
{
	$ok = $true
	# rule 2
	$okFile = "#if defined(OPEN_EXTERNAL_TEXT)`n x(); PlatformFunctions::openDirectoryExternal(p);`n#endif"
	$badFile = "x(); PlatformFunctions::openDirectoryExternal(p);"
	$elseFile = "#if defined(OPEN_EXTERNAL_TEXT)`n#else`n PlatformFunctions::openDirectoryExternal(p);`n#endif"
	$negFile = "#ifndef OPEN_EXTERNAL_TEXT`n PlatformFunctions::openDirectoryExternal(p);`n#endif"
	$ok = $ok -and ((Find-UnguardedLines "F.cpp" $okFile '\bopenDirectoryExternal\(' 'OPEN_EXTERNAL_TEXT' '').Count -eq 0)
	$ok = $ok -and ((Find-UnguardedLines "F.cpp" $badFile '\bopenDirectoryExternal\(' 'OPEN_EXTERNAL_TEXT' '').Count -eq 1)
	$ok = $ok -and ((Find-UnguardedLines "F.cpp" $elseFile '\bopenDirectoryExternal\(' 'OPEN_EXTERNAL_TEXT' '').Count -eq 1)
	$ok = $ok -and ((Find-UnguardedLines "F.cpp" $negFile '\bopenDirectoryExternal\(' 'OPEN_EXTERNAL_TEXT' '').Count -eq 1)
	# rule 3
	$desktop = "#if defined(PLATFORM_WINDOWS) || defined(PLATFORM_MAC) || defined(PLATFORM_LINUX)`n m.addEntry(`"Open mods `" X, 1);`n#endif"
	$everywhere = "m.addEntry(`"Open mods `" X, 1);"
	$withWeb = "#if defined(PLATFORM_WINDOWS) || defined(PLATFORM_WEB)`n m.addEntry(`"Open mods `" X, 1);`n#endif"
	$ok = $ok -and ((Find-UnguardedLines "M.cpp" $desktop 'addEntry\("Open mods' 'PLATFORM_WINDOWS' 'PLATFORM_WEB|PLATFORM_ANDROID|PLATFORM_IOS').Count -eq 0)
	$ok = $ok -and ((Find-UnguardedLines "M.cpp" $everywhere 'addEntry\("Open mods' 'PLATFORM_WINDOWS' 'PLATFORM_WEB|PLATFORM_ANDROID|PLATFORM_IOS').Count -eq 1)
	$ok = $ok -and ((Find-UnguardedLines "M.cpp" $withWeb 'addEntry\("Open mods' 'PLATFORM_WINDOWS' 'PLATFORM_WEB|PLATFORM_ANDROID|PLATFORM_IOS').Count -eq 1)
	# rule 4
	$menuOk = "auto a = chooseDirectoryOpenAction(x, y);`nbool ok = isDirectoryOpenAllowedNow(a, g);`nswitch (a)`n{`n case DirectoryOpenAction::EXTERNAL:`n  PlatformFunctions::openDirectoryExternal(path);`n  break;`n}"
	$menuBad = "auto a = chooseDirectoryOpenAction(x, y);`nbool ok = isDirectoryOpenAllowedNow(a, g);`nswitch (a)`n{`n case DirectoryOpenAction::FILE_BROWSER:`n  PlatformFunctions::openDirectoryExternal(path);`n  break;`n}"
	$menuOld = "case Function::OPEN_MODS:`n PlatformFunctions::openDirectoryExternal(path);"
	$ok = $ok -and ((Find-MenuCallOutsideExternalCase $menuOk).Count -eq 0)
	$ok = $ok -and ((Find-MenuCallOutsideExternalCase $menuBad).Count -eq 1)
	$ok = $ok -and ((Find-MenuCallOutsideExternalCase $menuOld).Count -eq 3)
	$menuNoGate = "auto a = chooseDirectoryOpenAction(x, y);`nswitch (a)`n{`n case DirectoryOpenAction::EXTERNAL:`n  PlatformFunctions::openDirectoryExternal(path);`n  break;`n}"
	$ok = $ok -and ((Find-MenuCallOutsideExternalCase $menuNoGate).Count -eq 1)
	# the log file entry (rule 4): only inside #if 0
	$logOff = "#if 0`n mItems.emplace_back(Item { `"Open log file`", Item::Function::OPEN_LOGFILE });`n#endif"
	$logOn = "mItems.emplace_back(Item { `"Open log file`", Item::Function::OPEN_LOGFILE });"
	$logCase = "case Item::Function::OPEN_LOGFILE:`n x();"
	$ok = $ok -and ((Find-UnguardedLines "C.cpp" $logOff 'Function::OPEN_LOGFILE\s*\}' '^\s*0\s*$' '').Count -eq 0)
	$ok = $ok -and ((Find-UnguardedLines "C.cpp" $logOn 'Function::OPEN_LOGFILE\s*\}' '^\s*0\s*$' '').Count -eq 1)
	$ok = $ok -and ((Find-UnguardedLines "C.cpp" $logCase 'Function::OPEN_LOGFILE\s*\}' '^\s*0\s*$' '').Count -eq 0)
	# rule 5
	$sameList = "bool PlatformFunctions::canOpenDirectoryExternal()`n{`n#if defined(PLATFORM_WINDOWS) || defined(PLATFORM_MAC)`n return true;`n#endif`n}`nvoid PlatformFunctions::openDirectoryExternal(p)`n{`n#if defined(PLATFORM_WINDOWS)`n a;`n#elif defined(PLATFORM_MAC)`n b;`n#endif`n}`nvoid PlatformFunctions::openURLExternal(u)`n{`n#ifdef PLATFORM_WEB`n#endif`n}"
	$moreCan = "bool PlatformFunctions::canOpenDirectoryExternal()`n{`n#if defined(PLATFORM_WINDOWS) || defined(PLATFORM_MAC) || defined(PLATFORM_WEB)`n return true;`n#endif`n}`nvoid PlatformFunctions::openDirectoryExternal(p)`n{`n#if defined(PLATFORM_WINDOWS)`n a;`n#elif defined(PLATFORM_MAC)`n b;`n#endif`n}"
	$ok = $ok -and ((Find-CapabilityMismatch $sameList).Count -eq 0)
	$ok = $ok -and ((Find-CapabilityMismatch $moreCan).Count -eq 1)
	$ok = $ok -and ((Find-CapabilityMismatch "nothing here").Count -eq 1)
	if ($ok) { Write-Host "PASS lint_open_folder_guard self test"; exit 0 }
	Write-Host "FAIL lint_open_folder_guard self test" -ForegroundColor Red; exit 1
}

$problems = @()
$scanned = 0
$found = @{}
$files = Get-ChildItem -Path $Source -Recurse -Include *.cpp,*.h,*.hpp,*.mm -File | Where-Object { $_.FullName.Substring($Source.Length) -notmatch '[\\/](third_party|thirdparty|external|_emscripten|build)[\\/]' }
foreach ($f in $files)
{
	$text = Get-Content $f.FullName -Raw
	$scanned++
	if ($f.Name -eq "PlatformFunctions.cpp") { $problems += @(Find-CapabilityMismatch $text); continue }
	if ($f.Name -eq "PlatformFunctions.h") { continue }
	if ($f.Name -eq "ModsStartPage.cpp") { $problems += @(Find-UnguardedLines $f.Name $text 'addEntry\("Open mods' 'PLATFORM_WINDOWS' 'PLATFORM_WEB|PLATFORM_ANDROID|PLATFORM_IOS') }
	if ($text -notmatch '\bopenDirectoryExternal\(') { continue }
	if ($allowed -notcontains $f.Name) { $problems += ($f.Name + ": calls openDirectoryExternal but is not in the allowed list; make sure it only runs where canOpenDirectoryExternal() is true, then add it"); continue }
	$found[$f.Name] = $true
	if ($f.Name -eq "ImGuiFileBrowser.cpp") { $problems += @(Find-UnguardedLines $f.Name $text '\bopenDirectoryExternal\(' '\bOPEN_EXTERNAL_TEXT\b' '') }
	if ($f.Name -eq "ApplicationContextMenu.cpp")
	{
		$problems += @(Find-MenuCallOutsideExternalCase $text)
		# the "Open log file" entry calls openFileExternal, which has no branch for the web, Android, iOS or Mac: it stays switched off (#if 0) until it has a guard of its own
		$problems += @(Find-UnguardedLines $f.Name $text 'Function::OPEN_LOGFILE\s*\}' '^\s*0\s*$' '')
	}
}
foreach ($name in $allowed) { if (-not $found.ContainsKey($name)) { $problems += ($name + ": expected caller not found (renamed or removed? update lint_open_folder_guard.ps1)") } }

if ($problems.Count -gt 0) { $problems | ForEach-Object { Write-Host "FAIL $_" -ForegroundColor Red }; exit 1 }
Write-Host ("PASS lint_open_folder_guard: " + $scanned + " source files scanned, openDirectoryExternal only behind a platform guard or the EXTERNAL case")
