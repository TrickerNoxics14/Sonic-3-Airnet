# Installs an update that the game has already downloaded, verified (SHA-256) and unpacked into a staging folder.
# Started by the game (hidden) right before it closes. Runs in Windows PowerShell 5.1.
#
# What it does, in this order:
#   1. waits for the game to exit and for Sonic3AIR.exe to be free (nothing is changed if that does not happen, the old game is started again)
#   2. checks every staged file against the list the game wrote (SHA-256 and size)
#   3. backs up every file it is about to replace or delete, copies the new files over, checks the copies
#   4. installs the co-op mod from bundled_mods into the mods folder of the game's data folder
#   5. on ANY error: puts everything back from the backup (rollback) and starts the old game again
#   6. on success: removes the staging folder, keeps the backup of the last update, starts the new game
# It never touches: saves, save states, user settings, settings.json, active-mods.json, mods the user installed (only the folder DropInCoop of the
# bundled co-op mod is replaced), the ROM, config.json (kept as it is if it exists), and anything in the game folder that is not part of the package.
# It never runs anything from the staging folder.
param(
	[Parameter(Mandatory = $true)][string]$GameDir,
	[Parameter(Mandatory = $true)][string]$StagingDir,
	[Parameter(Mandatory = $true)][string]$UpdateDir,
	[Parameter(Mandatory = $true)][string]$ModsDir,
	[string]$ExeName = "Sonic3AIR.exe",
	[int]$ProcessId = 0,
	[string]$FromVersion = "",
	[string]$ToVersion = "",
	[int]$WaitSeconds = 60,
	[int]$LockWaitSeconds = 30,
	[switch]$NoRestart,
	[string]$TestFailAt = ""		# test switch of the automated checks only (the game never passes it): "afterGameFiles" or "afterMods" makes the helper fail on purpose, to prove the rollback
)

$ErrorActionPreference = 'Stop'
$logPath = Join-Path $UpdateDir "helper_log.txt"
$resultPath = Join-Path $UpdateDir "last_result.txt"

function Write-Log([string]$text)
{
	try { Add-Content -LiteralPath $logPath -Value ("{0}  {1}" -f (Get-Date -Format "yyyy-MM-dd HH:mm:ss"), $text) -Encoding ASCII } catch { }
}

function Write-Result([string]$text)
{
	try { Set-Content -LiteralPath $resultPath -Value $text -Encoding ASCII } catch { }
	Write-Log ("RESULT: " + $text)
}

function Start-Game
{
	if ($NoRestart) { Write-Log "not restarting the game (test switch)"; return }
	try { Start-Process -FilePath (Join-Path $GameDir $ExeName) -WorkingDirectory $GameDir } catch { Write-Log ("could not start the game: " + $_.Exception.Message) }
}

# Never installed, never overwritten, never deleted by an update
$protectedPattern = '(?i)(^|/)(saves|states|savedata|mods)(/|$)|(^|/)settings[^/]*\.json$|(^|/)active-mods\.json$|(^|/)testhooks\.enabled$|\.(gen|smd|sav|srm)$'
# Installed once, afterwards the user's: kept as it is if it exists
$keepIfExistsPattern = '(?i)^config\.json$'

# SHA-256 of a file with the .NET class directly (no module has to be loaded)
function Get-Sha256Hex([string]$path)
{
	$sha = [System.Security.Cryptography.SHA256]::Create()
	try
	{
		$stream = [System.IO.File]::Open($path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::Read)
		try { return ([System.BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-', '').ToLowerInvariant()) } finally { $stream.Dispose() }
	}
	finally { $sha.Dispose() }
}

function Test-SafeRelativePath([string]$relative)
{
	if ([string]::IsNullOrWhiteSpace($relative)) { return $false }
	if ($relative -match '[:\\<>"|?*\x00-\x1f]') { return $false }
	if ($relative.StartsWith('/')) { return $false }
	foreach ($part in $relative.Split('/')) { if ($part -eq '' -or $part -eq '.' -or $part -eq '..' -or $part.EndsWith('.') -or $part.EndsWith(' ')) { return $false } }
	return $true
}

function Get-FullInside([string]$root, [string]$relative)
{
	$full = [IO.Path]::GetFullPath((Join-Path $root ($relative.Replace('/', '\'))))
	$rootFull = [IO.Path]::GetFullPath($root).TrimEnd('\') + '\'
	if (-not $full.StartsWith($rootFull, [StringComparison]::OrdinalIgnoreCase)) { throw "path leaves its folder: $relative" }
	return $full
}

New-Item -ItemType Directory -Path $UpdateDir -Force | Out-Null
Write-Log ("=== update helper: " + $FromVersion + " -> " + $ToVersion + ", game folder " + $GameDir)

# State for the rollback
$backupRoot = Join-Path $UpdateDir "backup"
$backupDir = Join-Path $backupRoot ("from_" + $FromVersion + "_to_" + $ToVersion)
$addedFiles = New-Object System.Collections.ArrayList			# did not exist before
$changedFiles = New-Object System.Collections.ArrayList			# existed, backed up (replaced or deleted)
$modsBackedUp = $false
$modsReplaced = $false
$changesStarted = $false

function Test-SameFile([string]$first, [string]$second)
{
	# True only if both files can be read and are identical (a file that could not be replaced in the first place needs no restoring)
	try
	{
		if (-not (Test-Path -LiteralPath $first -PathType Leaf)) { return $false }
		if ((Get-Item -LiteralPath $first).Length -ne (Get-Item -LiteralPath $second).Length) { return $false }
		return ((Get-Sha256Hex $first) -eq (Get-Sha256Hex $second))
	}
	catch { return $false }
}

function Restore-Everything
{
	# Puts the game folder (and the mod folder) back as it was before the helper changed anything
	$problems = 0
	foreach ($relative in $addedFiles)
	{
		try { $path = Get-FullInside $GameDir $relative; if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Force } } catch { $problems++; Write-Log ("rollback: could not remove " + $relative + ": " + $_.Exception.Message) }
	}
	foreach ($relative in $changedFiles)
	{
		try
		{
			$source = Get-FullInside (Join-Path $backupDir "game") $relative
			$path = Get-FullInside $GameDir $relative
			if (Test-SameFile $path $source) { continue }
			New-Item -ItemType Directory -Path (Split-Path -Parent $path) -Force | Out-Null
			[IO.File]::Copy($source, $path, $true)
		}
		catch { $problems++; Write-Log ("rollback: could not restore " + $relative + ": " + $_.Exception.Message) }
	}
	if ($modsReplaced)
	{
		try
		{
			$modTarget = Join-Path $ModsDir "DropInCoop"
			if (Test-Path -LiteralPath $modTarget) { Remove-Item -LiteralPath $modTarget -Recurse -Force }
			if ($modsBackedUp) { Copy-Item -LiteralPath (Join-Path $backupDir "mods\DropInCoop") -Destination $modTarget -Recurse -Force }
		}
		catch { $problems++; Write-Log ("rollback: could not restore the co-op mod: " + $_.Exception.Message) }
	}
	return $problems
}

try
{
	# ---- 1. Wait for the game ------------------------------------------------------------------------------------------------------
	if ($ProcessId -gt 0)
	{
		$process = Get-Process -Id $ProcessId -ErrorAction SilentlyContinue
		if ($null -ne $process)
		{
			Write-Log ("waiting for the game (process " + $ProcessId + ") to close")
			if (-not $process.WaitForExit($WaitSeconds * 1000))
			{
				Write-Result "FAILED: the game did not close in time, nothing was changed"
				exit 5
			}
		}
	}
	$exePath = Join-Path $GameDir $ExeName
	$deadline = (Get-Date).AddSeconds($LockWaitSeconds)
	$free = $false
	while ((Get-Date) -lt $deadline)
	{
		try { $stream = [IO.File]::Open($exePath, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None); $stream.Dispose(); $free = $true; break }
		catch { Start-Sleep -Milliseconds 500 }
	}
	if (-not $free)
	{
		Write-Log ("the program file stays locked: " + $exePath)
		Write-Result "FAILED: the game program file is still in use (another copy running, or a virus scanner), nothing was changed"
		Start-Game
		exit 3
	}

	# ---- 2. Check the staging folder against the list the game wrote ---------------------------------------------------------------
	$listPath = Join-Path $UpdateDir "staging_manifest.txt"
	if (-not (Test-Path -LiteralPath $listPath)) { throw "the list of staged files is missing" }
	$staged = New-Object System.Collections.ArrayList
	foreach ($line in Get-Content -LiteralPath $listPath)
	{
		if ($line.Trim() -eq '') { continue }
		if ($line -notmatch '^([0-9a-f]{64})  (\d+)  (.+)$') { throw "bad line in the list of staged files" }
		$relative = $Matches[3]
		if (-not (Test-SafeRelativePath $relative)) { throw ("unsafe path in the list of staged files: " + $relative) }
		[void]$staged.Add([pscustomobject]@{ Hash = $Matches[1]; Size = [int64]$Matches[2]; Path = $relative })
	}
	if ($staged.Count -eq 0) { throw "the list of staged files is empty" }
	$hasExe = $false
	foreach ($item in $staged)
	{
		$full = Get-FullInside $StagingDir $item.Path
		if (-not (Test-Path -LiteralPath $full -PathType Leaf)) { throw ("staged file is missing: " + $item.Path) }
		if ((Get-Item -LiteralPath $full).Length -ne $item.Size) { throw ("staged file has the wrong size: " + $item.Path) }
		if ((Get-Sha256Hex $full) -ne $item.Hash) { throw ("staged file does not match its SHA-256: " + $item.Path) }
		if ($item.Path -ieq $ExeName) { $hasExe = $true }
	}
	if (-not $hasExe) { throw "the staged files do not contain the game program" }
	Write-Log ("staging folder checked: " + $staged.Count + " files")

	# ---- 3. Plan: what is copied, what is deleted ----------------------------------------------------------------------------------
	$toCopy = New-Object System.Collections.ArrayList
	foreach ($item in $staged)
	{
		if ($item.Path -match $protectedPattern) { Write-Log ("skipped (protected): " + $item.Path); continue }
		$destination = Get-FullInside $GameDir $item.Path
		if (($item.Path -match $keepIfExistsPattern) -and (Test-Path -LiteralPath $destination)) { Write-Log ("kept as it is (user file): " + $item.Path); continue }
		[void]$toCopy.Add($item)
	}
	$newPaths = @{}
	foreach ($item in $staged) { $newPaths[$item.Path.ToLowerInvariant()] = $true }
	$toDelete = New-Object System.Collections.ArrayList
	$installedListPath = Join-Path $GameDir "installed_files.txt"
	if (Test-Path -LiteralPath $installedListPath)
	{
		foreach ($line in Get-Content -LiteralPath $installedListPath)
		{
			$relative = $line.Trim()
			if ($relative -eq '' -or -not (Test-SafeRelativePath $relative)) { continue }
			if ($relative -match $protectedPattern -or $relative -match $keepIfExistsPattern) { continue }
			if ($newPaths.ContainsKey($relative.ToLowerInvariant())) { continue }
			if (Test-Path -LiteralPath (Get-FullInside $GameDir $relative) -PathType Leaf) { [void]$toDelete.Add($relative) }
		}
	}

	# ---- 4. Backup (one backup is kept: the one of the last update) ------------------------------------------------------------------
	if (Test-Path -LiteralPath $backupRoot) { Get-ChildItem -LiteralPath $backupRoot -Directory | ForEach-Object { Remove-Item -LiteralPath $_.FullName -Recurse -Force } }
	New-Item -ItemType Directory -Path (Join-Path $backupDir "game") -Force | Out-Null
	$backupTargets = New-Object System.Collections.ArrayList
	foreach ($item in $toCopy) { if (Test-Path -LiteralPath (Get-FullInside $GameDir $item.Path)) { [void]$backupTargets.Add($item.Path) } }
	foreach ($relative in $toDelete) { [void]$backupTargets.Add($relative) }
	if (Test-Path -LiteralPath $installedListPath) { [void]$backupTargets.Add("installed_files.txt") }
	foreach ($relative in $backupTargets)
	{
		$source = Get-FullInside $GameDir $relative
		$copy = Get-FullInside (Join-Path $backupDir "game") $relative
		New-Item -ItemType Directory -Path (Split-Path -Parent $copy) -Force | Out-Null
		[IO.File]::Copy($source, $copy, $true)
	}
	$modTargetDir = Join-Path $ModsDir "DropInCoop"
	$stagedMod = Join-Path $StagingDir "bundled_mods\DropInCoop"
	if ((Test-Path -LiteralPath $stagedMod -PathType Container) -and (Test-Path -LiteralPath $modTargetDir -PathType Container))
	{
		Copy-Item -LiteralPath $modTargetDir -Destination (Join-Path $backupDir "mods\DropInCoop") -Recurse -Force
		$modsBackedUp = $true
	}
	Write-Log ("backup made: " + $backupTargets.Count + " files in " + $backupDir)

	# ---- 5. Install ----------------------------------------------------------------------------------------------------------------
	$changesStarted = $true
	$orderedCopy = @($toCopy | Where-Object { $_.Path -ine $ExeName }) + @($toCopy | Where-Object { $_.Path -ieq $ExeName })		# the program file last
	foreach ($item in $orderedCopy)
	{
		$source = Get-FullInside $StagingDir $item.Path
		$destination = Get-FullInside $GameDir $item.Path
		$existed = Test-Path -LiteralPath $destination
		New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
		if ($existed) { [void]$changedFiles.Add($item.Path) } else { [void]$addedFiles.Add($item.Path) }
		[IO.File]::Copy($source, $destination, $true)
		if ((Get-Sha256Hex $destination) -ne $item.Hash) { throw ("the copy of " + $item.Path + " does not match") }
	}
	foreach ($relative in $toDelete)
	{
		[void]$changedFiles.Add($relative)
		Remove-Item -LiteralPath (Get-FullInside $GameDir $relative) -Force
	}
	if (Test-Path -LiteralPath $installedListPath) { [void]$changedFiles.Add("installed_files.txt") } else { [void]$addedFiles.Add("installed_files.txt") }
	if ($TestFailAt -eq "afterGameFiles") { throw "test failure after the game files" }
	$installedLines = @($staged | Where-Object { $_.Path -notmatch $protectedPattern } | ForEach-Object { $_.Path })
	Set-Content -LiteralPath $installedListPath -Value $installedLines -Encoding ASCII

	# The co-op mod: the game only loads mods from its data folder, so the bundled copy is installed there
	if (Test-Path -LiteralPath $stagedMod -PathType Container)
	{
		New-Item -ItemType Directory -Path $ModsDir -Force | Out-Null
		$modsReplaced = $true
		if (Test-Path -LiteralPath $modTargetDir) { Remove-Item -LiteralPath $modTargetDir -Recurse -Force }
		Copy-Item -LiteralPath $stagedMod -Destination $modTargetDir -Recurse -Force
		Write-Log "co-op mod installed into the mods folder"
	}
	if ($TestFailAt -eq "afterMods") { throw "test failure after the mods" }

	# ---- 6. Done -------------------------------------------------------------------------------------------------------------------
	try { Remove-Item -LiteralPath $StagingDir -Recurse -Force; Remove-Item -LiteralPath $listPath -Force } catch { Write-Log ("could not remove the staging folder: " + $_.Exception.Message) }
	Write-Result ("Updated from v" + $FromVersion + " to v" + $ToVersion + ".")
	Start-Game
	exit 0
}
catch
{
	$reason = $_.Exception.Message
	Write-Log ("ERROR: " + $reason)
	if ($changesStarted)
	{
		$problems = Restore-Everything
		if ($problems -eq 0)
		{
			Write-Result ("FAILED and rolled back, the old version is back: " + $reason)
			Start-Game
			exit 1
		}
		Write-Result ("FAILED and the rollback was incomplete (" + $problems + " problems), the backup is in " + $backupDir + ": " + $reason)
		exit 2
	}
	Write-Result ("FAILED, nothing was changed: " + $reason)
	Start-Game
	exit 4
}
