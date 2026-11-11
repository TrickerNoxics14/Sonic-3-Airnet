# Automated checks for Sonic 3 A.I.R. Drop-In Co-op (split screen and online play).
# The list of checks and what each one proves is in checks\CHECKLIST.md.
#
#   checks\run_checks.bat                 everything (uses the current build and package)
#   checks\run_checks.bat -Build          compile the engine first (a few minutes), then everything
#   checks\run_checks.bat -Package        rebuild the release package first, then everything
#   checks\run_checks.bat -Quick          static checks and the engine self test only (no game windows)
#
# Result: a table on screen and checks\last_report.txt. Exit code 0 only if every check passed.
# While the game checks run, the game is started and closed by this script. Close the game before starting.
# Your mod list, config.json and the Drop-In Co-op settings are restored afterwards.

param(
	[switch]$Build,
	[switch]$Package,
	[switch]$Quick,
	[switch]$RuntimeOnly,
	[string]$EngineRoot = '',
	[string[]]$Only = @()	# Only some of the game stages D, E, F, for example: -Only D,F
)

$ErrorActionPreference = "Stop"
# All runs share game configuration and logs; acquire ownership before touching either.
$checkRunMutex = New-Object System.Threading.Mutex($false, "Local\S3AIRDropInChecks")
if (-not $checkRunMutex.WaitOne(0)) {
	$checkRunMutex.Dispose()
	throw "Another co-op check run owns the game and logs. Wait for it to finish."
}
Add-Type -AssemblyName System.IO.Compression.FileSystem

# Accept "-Only D,F" as well as "-Only D F"
$Only = @($Only | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim().ToUpper() } | Where-Object { $_ })

$checksDir = $PSScriptRoot
$root = Split-Path -Parent $checksDir
$sourceRoot = if ($EngineRoot) { [IO.Path]::GetFullPath($EngineRoot) } else { Join-Path $root "sonic3air_source" }
$game = Join-Path $sourceRoot "Oxygen\sonic3air"
$bin = Join-Path $game "bin\Release_x64"
$exe = Join-Path $bin "Sonic3AIR.exe"
$appdata = Join-Path $env:APPDATA "Sonic3AIR"
$logFile = Join-Path $appdata "logfile.txt"
$hostLogFile = Join-Path $appdata "logfile_host.txt"		# Two game windows on this computer each write their own log (see S3AIR_LOGNAME)
$friendLogFile = Join-Path $appdata "logfile_friend.txt"
$onlineLog = Join-Path $appdata "online_log.txt"
$onlineHostLog = Join-Path $appdata 'online_log_host.txt'
$onlineFriendLog = Join-Path $appdata 'online_log_friend.txt'
$selfTestFile = Join-Path $appdata "selftest_results.txt"
$zipPath = Join-Path $root "Sonic3AIR_Online.zip"
$sourceScripts = Join-Path $game "scripts"
# The release zip is built from the stable online fork, not from the older test tree used for the local game stages (F/G/Q...).
# The A-stage package comparisons must look at the tree the package was actually built from.
$pkgRoot = if ($EngineRoot) { $sourceRoot } else { Join-Path $root "sonic3air_online_stable_source" }
$pkgGame = Join-Path $pkgRoot "Oxygen\sonic3air"
$pkgExe = Join-Path $pkgGame "bin\Release_x64\Sonic3AIR.exe"
$pkgScripts = Join-Path $pkgGame "scripts"
$sourceMod = Join-Path $root "DropInCoop"
$harnessSource = Join-Path $checksDir "harness\ChecksHarness"
$reportPath = if ($RuntimeOnly -and $EngineRoot) { Join-Path $checksDir "online_stable_runtime_report.txt" } else { Join-Path $checksDir "last_report.txt" }
$recoveryPath = Join-Path $checksDir '.run-recovery.json'
# The engine honours the developer switches (S3AIR_NETPLAY, S3AIR_LOGNAME, ...) only while this marker file lies next to the executable.
# This run creates it for its own duration and removes it again; the release package never contains it (make_package.ps1 refuses to ship it).
$testHooksMarker = Join-Path $bin 'testhooks.enabled'
$gameExitCode = 0

$results = New-Object System.Collections.ArrayList
$startedProcesses = New-Object System.Collections.ArrayList

function Add-Result([string]$id, [string]$title, [bool]$passed, [string]$detail)
{
	[void]$results.Add([pscustomobject]@{ Id = $id; Title = $title; Passed = $passed; Detail = $detail })
	$mark = if ($passed) { "PASS" } else { "FAIL" }
	$color = if ($passed) { "Green" } else { "Red" }
	Write-Host ("  [" + $mark + "] " + $id + "  " + $title + $(if ($detail) { "  -- " + $detail } else { "" })) -ForegroundColor $color
}

function Write-Step([string]$text)
{
	Write-Host ""
	Write-Host $text -ForegroundColor Cyan
}

function Read-SharedText([string]$path)
{
	# The game keeps its log files open while it runs, so read them without locking them
	if (-not (Test-Path $path)) { return "" }
	try
	{
		$stream = [IO.File]::Open($path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
		try
		{
			$reader = New-Object IO.StreamReader($stream)
			return $reader.ReadToEnd()
		}
		finally
		{
			$stream.Dispose()
		}
	}
	catch
	{
		return ""
	}
}

function Wait-Until([scriptblock]$condition, [int]$seconds)
{
	$deadline = (Get-Date).AddSeconds($seconds)
	while ((Get-Date) -lt $deadline)
	{
		if (& $condition) { return $true }
		Start-Sleep -Milliseconds 500
	}
	return $false
}

function Start-GameProcess([hashtable]$variables)
{
	$variables['S3AIR_CHECKS'] = '1'
	foreach ($key in $variables.Keys)
	{
		[Environment]::SetEnvironmentVariable($key, [string]$variables[$key], "Process")
	}
	try
	{
		$process = Start-Process -FilePath $exe -WorkingDirectory $game -PassThru
	}
	finally
	{
		foreach ($key in $variables.Keys)
		{
			[Environment]::SetEnvironmentVariable($key, $null, "Process")
		}
	}
	[void]$startedProcesses.Add($process)
	return $process
}

function Stop-StartedProcesses
{
	foreach ($process in $startedProcesses)
	{
		if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue }
	}
	$startedProcesses.Clear()
	Start-Sleep -Milliseconds 800
}

function Assert-NoGameRunning
{
	if (Get-Process -Name "Sonic3AIR" -ErrorAction SilentlyContinue)
	{
		throw "Sonic 3 A.I.R. is still running. Close it first, the checks start and stop their own game windows."
	}
}

function Set-ActiveMods([string[]]$mods)
{
	$items = ($mods | ForEach-Object { '"' + $_ + '"' }) -join ", "
	$json = "{`n`t`"ActiveMods`" : [" + $items + "],`n`t`"UseLegacyLoading`" : false`n}"
	Set-Content -Path (Join-Path $appdata "mods\active-mods.json") -Value $json -Encoding utf8
}

function Get-TextHash([string]$text)
{
	$sha = [Security.Cryptography.SHA256]::Create()
	return ([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($text)))).Replace("-", "")
}

function Get-FileHashHex([string]$path)
{
	return (Get-FileHash -Path $path -Algorithm SHA256).Hash
}

function Get-EntryHashHex($entry)
{
	$stream = $entry.Open()
	try
	{
		$sha = [Security.Cryptography.SHA256]::Create()
		return ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace("-", "")
	}
	finally
	{
		$stream.Dispose()
	}
}

function Get-EntryText($entry)
{
	$stream = $entry.Open()
	try
	{
		$reader = New-Object IO.StreamReader($stream)
		return $reader.ReadToEnd()
	}
	finally
	{
		$stream.Dispose()
	}
}

function New-SessionCode
{
	# 8 random characters (the first one is never 0) plus one check character, as the game's NetplayCodes::makeSessionCode writes them
	$alphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"
	$indices = @(1..8 | ForEach-Object { Get-Random -Maximum 32 })
	if ($indices[0] -eq 0) { $indices[0] = 1 }
	$sum = 7
	for ($i = 0; $i -lt 8; $i++) { $sum += (2 * $i + 1) * $indices[$i] }
	$chars = @($indices | ForEach-Object { $alphabet[$_] })
	return ((-join $chars[0..3]) + "-" + (-join $chars[4..7]) + $alphabet[$sum -band 31])
}

function New-Secret
{
	# A random 30 bit join secret as 8 hexadecimal digits (developer test modes hostdirect: and joindirect:)
	return ('{0:x8}' -f (Get-Random -Minimum 1 -Maximum 0x3fffffff))
}

function Get-NetLog([string]$suffix)
{
	return Read-SharedText (Join-Path $appdata ("logfile" + $suffix + ".txt"))
}

function Get-OnlineLines
{
	$text = (Read-SharedText $onlineHostLog) + "`n" + (Read-SharedText $onlineFriendLog)
	return @($text -split "`r?`n" | Where-Object { $_ -match '^\[(host|friend)\]' })
}

function Get-FriendFps
{
	# Only the lines from inside the game (state 5); the lines before are the join itself
	$lines = @(Get-OnlineLines | Where-Object { $_ -match '^\[friend\].* state=5 ' })
	$values = @()
	for ($i = 2; $i -lt $lines.Count; $i++)		# The first seconds in the game are skipped as well
	{
		$match = [regex]::Match($lines[$i], 'fps=([\d.]+)')
		if ($match.Success) { $values += [double]$match.Groups[1].Value }
	}
	return $values
}

function Get-FriendRepairs
{
	$max = 0
	foreach ($line in @(Get-OnlineLines | Where-Object { $_ -match '^\[friend\]' }))
	{
		$match = [regex]::Match($line, 'repairs=(\d+)')
		if ($match.Success -and [int]$match.Groups[1].Value -gt $max) { $max = [int]$match.Groups[1].Value }
	}
	return $max
}

function Save-RunLogs([string]$tag)
{
	# The logs of the last run stay next to the report, so a failed check can be looked into afterwards
	$logsFolder = Join-Path $checksDir "last_logs"
	New-Item -ItemType Directory -Path $logsFolder -Force | Out-Null
	if (Test-Path $friendLogFile) { Copy-Item $friendLogFile (Join-Path $logsFolder ($tag + "-logfile_friend.txt")) -Force }
	if (Test-Path $hostLogFile) { Copy-Item $hostLogFile (Join-Path $logsFolder ($tag + "-logfile_host.txt")) -Force }
	if (Test-Path $onlineLog) { Copy-Item $onlineLog (Join-Path $logsFolder ($tag + "-online_log.txt")) -Force }
	if (Test-Path $onlineHostLog) { Copy-Item $onlineHostLog (Join-Path $logsFolder ($tag + '-online_log_host.txt')) -Force }
	if (Test-Path $onlineFriendLog) { Copy-Item $onlineFriendLog (Join-Path $logsFolder ($tag + '-online_log_friend.txt')) -Force }
}

# Screenshots of the game window (the picture is checked by reading the PNG files in checks\last_logs)
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class GameWindowCapture
{
	[StructLayout(LayoutKind.Sequential)]
	public struct RECT { public int Left; public int Top; public int Right; public int Bottom; }
	[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
	[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
}
"@

# The whole screen (for error windows, which are separate windows of their own)
function Save-ScreenPicture([string]$path)
{
	$bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
	$bitmap = New-Object System.Drawing.Bitmap $bounds.Width, $bounds.Height
	$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
	$graphics.CopyFromScreen($bounds.Left, $bounds.Top, 0, 0, $bitmap.Size)
	$bitmap.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
	$graphics.Dispose()
	$bitmap.Dispose()
}

function Save-GameScreenshot([System.Diagnostics.Process]$process, [string]$path)
{
	$process.Refresh()
	$handle = $process.MainWindowHandle
	if ($handle -eq [IntPtr]::Zero) { return $false }
	[void][GameWindowCapture]::SetForegroundWindow($handle)
	Start-Sleep -Milliseconds 400
	$rect = New-Object GameWindowCapture+RECT
	[void][GameWindowCapture]::GetWindowRect($handle, [ref]$rect)
	$width = $rect.Right - $rect.Left
	$height = $rect.Bottom - $rect.Top
	if ($width -le 0 -or $height -le 0) { return $false }
	$bitmap = New-Object System.Drawing.Bitmap $width, $height
	$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
	$graphics.CopyFromScreen($rect.Left, $rect.Top, 0, 0, $bitmap.Size)
	$bitmap.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
	$graphics.Dispose()
	$bitmap.Dispose()
	return $true
}

function Get-QuadCaptureStats([string]$path)
{
	if (-not (Test-Path $path)) { return $null }
	$bitmap = [System.Drawing.Bitmap]::FromFile($path)
	try
	{
		$halfWidth = [int]($bitmap.Width / 2)
		$halfHeight = [int]($bitmap.Height / 2)
		$counts = @()
		for ($view = 0; $view -lt 4; ++$view)
		{
			$x0 = ($view % 2) * $halfWidth
			$y0 = [int]([math]::Floor($view / 2)) * $halfHeight
			$yStart = $y0 + $(if ($view -lt 2) { [math]::Min(24, $halfHeight / 3) } else { 0 })
			$yEnd = $y0 + $halfHeight - $(if ($view -ge 2) { [math]::Min(12, $halfHeight / 6) } else { 0 })
			$count = 0
			for ($y = $yStart; $y -lt $yEnd; $y += 2)
			{
				for ($x = $x0; $x -lt ($x0 + $halfWidth); $x += 2)
				{
					$pixel = $bitmap.GetPixel($x, $y)
					if (($pixel.R + $pixel.G + $pixel.B) -gt 100) { ++$count }
				}
			}
			$counts += $count
		}
		return [pscustomobject]@{ Width=$bitmap.Width; Height=$bitmap.Height; Quadrants=$counts }
	}
	finally { $bitmap.Dispose() }
}

function Wants([string]$stage)
{
	return ($Only.Count -eq 0) -or ($Only -contains $stage)
}

function Invoke-OnlineJoin([string]$code, [hashtable]$extraVariables, [int]$secondsAfterJoin, [string]$tag)
{
	# Host and friend on this computer, joined only by the code (the same network path as two laptops on one network)
	Set-ActiveMods @('DropInCoop')
	Stop-StartedProcesses
	Remove-Item $onlineLog -ErrorAction SilentlyContinue
	Remove-Item $onlineHostLog, $onlineFriendLog -ErrorAction SilentlyContinue
	Remove-Item $hostLogFile -ErrorAction SilentlyContinue
	Remove-Item $friendLogFile -ErrorAction SilentlyContinue

	$hostVariables = @{}
	$friendVariables = @{}
	foreach ($key in $extraVariables.Keys) { $hostVariables[$key] = $extraVariables[$key]; $friendVariables[$key] = $extraVariables[$key] }
	$hostVariables["S3AIR_NETPLAY"] = "hostlan:" + $code
	$friendVariables["S3AIR_NETPLAY"] = "joinlan:" + $code
	$hostVariables["S3AIR_LOGNAME"] = "_host"
	$friendVariables["S3AIR_LOGNAME"] = "_friend"

	[void](Start-GameProcess $hostVariables)
	Start-Sleep -Seconds 10
	[void](Start-GameProcess $friendVariables)

	$reached = Wait-Until { (@(Get-OnlineLines | Where-Object { $_ -match '^\[friend\].* state=5 ' })).Count -gt 0 } 90
	if ($reached) { Start-Sleep -Seconds $secondsAfterJoin }

	$logText = (Read-SharedText $friendLogFile) + "`n" + (Read-SharedText $hostLogFile)
	$lines = Get-OnlineLines
	Stop-StartedProcesses
	Set-Content -Path $onlineLog -Value $lines -Encoding utf8
	Save-RunLogs $tag
	return [pscustomobject]@{ Reached = $reached; Log = $logText; Lines = $lines }
}

# ---------------------------------------------------------------------------------------------------------------
# Start

Write-Host ""
Write-Host "Sonic 3 A.I.R. Drop-In Co-op: automated checks" -ForegroundColor White
Write-Host ("Project: " + $root)
Write-Host ""

try
{
	Assert-NoGameRunning
	if (Test-Path -LiteralPath $testHooksMarker) { Remove-Item -LiteralPath $testHooksMarker -Force }		# Left behind by an interrupted run
	if (Test-Path -LiteralPath $recoveryPath) {
		$recovery = Get-Content -Raw -LiteralPath $recoveryPath | ConvertFrom-Json
		[IO.File]::WriteAllText((Join-Path $game 'config.json'), $recovery.Config)
		$modsRecoveryPath = Join-Path $appdata 'mods\active-mods.json'
		if ($recovery.HadMods) { [IO.File]::WriteAllText($modsRecoveryPath, $recovery.Mods) }
		elseif (Test-Path -LiteralPath $modsRecoveryPath) { Remove-Item -LiteralPath $modsRecoveryPath -Force }
		Remove-Item -LiteralPath $recoveryPath -Force
		Write-Host 'Recovered player configuration from an interrupted check run.'
	}

	if ($Build)
	{
		Write-Step "Building the engine (this takes a few minutes)..."
		& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $sourceRoot "build_game.ps1") *> (Join-Path $sourceRoot "build_game.log")
		if ((Read-SharedText (Join-Path $sourceRoot 'build_game.log')) -notmatch '(?m)^exit=0\s*$') {
			throw 'Engine build failed. See sonic3air_source/build_game.log; no package or game checks will be run.'
		}
	}
	if ($Build -or $Package)
	{
		Write-Step "Building the release package..."
		& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $checksDir "make_package.ps1")
	}

	if (-not $RuntimeOnly)
	{
	# -----------------------------------------------------------------------------------------------------------
	Write-Step "A. Build and package (static)"

	$buildLog = Join-Path $pkgRoot "build_game.log"
	if (-not (Test-Path -LiteralPath $buildLog)) { $buildLog = Join-Path $checksDir "stable-build.log" }
	$buildLogText = Read-SharedText $buildLog
	$buildSucceeded = ($buildLogText -match "exit=0") -or (($buildLogText -match "Sonic3AIR\.exe") -and ($buildLogText -notmatch "(?i)error"))
	$sourceFiles = @(Get-ChildItem -Path (Join-Path $pkgRoot "Oxygen\oxygenengine\source"), (Join-Path $pkgRoot "Oxygen\sonic3air\source") -Recurse -Include *.cpp, *.h -File)
	$newestSource = ($sourceFiles | Measure-Object -Property LastWriteTime -Maximum).Maximum
	$exeExists = Test-Path $pkgExe
	$exeTime = if ($exeExists) { (Get-Item $pkgExe).LastWriteTime } else { [DateTime]::MinValue }
	Add-Result "A1" "Engine is built from the current C++ source" ($exeExists -and $buildSucceeded -and ($exeTime -ge $newestSource)) `
		("build log ok: " + $buildSucceeded + ", exe newer than newest source file: " + ($exeTime -ge $newestSource) + " (run with -Build if not)")

	if (-not (Test-Path $zipPath))
	{
		Add-Result "A2" "Release package exists" $false "Sonic3AIR_Online.zip is missing (run with -Package)"
	}
	else
	{
		$zip = [IO.Compression.ZipFile]::OpenRead($zipPath)
		try
		{
			$entries = @{}
			foreach ($entry in $zip.Entries) { $entries[$entry.FullName.Replace('\', '/')] = $entry }
			$prefix = "Sonic3AIR_Online/"
			Add-Result "A2" "Release package exists and can be opened" $true ("" + $entries.Count + " files")

			$packagedExe = $entries[$prefix + "Sonic3AIR.exe"]
			$exeMatches = ($null -ne $packagedExe) -and $exeExists -and ((Get-EntryHashHex $packagedExe) -eq (Get-FileHashHex $pkgExe))
			Add-Result "A3" "Packaged engine is the same file as the build" $exeMatches ""

			$missingScripts = @()
			$scriptFiles = @(Get-ChildItem -Path $pkgScripts -Recurse -File)
			foreach ($file in $scriptFiles)
			{
				$relative = $file.FullName.Substring($pkgScripts.Length + 1).Replace('\', '/')
				$entry = $entries[$prefix + "scripts/" + $relative]
				if ($null -eq $entry -or (Get-EntryHashHex $entry) -ne (Get-FileHashHex $file.FullName)) { $missingScripts += $relative }
			}
			$packagedScriptCount = @($entries.Keys | Where-Object { $_.StartsWith($prefix + "scripts/") }).Count
			Add-Result "A4" "Packaged game scripts are the current scripts" (($missingScripts.Count -eq 0) -and ($packagedScriptCount -eq $scriptFiles.Count)) `
				("" + $scriptFiles.Count + " files, " + $missingScripts.Count + " differ, " + $packagedScriptCount + " packaged")

			$missingMod = @()
			$modFiles = @(Get-ChildItem -Path $sourceMod -Recurse -File)
			foreach ($file in $modFiles)
			{
				$relative = $file.FullName.Substring($sourceMod.Length + 1).Replace('\', '/')
				$entry = $entries[$prefix + "bundled_mods/DropInCoop/" + $relative]
				if ($null -eq $entry -or (Get-EntryHashHex $entry) -ne (Get-FileHashHex $file.FullName)) { $missingMod += $relative }
			}
			Add-Result "A5" "Packaged co-op mod is the current DropInCoop" ($missingMod.Count -eq 0) ("" + $modFiles.Count + " files, " + $missingMod.Count + " differ")

			$cacheEntries = @($entries.Keys | Where-Object { $_ -match '/saves/|scripts\.bin$|/states/' })
			Add-Result "A6" "No saves, script caches or save states in the package" ($cacheEntries.Count -eq 0) ($cacheEntries -join ", ")

			$romLike = @($entries.Values | Where-Object { $_.FullName -match '(?i)sonic_knuckles|sonic_3_and|\.(md|gen|smd)$' -or (($_.FullName -match '\.bin$') -and ($_.Length -gt 512KB)) })
			$engineDataOk = ($null -ne $entries[$prefix + "data/oxygen_icon.png"]) -and ($null -ne $entries[$prefix + "data/shader"] -or @($entries.Keys | Where-Object { $_ -like ($prefix + "data/shader/*") }).Count -gt 0)
			Add-Result "A11" "Package contains the engine data the game needs at start" $engineDataOk "data/oxygen_icon.png and data/shader must be present"
			Add-Result "A7" "No game ROM in the package" ($romLike.Count -eq 0) (($romLike | ForEach-Object { $_.FullName }) -join ", ")

			$configEntry = $entries[$prefix + "config.json"]
			$configText = if ($configEntry) { Get-EntryText $configEntry } else { "" }
			$configOk = ($configText -match '"StartPhase": "2"') -and -not ($configText -match '(?m)^\s*"LoadSaveState"') -and -not ($configText -match '(?m)^\s*"LoadLevel"')
			Add-Result "A8" "Packaged config starts the normal way (no test start)" $configOk ""

			$readmeEntry = $entries[$prefix + "README_ONLINE.txt"]
			$readmeText = if ($readmeEntry) { Get-EntryText $readmeEntry } else { "" }
			$readmeOk = ($readmeText -match "Host \(works anywhere\)") -and ($readmeText -match "online_log\.txt")
			Add-Result "A9" "Readme explains the join code and the log file" $readmeOk ""

			$testLeftovers = @($entries.Keys | Where-Object { $_ -match 'ChecksHarness|ActHarness|BossHarness|TeleportHarness|PartyHarness|NetPartyHarness|_devtools|DropInTest' })
			Add-Result "A10" "No developer test mods in the package" ($testLeftovers.Count -eq 0) ($testLeftovers -join ", ")

				$markerEntries = @($entries.Keys | Where-Object { $_ -match '(?i)testhooks\.enabled' })
				Add-Result "A12" "The package does not contain the developer test-hooks marker file" ($markerEntries.Count -eq 0) ($markerEntries -join ", ")

				$labelCommonEntry = $entries[$prefix + "bundled_mods/DropInCoop/scripts/common.lemon"]
				$labelCommonText = if ($labelCommonEntry) { Get-EntryText $labelCommonEntry } else { "" }
				Add-Result "A15" "Package: DropIn.drawPlayerLabel takes signed positions (a label partly off the left edge keeps its negative x in the stock-engine fallback instead of becoming 65xxx)" ($labelCommonText -match 'function\s+void\s+DropIn\.drawPlayerLabel\(u8 slot, s16 x, s16 y,') "bundled_mods/DropInCoop/scripts/common.lemon"
		}
		finally
		{
			$zip.Dispose()
		}
	}

	# -----------------------------------------------------------------------------------------------------------
	Write-Step "B. Scripts (static)"

	$modScripts = @(Get-ChildItem -Path (Join-Path $sourceMod "scripts") -Filter *.lemon -File)
	$unbalanced = @()
	foreach ($file in $modScripts)
	{
		# Strings and comments are removed first, so that brackets inside them do not count
		$text = Get-Content $file.FullName -Raw
		$text = [regex]::Replace($text, '"(?:\\.|[^"\\\r\n])*"', '""')
		$text = [regex]::Replace($text, '(?s)/\*.*?\*/', '')
		$text = [regex]::Replace($text, '//[^\r\n]*', '')
		$parens = ($text.ToCharArray() | Where-Object { $_ -eq '(' }).Count - ($text.ToCharArray() | Where-Object { $_ -eq ')' }).Count
		$braces = ($text.ToCharArray() | Where-Object { $_ -eq '{' }).Count - ($text.ToCharArray() | Where-Object { $_ -eq '}' }).Count
		if ($parens -ne 0 -or $braces -ne 0) { $unbalanced += ($file.Name + " (parentheses " + $parens + ", braces " + $braces + ")") }
	}
	Add-Result "B1" "Co-op scripts have balanced brackets" ($unbalanced.Count -eq 0) ($unbalanced -join "; ")

	$hookHits = @(Select-String -Path (Join-Path $sourceMod "scripts\*.lemon") -Pattern 'test\.frame|S3AIR_|Checks\.report' | ForEach-Object { $_.Filename + ":" + $_.LineNumber })
	Add-Result "B2" "Co-op scripts contain no test hooks" ($hookHits.Count -eq 0) ($hookHits -join ", ")
	$signatures = @{}
	foreach ($file in $modScripts) {
		foreach ($definition in [regex]::Matches((Get-Content -Raw $file.FullName), '(?m)^function\s+\w+\s+([\w.]+)\(([^)]*)\)')) {
			$types = @($definition.Groups[2].Value -split ',' | ForEach-Object { ($_.Trim() -split '\s+')[0] }) -join ','
			$key = $definition.Groups[1].Value + '(' + $types + ')'
			if (-not $signatures.ContainsKey($key)) { $signatures[$key] = @() }
			$signatures[$key] += $file.Name
		}
	}
	$duplicates = @($signatures.Keys | Where-Object { $signatures[$_].Count -gt 1 })
	Add-Result 'B3' 'Each mod override has exactly one implementation per signature' ($duplicates.Count -eq 0) ($duplicates -join ', ')
	}
	else
	{
		Write-Host "Runtime-only audit: static package and source checks skipped; using $game" -ForegroundColor DarkGray
	}

	# -----------------------------------------------------------------------------------------------------------
	if (-not (Test-Path $exe))
	{
		throw "No engine build found. Run checks\run_checks.bat -Build first."
	}

	# Make sure the game uses the current co-op mod: the game installs the copy from its own folder at each start
	$binMod = Join-Path $bin "bundled_mods\DropInCoop"
	if (Test-Path $binMod) { Remove-Item $binMod -Recurse -Force }
	New-Item -ItemType Directory -Path (Split-Path $binMod) -Force | Out-Null
	Copy-Item $sourceMod $binMod -Recurse

	# Save the player's own setup, the checks change it and put it back afterwards
	$savedMods = Join-Path $appdata "mods\active-mods.json"
	$savedModsText = if (Test-Path $savedMods) { Read-SharedText $savedMods } else { $null }
	$savedConfigText = Read-SharedText (Join-Path $game "config.json")
	$configPath = Join-Path $game "config.json"
	@{ Config=$savedConfigText; Mods=$savedModsText; HadMods=($null -ne $savedModsText) } | ConvertTo-Json | Set-Content -LiteralPath $recoveryPath -Encoding utf8
	Set-Content -LiteralPath $testHooksMarker -Value 'Developer test hooks are enabled while a check run is active. Created and removed by checks\run_checks.ps1.' -Encoding ascii

	try
	{
		if (-not $RuntimeOnly)
		{
		# -------------------------------------------------------------------------------------------------------
		Write-Step "C. Engine self test (join codes)"

		Set-ActiveMods @("DropInCoop")
		if (Test-Path $selfTestFile) { Remove-Item $selfTestFile -Force }
		[void](Start-GameProcess @{ "S3AIR_SELFTEST" = "1" })
		$finished = Wait-Until { (Read-SharedText $selfTestFile) -match "SELFTEST COMPLETE" } 90
		Stop-StartedProcesses
		if (-not $finished)
		{
			Add-Result "C1" "Engine self test ran" $false "no result file after 90 seconds"
		}
		else
		{
			$selfLines = @((Read-SharedText $selfTestFile) -split "`r?`n" | Where-Object { $_ -match '^(PASS|FAIL) ' })
			$selfNumber = 0
			foreach ($line in $selfLines)
			{
				$selfNumber++
				$passed = $line.StartsWith("PASS ")
				$text = $line.Substring(5)
				Add-Result ("C" + $selfNumber) $text.Split(":")[0] $passed $(if ($text.Contains(":")) { $text.Substring($text.IndexOf(":") + 1).Trim() } else { "" })
			}
			if ($selfLines.Count -eq 0) { Add-Result "C1" "Engine self test produced results" $false "" }
		}
		}
		else
		{
			Write-Host 'Stable fork runtime mode: engine self-test hook not present in this upstream base; code-level tests run separately.' -ForegroundColor DarkGray
		}

		if ($Quick)
		{
			Write-Host ""
			Write-Host "Quick run: the game and online checks (L, U, D, E, F, M, N, R) were skipped." -ForegroundColor Yellow
		}
		else
		{
			# ---------------------------------------------------------------------------------------------------
			# ---------------------------------------------------------------------------------------------------
			if ($Only -contains "L")	# The lobby is part of the online fork only: run it with the stable engine, see run_online_backend_checks.ps1
			{
			Write-Step "L. Online lobby: a friend joins by the code and presses Ready, the host sees it"

			$lobby = Invoke-OnlineJoin (New-SessionCode) @{ "S3AIR_LOBBY_AUTOREADY" = "1" } 5 "L1"
			$hostSawReady = $lobby.Log -match "lobby: player 2 is ready"
			$hostAllReady = $lobby.Log -match "lobby: all players are ready"
			$friendSawLobby = $lobby.Log -match "lobby: you are slot 2"
			Add-Result "L1" "The friend joins and the host sees the Ready press" ($lobby.Reached -and $hostSawReady) ("joined: " + $lobby.Reached + ", host saw ready: " + $hostSawReady)
			Add-Result "L2" "The host knows when every friend is ready" $hostAllReady ""
			Add-Result "L3" "The friend receives the lobby list from the host" $friendSawLobby ""

			# The same through the lobby menu of the released package: two copies start at the main menu, one creates a lobby, one joins it with the code
			$lobbyDir = Join-Path ([IO.Path]::GetTempPath()) ("s3air_lobbycheck_" + [Guid]::NewGuid().ToString("N").Substring(0, 8))
			try
			{
				Stop-StartedProcesses
				Add-Type -AssemblyName System.IO.Compression.FileSystem
				Add-Type -AssemblyName System.Drawing
				[IO.Compression.ZipFile]::ExtractToDirectory($zipPath, $lobbyDir)
				$lobbyExe = Get-ChildItem -LiteralPath $lobbyDir -Filter "Sonic3AIR.exe" -Recurse -File | Select-Object -First 1
				Set-ActiveMods @('DropInCoop')
				Remove-Item $hostLogFile, $friendLogFile -ErrorAction SilentlyContinue
				$uiCode = New-SessionCode

				function Start-LobbyInstance([hashtable]$vars)
				{
					foreach ($key in $vars.Keys) { [Environment]::SetEnvironmentVariable($key, [string]$vars[$key], "Process") }
					try { $proc = Start-Process -FilePath $exe -WorkingDirectory $lobbyExe.DirectoryName -PassThru }		# The engine file that is allowed through the firewall, with the unzipped package's data
					finally { foreach ($key in $vars.Keys) { [Environment]::SetEnvironmentVariable($key, $null, "Process") } }
					[void]$startedProcesses.Add($proc)
					return $proc
				}
				Add-Type -TypeDefinition 'using System; using System.Runtime.InteropServices; public class LobbyWin { [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r); [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr h, int x, int y, int w, int hgt, bool repaint); [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; } }' -ErrorAction SilentlyContinue
				$uiHost = Start-LobbyInstance @{ "S3AIR_LOBBY_OPEN" = "hostlan"; "S3AIR_LOBBY_CODE" = $uiCode; "S3AIR_LOGNAME" = "_host" }
				Start-Sleep -Seconds 12
				$uiHost.Refresh(); [void][LobbyWin]::MoveWindow($uiHost.MainWindowHandle, 0, 0, 760, 480, $true)
				$uiFriend = Start-LobbyInstance @{ "S3AIR_LOBBY_OPEN" = "join"; "S3AIR_LOBBY_CODE" = $uiCode; "S3AIR_LOBBY_AUTOREADY" = "1"; "S3AIR_LOGNAME" = "_friend" }
				Start-Sleep -Seconds 8
				$uiFriend.Refresh(); [void][LobbyWin]::MoveWindow($uiFriend.MainWindowHandle, 780, 0, 760, 480, $true)
				$uiReady = Wait-Until { (Read-SharedText $hostLogFile) -match "lobby: all players are ready" } 90
				Start-Sleep -Seconds 4

				# Pictures of both lobby screens (of the windows, as a player sees them)
				Add-Type -TypeDefinition 'using System; using System.Runtime.InteropServices; public class LobbyWin { [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r); [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr h, int x, int y, int w, int hgt, bool repaint); [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; } }' -ErrorAction SilentlyContinue
				foreach ($pair in @(@($uiHost, "lobby-host.png"), @($uiFriend, "lobby-friend.png")))
				{
					try
					{
						$pair[0].Refresh()
						$rect = New-Object LobbyWin+RECT
						[void][LobbyWin]::GetWindowRect($pair[0].MainWindowHandle, [ref]$rect)
						$bitmap = New-Object System.Drawing.Bitmap ($rect.R - $rect.L), ($rect.B - $rect.T)
						$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
						$graphics.CopyFromScreen($rect.L, $rect.T, 0, 0, $bitmap.Size)
						$bitmap.Save((Join-Path (Join-Path $checksDir "last_logs") $pair[1]))
						$graphics.Dispose(); $bitmap.Dispose()
					}
					catch { }
				}

				$uiHostLog = Read-SharedText $hostLogFile
				$uiFriendLog = Read-SharedText $friendLogFile
				Add-Result "L4" "Through the lobby menu: the friend joins the host's lobby with the code" (($uiHostLog -match "lobby: player 2 is ready") -and ($uiFriendLog -match "lobby: you are slot 2")) ""
				Add-Result "L5" "Through the lobby menu: the host sees that everybody is ready" $uiReady ""
			}
			catch
			{
				Add-Result "L4" "Through the lobby menu: the friend joins the host's lobby with the code" $false $_.Exception.Message
			}
			finally
			{
				Stop-StartedProcesses
				if ((Split-Path -Leaf $lobbyDir) -like "s3air_lobbycheck_*" -and (Test-Path -LiteralPath $lobbyDir)) { Remove-Item -LiteralPath $lobbyDir -Recurse -Force -ErrorAction SilentlyContinue }
			}
			}

			# ---------------------------------------------------------------------------------------------------
			if (($Only -contains 'L') -or ($Only -contains 'L2'))
			{
			Write-Step 'L2. Lobby features: names, ping, chat, input delay, reconnect to the slot, kick, ready check (host and two friends, one computer)'

			Set-ActiveMods @('DropInCoop')
			Stop-StartedProcesses
			$lastLogs = Join-Path $checksDir 'last_logs'
			New-Item -ItemType Directory -Path $lastLogs -Force | Out-Null
			$l2Code = New-SessionCode
			foreach ($suffix in @('_lhost', '_lf1', '_lf2')) { Remove-Item (Join-Path $appdata ("logfile" + $suffix + ".txt")) -ErrorAction SilentlyContinue }
			Add-Type -AssemblyName System.Drawing
			Add-Type -TypeDefinition 'using System; using System.Runtime.InteropServices; public class LobbyWin2 { [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r); [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr h, int x, int y, int w, int hgt, bool repaint); [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd); [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; } }' -ErrorAction SilentlyContinue

			function Save-WindowPicture([System.Diagnostics.Process]$process, [string]$path, [System.Diagnostics.Process[]]$others)
			{
				# The other game windows are minimized for a moment, so that the one that is photographed is really in front
				foreach ($other in $others) { $other.Refresh(); [void][LobbyWin2]::ShowWindow($other.MainWindowHandle, 7) }
				Start-Sleep -Milliseconds 700
				$process.Refresh()
				[void][LobbyWin2]::ShowWindow($process.MainWindowHandle, 9)
				Start-Sleep -Milliseconds 700
				[void](Save-GameScreenshot $process $path)
				foreach ($other in $others) { [void][LobbyWin2]::ShowWindow($other.MainWindowHandle, 4) }
			}

			try
			{
				# The host: opens the lobby, sets a fixed input delay of 3 after a few seconds, kicks player 2 (slot index 1) after 45 seconds,
				# presses START GAME after 55 seconds (all counted from the moment the first friend is in), with a short ready check of 10 seconds
				$l2Host = Start-GameProcess @{ 'S3AIR_LOBBY_OPEN' = 'hostlan'; 'S3AIR_LOBBY_CODE' = $l2Code; 'S3AIR_LOGNAME' = '_lhost'; 'S3AIR_PLAYER_NAME' = 'Hosty'; 'S3AIR_LOBBY_AUTODELAY' = '3@8'; 'S3AIR_LOBBY_AUTOKICK' = '1@45'; 'S3AIR_LOBBY_AUTOSTART' = '55'; 'S3AIR_LOBBY_READY_TIMEOUT' = '10' }
				Start-Sleep -Seconds 14
				# Friend one: a normal name, presses Ready, says a quick message, then tries to flood the chat
				$l2F1 = Start-GameProcess @{ 'S3AIR_LOBBY_OPEN' = 'join'; 'S3AIR_LOBBY_CODE' = $l2Code; 'S3AIR_LOGNAME' = '_lf1'; 'S3AIR_PLAYER_NAME' = 'Friend One'; 'S3AIR_LOBBY_AUTOREADY' = '1'; 'S3AIR_LOBBY_CHAT' = '5|'; 'S3AIR_LOBBY_CHAT_SPAM' = '1'; 'S3AIR_LOBBY_AUTORETRY' = '1' }
				$f1In = Wait-Until { (Get-NetLog '_lhost') -match 'a friend proved the code and is player 2' } 90
				Start-Sleep -Seconds 4
				# Friend two: a name full of forbidden characters and too long, never presses Ready, writes a free text with markup, and loses its connection once
				$l2F2 = Start-GameProcess @{ 'S3AIR_LOBBY_OPEN' = 'join'; 'S3AIR_LOBBY_CODE' = $l2Code; 'S3AIR_LOGNAME' = '_lf2'; 'S3AIR_PLAYER_NAME' = 'Bad<Name>|XY 12345678901234'; 'S3AIR_LOBBY_CHAT' = '0|Hi <b>there</b> :)'; 'S3AIR_NETPLAY_LOBBY_DROP_TEST' = '1'; 'S3AIR_LOBBY_AUTORETRY' = '1' }
				$f2In = Wait-Until { (Get-NetLog '_lhost') -match 'a friend proved the code and is player 3' } 90
				$backInSlot = Wait-Until { (Get-NetLog '_lf2') -match 'reconnected to the lobby, back in player slot 3' } 90
				Start-Sleep -Seconds 4

				# Pictures of the host's and the second friend's lobby (names, ping, chat, input delay)
				Start-Sleep -Seconds 2
				Save-WindowPicture $l2Host (Join-Path $lastLogs 'L2-lobby-host.png') @($l2F1, $l2F2)
				Save-WindowPicture $l2F2 (Join-Path $lastLogs 'L2-lobby-friend2.png') @($l2Host, $l2F1)

				# The kick, then the ready check that ends without the friend who never pressed Ready
				$kicked = Wait-Until { (Get-NetLog '_lf1') -match 'Netplay failure: KICKED' } 120
				Start-Sleep -Seconds 2
				Save-WindowPicture $l2F1 (Join-Path $lastLogs 'L2-kicked-friend1.png') @($l2Host, $l2F2)
				$readyDone = Wait-Until { (Get-NetLog '_lhost') -match 'ready check timed out, starting without' } 120
				$leftOut = Wait-Until { (Get-NetLog '_lf2') -match 'Netplay failure: LEFT_OUT' } 30
				Start-Sleep -Seconds 2
				Save-WindowPicture $l2F2 (Join-Path $lastLogs 'L2-leftout-friend2.png') @($l2Host, $l2F1)
				# The player who was left out presses Retry (a test switch does it eight seconds later): nothing blocks it, it gets a slot again
				$cameBack = Wait-Until { ([regex]::Matches((Get-NetLog '_lhost'), 'a friend proved the code and is player 3')).Count -ge 3 } 60

				$hostText = Get-NetLog '_lhost'
				$f1Text = Get-NetLog '_lf1'
				$f2Text = Get-NetLog '_lf2'
				foreach ($pair in @(@('_lhost', 'L2-host'), @('_lf1', 'L2-friend1'), @('_lf2', 'L2-friend2'))) { Copy-Item (Join-Path $appdata ("logfile" + $pair[0] + ".txt")) (Join-Path $lastLogs ($pair[1] + '.txt')) -Force -ErrorAction SilentlyContinue }

				Add-Result 'L6' 'Both friends join the lobby' ($f1In -and $f2In) ("friend 1 in: $f1In, friend 2 in: $f2In")
				Add-Result 'L7' 'Player names: sent to the host, cleaned up (no markup, 12 letters at most), shown to everybody' (($hostText -match 'player 2 is called "Friend One"') -and ($hostText -match 'player 3 is called "BadNameXY 12"') -and ($f1Text -match 'names Hosty,Friend One,BadNameXY 12,') -and -not ($hostText -match 'is called "[^"]*[<>|]')) ('host knows: ' + (([regex]::Matches($hostText, 'player \d is called "[^"]*"') | ForEach-Object { $_.Value }) -join '; '))
				$pingLines = @([regex]::Matches($hostText, 'ping of player 2 \(Friend One\) is (\d+) ms') | ForEach-Object { [int]$_.Groups[1].Value })
				Add-Result 'L8' 'Ping: the host measures a round trip per friend, the friend gets its own number from the host' (($pingLines.Count -ge 1) -and (($pingLines | Measure-Object -Maximum).Maximum -lt 400) -and ($f1Text -match 'the host measured our ping: \d+ ms')) ('host samples (ms): ' + ($pingLines -join ','))
				$spamDropped = ([regex]::Matches($hostText, 'chat of player 2 dropped, too fast')).Count
				$p2Chats = ([regex]::Matches($hostText, 'chat, player 2 \(Friend One\): ')).Count
				Add-Result 'L9' 'Chat: a quick message and a free text arrive cleaned up for everybody; a flood from one player is dropped by the host' (($hostText -match 'chat, player 2 \(Friend One\): Good luck!') -and ($f1Text -match 'chat, player 2 \(Friend One\): Good luck!') -and ($hostText -match 'chat, player 3 \(BadNameXY 12\): Hi bthereb :\)') -and ($f2Text -match 'chat, player 3 \(BadNameXY 12\): Hi bthereb :\)') -and ($spamDropped -ge 4) -and ($p2Chats -le 3)) ("flood lines dropped: $spamDropped, accepted from player 2: $p2Chats")
				Add-Result 'L10' 'Input delay: the host sets 3 frames in the lobby, everybody sees it' (($hostText -match 'input delay setting is now 3 frames') -and ($f1Text -match 'input delay 3 frames') -and ($f2Text -match 'input delay 3 frames')) ''
				Add-Result 'L11' 'Reconnect: a friend whose connection drops comes back to the same player slot with its name' ($backInSlot -and ($f2Text -match 'automated check dropping the lobby connection') -and ($f2Text -match 'lost the lobby connection, trying to come back to player slot 3') -and ($hostText -match 'friend came back.*player slot 3') -and ([regex]::Matches($hostText, 'player 3 is called "BadNameXY 12"').Count -ge 2)) ("back in slot 3: $backInSlot")
				$retryRefused = (([regex]::Matches($f1Text, 'Netplay failure: KICKED')).Count -ge 2) -and ($hostText -match 'refused .* the host removed this player a moment ago')
				Add-Result 'L12' 'Kick: the host removes a player, who is told so (no Retry button) and cannot come back right away' ($kicked -and ($hostText -match 'kicked player 2 \(Friend One\)') -and ($f1Text -match 'showing the problem to the player \(KICKED\) without a Retry button') -and $retryRefused) ("told: $kicked, a retry a few seconds later was refused: $retryRefused")
				Add-Result 'L14' 'A player who was left out can press Retry and is in the lobby again (the ready check is not a kick)' ($cameBack -and (([regex]::Matches((Get-NetLog '_lf2'), 'Netplay failure: LEFT_OUT')).Count -eq 1)) ("back in the lobby: $cameBack")
				Add-Result 'L13' 'Ready check: the host starts without waiting forever; the player who is not ready gets a prompt and is left out' ($readyDone -and ($hostText -match 'ready check started, 10 seconds') -and ($f2Text -match 'the host wants to start, ready check running') -and ($hostText -match 'player 3 \(BadNameXY 12\) was not ready and is left out') -and $leftOut) ("timed out: $readyDone, left out told: $leftOut")
			}
			catch
			{
				Add-Result 'L6' 'Lobby features run' $false $_.Exception.Message
			}
			finally
			{
				Stop-StartedProcesses
			}
			}

			# ---------------------------------------------------------------------------------------------------
			if (Wants 'W')
			{
			Write-Step 'W. Bad internet, simulated: a lag proxy between host and friend (200 ms round trip, 30 ms jitter, 3 percent loss), with and without input delay'

			Set-ActiveMods @('DropInCoop')
			$lastLogs = Join-Path $checksDir 'last_logs'
			New-Item -ItemType Directory -Path $lastLogs -Force | Out-Null
			$proxyScript = Join-Path $checksDir 'udp_lag_proxy.ps1'
			$wRtt = 200; $wJitter = 30; $wLoss = 3

			function Read-ProxyStats([string]$path)
			{
				$stats = @{}
				foreach ($line in @((Read-SharedText $path) -split "`r?`n")) { if ($line -match '^(\w+)=(.*)$') { $stats[$Matches[1]] = $Matches[2] } }
				return $stats
			}

			foreach ($run in @(@{ Id = 'W1'; Delay = '0'; Title = 'input delay off' }, @{ Id = 'W2'; Delay = 'auto'; Title = 'input delay Auto' }, @{ Id = 'W3'; Delay = '4'; Title = 'input delay fixed at 4 frames' }))
			{
				$tag = $run.Id
				try
				{
					Stop-StartedProcesses
					foreach ($suffix in @('_whost', '_wfriend')) { Remove-Item (Join-Path $appdata ("logfile" + $suffix + ".txt")), (Join-Path $appdata ("online_log" + $suffix + ".txt")) -ErrorAction SilentlyContinue }
					$statsPath = Join-Path $lastLogs ('W-proxy-' + $tag + '.txt')
					Remove-Item -LiteralPath $statsPath -ErrorAction SilentlyContinue
					$wSecret = New-Secret

					# The friend connects to the proxy port 28841; the proxy passes everything on to the host at 28840 (delayed, jittered, with loss)
					$proxyArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', ('"' + $proxyScript + '"'), '-ListenPort', '28841', '-TargetPort', '28840', '-RttMs', $wRtt, '-JitterMs', $wJitter, '-LossPercent', $wLoss, '-StatsFile', ('"' + $statsPath + '"'), '-Seed', '4242', '-Seconds', '400')
					$proxyProcess = Start-Process -FilePath 'powershell.exe' -ArgumentList $proxyArgs -PassThru -WindowStyle Hidden
					[void]$startedProcesses.Add($proxyProcess)
					[void](Wait-Until { (Read-SharedText $statsPath) -match 'uptime_s' } 30)

					$wHostP = Start-GameProcess @{ 'S3AIR_NETPLAY' = "hostdirect:$wSecret"; 'S3AIR_LOGNAME' = '_whost'; 'S3AIR_NETPLAY_DELAY' = $run.Delay; 'S3AIR_NETPLAY_DELAYLOG' = '1'; 'S3AIR_PLAYER_NAME' = $(if ($tag -eq 'W2') { 'Hosty' } else { '' }) }
					Start-Sleep -Seconds 12
					$wFriendP = Start-GameProcess @{ 'S3AIR_NETPLAY' = ("joindirect:" + $wSecret + ":28841"); 'S3AIR_LOGNAME' = '_wfriend'; 'S3AIR_NETPLAY_TEST_INPUT' = '0x1'; 'S3AIR_NETPLAY_TEST_TOGGLE' = '1'; 'S3AIR_PLAYER_NAME' = $(if ($tag -eq 'W2') { 'Laggy' } else { '' }) }
					$wReached = Wait-Until { (@((Read-SharedText (Join-Path $appdata 'online_log_wfriend.txt')) -split "`r?`n" | Where-Object { $_ -match '^\[friend\].* state=5 ' })).Count -gt 0 } 120
					if ($wReached) { Start-Sleep -Seconds 40 }
					if ($wReached -and $tag -eq 'W2')
					{
						# Pictures of the game screens: the labels under the screen show the players' names (the host is called Hosty, the friend keeps the default)
						Add-Type -TypeDefinition 'using System; using System.Runtime.InteropServices; public class GameWin4 { [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd); }' -ErrorAction SilentlyContinue
						foreach ($shot in @(@($wHostP, 'W-game-host.png', $wFriendP), @($wFriendP, 'W-game-friend.png', $wHostP)))
						{
							$shot[2].Refresh(); [void][GameWin4]::ShowWindow($shot[2].MainWindowHandle, 7); Start-Sleep -Milliseconds 700
							$shot[0].Refresh(); [void][GameWin4]::ShowWindow($shot[0].MainWindowHandle, 9); Start-Sleep -Milliseconds 700
							[void](Save-GameScreenshot $shot[0] (Join-Path $lastLogs $shot[1]))
							[void][GameWin4]::ShowWindow($shot[2].MainWindowHandle, 4)
						}
						Start-Sleep -Seconds 5
					}
					elseif ($wReached) { Start-Sleep -Seconds 5 }

					$wHostLog = Get-NetLog '_whost'
					$wFriendLog = Get-NetLog '_wfriend'
					$wFriendLines = @((Read-SharedText (Join-Path $appdata 'online_log_wfriend.txt')) -split "`r?`n" | Where-Object { $_ -match '^\[friend\]' })
					$wHostLines = @((Read-SharedText (Join-Path $appdata 'online_log_whost.txt')) -split "`r?`n" | Where-Object { $_ -match '^\[host\]' })
					Copy-Item (Join-Path $appdata 'logfile_whost.txt') (Join-Path $lastLogs ('W-' + $tag + '-host.txt')) -Force -ErrorAction SilentlyContinue
					Copy-Item (Join-Path $appdata 'logfile_wfriend.txt') (Join-Path $lastLogs ('W-' + $tag + '-friend.txt')) -Force -ErrorAction SilentlyContinue
					Set-Content -Path (Join-Path $lastLogs ('W-' + $tag + '-online-lines.txt')) -Value ($wHostLines + $wFriendLines) -Encoding utf8
					$stats = Read-ProxyStats $statsPath
					Stop-StartedProcesses

					# Friend: frames per second while in the game (the first samples are the join itself and skipped), repairs, sync progress
					$inGame = @($wFriendLines | Where-Object { $_ -match ' state=5 ' })
					$fpsValues = @()
					for ($i = 2; $i -lt $inGame.Count; $i++) { $m = [regex]::Match($inGame[$i], 'fps=([\d.]+)'); if ($m.Success) { $fpsValues += [double]$m.Groups[1].Value } }
					$fpsMin = if ($fpsValues.Count -gt 0) { ($fpsValues | Measure-Object -Minimum).Minimum } else { 0 }
					$fpsAvg = if ($fpsValues.Count -gt 0) { ($fpsValues | Measure-Object -Average).Average } else { 0 }
					$repairs = 0
					foreach ($line in $wFriendLines) { $m = [regex]::Match($line, 'repairs=(\d+)'); if ($m.Success -and [int]$m.Groups[1].Value -gt $repairs) { $repairs = [int]$m.Groups[1].Value } }
					$syncFrames = @($inGame | ForEach-Object { $m = [regex]::Match($_, 'last sync ok at frame (\d+)'); if ($m.Success) { [int]$m.Groups[1].Value } })
					$syncProgress = if ($syncFrames.Count -ge 2) { ($syncFrames | Measure-Object -Maximum).Maximum - ($syncFrames | Measure-Object -Minimum).Minimum } else { 0 }
					$aborted = $wFriendLog -match 'giving up|aborting' -or $wHostLog -match 'aborting'

					# Host: measured ping and input delay in its own online log
					$pings = @(); $delays = @()
					foreach ($line in $wHostLines) { $m = [regex]::Match($line, 'inputdelay=(\d+), ping=(-?\d+)'); if ($m.Success) { $delays += [int]$m.Groups[1].Value; if ([int]$m.Groups[2].Value -ge 0) { $pings += [int]$m.Groups[2].Value } } }
					$pingLast = if ($pings.Count -gt 0) { $pings[$pings.Count - 1] } else { -1 }
					$delayLast = if ($delays.Count -gt 0) { $delays[$delays.Count - 1] } else { -1 }

					$c2s = [int64]($stats['c2s_forwarded']); $s2c = [int64]($stats['s2c_forwarded']); $dropped = [int64]($stats['c2s_dropped']) + [int64]($stats['s2c_dropped'])
					$delayAvg = [double]($stats['delay_avg_ms'])
					$lossShare = if (($c2s + $s2c + $dropped) -gt 0) { 100.0 * $dropped / ($c2s + $s2c + $dropped) } else { 0 }

					Add-Result ($tag + 'a') ("Through the lag proxy ($($run.Title)): the friend joins and plays, no abort") ($wReached -and -not $aborted) ("in game: $wReached, aborted: $aborted")
					$userNameInLog = ($wFriendLog -match ('names [^\r\n]*' + [regex]::Escape($env:USERNAME)))
					Add-Result ($tag + 'h') ("Names in the lobby message: default is just Player (not the Windows user name) when none is set, the set names otherwise ($($run.Title))") (($wFriendLog -match $(if ($tag -eq 'W2') { 'names Hosty,Laggy,' } else { 'names Player,Player,' })) -and -not $userNameInLog) ("friend log names line: " + [regex]::Match($wFriendLog, 'names [^\r\n]*').Value)
					Add-Result ($tag + 'b') ("The traffic really went through the proxy with the set delay and loss ($($run.Title))") (($c2s -gt 500) -and ($s2c -gt 500) -and ($delayAvg -gt 85) -and ($delayAvg -lt 115) -and ($lossShare -gt 1.0) -and ($lossShare -lt 6.0)) ("friend to host $c2s datagrams, host to friend $s2c, dropped $dropped (" + [math]::Round($lossShare, 1) + " percent), average added one-way delay " + $delayAvg + " ms")
					Add-Result ($tag + 'c') ("Game stays in sync with 200 ms, jitter and loss ($($run.Title)): no repairs, the sync check keeps passing") (($repairs -eq 0) -and ($syncProgress -ge 600)) ("repairs: $repairs, sync check progressed by $syncProgress frames")
					Add-Result ($tag + 'd') ("Usable frame rate for the friend ($($run.Title)): lowest 30 fps or more, average 45 or more") (($fpsValues.Count -ge 4) -and ($fpsMin -ge 30) -and ($fpsAvg -ge 45)) ("samples: " + $fpsValues.Count + ", lowest " + [math]::Round($fpsMin, 1) + ", average " + [math]::Round($fpsAvg, 1))
					Add-Result ($tag + 'e') ("The host measured the round trip (about 200 ms) and warns about the high ping ($($run.Title))") (($pingLast -ge 150) -and ($pingLast -le 330) -and ($wHostLog -match 'WARNING player 2 .* has a high ping')) ("last measured ping: $pingLast ms")
					$delayOk = switch ($run.Delay) { '0' { ($delayLast -eq 0) -and (@($delays | Where-Object { $_ -ne 0 }).Count -eq 0) } 'auto' { ($delayLast -ge 4) -and ($delayLast -le 6) } default { ($delayLast -eq 4) } }
					Add-Result ($tag + 'f') ("Input delay in use is what was chosen ($($run.Title))") $delayOk ("host input delay samples: " + ($delays -join ','))

					# The delay is real: the friend's button changes every two seconds. The host logs when a change arrives and when it enters the game.
					# Each change must enter the game exactly as many frames later as the delay that was in effect.
					$rawChanges = @(); $usedChanges = @()
					foreach ($m in [regex]::Matches($wHostLog, 'Input delay log: frame (\d+) player 2 (raw|used) (\d+) delay (\d+)'))
					{
						$entry = [pscustomobject]@{ Frame = [int]$m.Groups[1].Value; Value = [int]$m.Groups[3].Value; Delay = [int]$m.Groups[4].Value }
						if ($m.Groups[2].Value -eq 'raw') { $rawChanges += $entry } else { $usedChanges += $entry }
					}
					$gaps = @()
					for ($i = 2; $i -lt [math]::Min($rawChanges.Count, $usedChanges.Count); $i++)
					{
						if ($rawChanges[$i].Value -eq $usedChanges[$i].Value -and $rawChanges[$i].Delay -eq $usedChanges[$i].Delay) { $gaps += ($usedChanges[$i].Frame - $rawChanges[$i].Frame) }
					}
					$expectedGap = switch ($run.Delay) { '0' { 0 } 'auto' { $delayLast } default { 4 } }
					$gapsOk = ($gaps.Count -ge 5) -and (@($gaps | Where-Object { $_ -ne $expectedGap }).Count -le 1)
					Add-Result ($tag + 'g') ("Each change of the friend's input reaches the game exactly $expectedGap frames after it reached the host ($($run.Title))") $gapsOk ("measured frame gaps: " + ($gaps -join ','))
				}
				catch
				{
					Add-Result ($tag + 'a') ("Lag proxy run ($($run.Title))") $false $_.Exception.Message
				}
				finally
				{
					Stop-StartedProcesses
				}
			}

			# The lobby screens through the proxy: the friend's ping is about 200 ms, the host sees it and is warned, everybody sees the input delay Auto picked
			$wTag = 'W4'
			try
			{
				Stop-StartedProcesses
				Add-Type -AssemblyName System.Drawing
				Add-Type -TypeDefinition 'using System; using System.Runtime.InteropServices; public class LobbyWin3 { [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd); }' -ErrorAction SilentlyContinue
				foreach ($suffix in @('_whost', '_wfriend')) { Remove-Item (Join-Path $appdata ("logfile" + $suffix + ".txt")), (Join-Path $appdata ("online_log" + $suffix + ".txt")) -ErrorAction SilentlyContinue }
				$statsPath = Join-Path $lastLogs 'W-proxy-W4.txt'
				Remove-Item -LiteralPath $statsPath -ErrorAction SilentlyContinue
				$wSecret = New-Secret
				$proxyArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', ('"' + $proxyScript + '"'), '-ListenPort', '28841', '-TargetPort', '28840', '-RttMs', $wRtt, '-JitterMs', $wJitter, '-LossPercent', $wLoss, '-StatsFile', ('"' + $statsPath + '"'), '-Seed', '4243', '-Seconds', '400')
				$proxyProcess = Start-Process -FilePath 'powershell.exe' -ArgumentList $proxyArgs -PassThru -WindowStyle Hidden
				[void]$startedProcesses.Add($proxyProcess)
				[void](Wait-Until { (Read-SharedText $statsPath) -match 'uptime_s' } 30)

				$wHostProc = Start-GameProcess @{ 'S3AIR_NETPLAY' = "hostdirect:$wSecret"; 'S3AIR_LOBBY_OPEN' = 'show'; 'S3AIR_LOGNAME' = '_whost'; 'S3AIR_PLAYER_NAME' = 'Hosty'; 'S3AIR_NETPLAY_DELAY' = 'auto' }
				Start-Sleep -Seconds 12
				$wFriendProc = Start-GameProcess @{ 'S3AIR_NETPLAY' = ("joindirect:" + $wSecret + ":28841"); 'S3AIR_LOBBY_OPEN' = 'show'; 'S3AIR_LOBBY_AUTOREADY' = '1'; 'S3AIR_LOGNAME' = '_wfriend'; 'S3AIR_PLAYER_NAME' = 'Laggy' }
				$w4In = Wait-Until { (Get-NetLog '_whost') -match 'a friend proved the code and is player 2' } 120
				$w4Warned = Wait-Until { (Get-NetLog '_whost') -match 'Lobby: showing the high ping warning for player 2' } 60
				$w4Drawn = Wait-Until { (Get-NetLog '_whost') -match 'Lobby: on screen: high ping warning' } 30		# written by the screen drawing code itself: a warning hidden by another message would not appear
				Start-Sleep -Seconds 4

				$shots = @(@($wHostProc, 'W4-lobby-host.png', @($wFriendProc)), @($wFriendProc, 'W4-lobby-friend.png', @($wHostProc)))
				foreach ($shot in $shots)
				{
					foreach ($other in $shot[2]) { $other.Refresh(); [void][LobbyWin3]::ShowWindow($other.MainWindowHandle, 7) }
					Start-Sleep -Milliseconds 700
					$shot[0].Refresh(); [void][LobbyWin3]::ShowWindow($shot[0].MainWindowHandle, 9)
					Start-Sleep -Milliseconds 700
					[void](Save-GameScreenshot $shot[0] (Join-Path $lastLogs $shot[1]))
					foreach ($other in $shot[2]) { [void][LobbyWin3]::ShowWindow($other.MainWindowHandle, 4) }
				}

				$w4Host = Get-NetLog '_whost'
				$w4Friend = Get-NetLog '_wfriend'
				Copy-Item (Join-Path $appdata 'logfile_whost.txt') (Join-Path $lastLogs 'W4-host.txt') -Force -ErrorAction SilentlyContinue
				Copy-Item (Join-Path $appdata 'logfile_wfriend.txt') (Join-Path $lastLogs 'W4-friend.txt') -Force -ErrorAction SilentlyContinue
				$hostPing = [regex]::Match($w4Host, 'ping of player 2 \(Laggy\) is (\d+) ms')
				$friendPing = [regex]::Match($w4Friend, 'the host measured our ping: (\d+) ms')
				$friendDelay = [regex]::Match($w4Friend, 'input delay AUTO \((\d) frames\)')
				$lastDelay = $null
				foreach ($match in [regex]::Matches($w4Friend, 'input delay AUTO \((\d) frames\)')) { $lastDelay = [int]$match.Groups[1].Value }
				Add-Result 'W4a' 'Lobby through the lag proxy: the friend joins with its name, the host sees the ping per player (about 200 ms) and is warned about the high ping' ($w4In -and $w4Warned -and $w4Drawn -and ($w4Host -match 'player 2 is called "Laggy"') -and $hostPing.Success -and ([int]$hostPing.Groups[1].Value -ge 150) -and ([int]$hostPing.Groups[1].Value -le 330)) ("host measured: " + $hostPing.Groups[1].Value + " ms, warning decided: $w4Warned, warning drawn on screen: $w4Drawn")
				Add-Result 'W4b' 'Lobby through the lag proxy: the friend sees its own ping and the input delay that Auto chose from it (4 to 6 frames)' ($friendPing.Success -and ([int]$friendPing.Groups[1].Value -ge 100) -and ($null -ne $lastDelay) -and ($lastDelay -ge 4) -and ($lastDelay -le 6)) ("friend sees ping " + $friendPing.Groups[1].Value + " ms, input delay Auto = $lastDelay frames")
			}
			catch
			{
				Add-Result 'W4a' 'Lobby through the lag proxy' $false $_.Exception.Message
			}
			finally
			{
				Stop-StartedProcesses
			}
			}

			# ---------------------------------------------------------------------------------------------------
			if (Wants "U")
			{
			Write-Step "U. The release zip, unzipped to a fresh folder and started as a player would start it"

			$unzipDir = Join-Path ([IO.Path]::GetTempPath()) ("s3air_pkgcheck_" + [Guid]::NewGuid().ToString("N").Substring(0, 8))
			try
			{
				if (-not (Test-Path -LiteralPath $zipPath)) { throw "Sonic3AIR_Online.zip is missing (run with -Package)" }
				Add-Type -AssemblyName System.IO.Compression.FileSystem
				[IO.Compression.ZipFile]::ExtractToDirectory($zipPath, $unzipDir)
				$packageExe = Get-ChildItem -LiteralPath $unzipDir -Filter "Sonic3AIR.exe" -Recurse -File | Select-Object -First 1
				if ($null -eq $packageExe) { throw "no Sonic3AIR.exe inside the zip" }
				$packageDir = $packageExe.DirectoryName
				$gameLog = Join-Path $appdata "logfile.txt"
				if (Test-Path -LiteralPath $gameLog) { Remove-Item -LiteralPath $gameLog -Force }

				# No test variables: this is the product as shipped (the engine installs its bundled co-op mod by itself, see U8)
				$packageProcess = Start-Process -FilePath $packageExe.FullName -WorkingDirectory $packageDir -PassThru
				[void]$startedProcesses.Add($packageProcess)
				$ready = Wait-Until { ((Read-SharedText $gameLog) -match "Ready to go") -or $packageProcess.HasExited } 45
				$logText = Read-SharedText $gameLog
				$startedOk = $ready -and ($logText -match "Ready to go") -and -not ($logText -match "(?i)Could not find engine data|Failed to load")
				$title = ""
				if (-not $packageProcess.HasExited) { $packageProcess.Refresh(); $title = $packageProcess.MainWindowTitle }
				Add-Result "U1" "The unzipped package starts without an error message" ($startedOk -and $title -notmatch "(?i)error") ("ready: " + ($logText -match "Ready to go") + ", window: '" + $title + "'")

				$ranFromPackage = $logText -match [regex]::Escape($packageExe.FullName)
				Add-Result "U2" "That run used the unzipped copy, not the build folder" $ranFromPackage ""

				Start-Sleep -Seconds 5
				$stillRunning = -not $packageProcess.HasExited
				Add-Result "U3" "The unzipped package keeps running for 5 seconds after start" $stillRunning ""

				# U8: the bundled co-op mod installs itself (the README promises "built in and switches itself on"). A second, completely fresh unzipped copy gets an empty
				# local save folder (savedata next to the exe makes the game use that instead of %APPDATA%), so it starts like a new player's game: nothing installed.
				# The mod must appear in its mods folder, be switched on, and a later start must repair a stale or damaged copy and remove leftover files.
				$freshDir = Join-Path ([IO.Path]::GetTempPath()) ("s3air_freshcheck_" + [Guid]::NewGuid().ToString("N").Substring(0, 8))
				try
				{
					[IO.Compression.ZipFile]::ExtractToDirectory($zipPath, $freshDir)
					$freshExe = Get-ChildItem -LiteralPath $freshDir -Filter "Sonic3AIR.exe" -Recurse -File | Select-Object -First 1
					$freshGameDir = $freshExe.DirectoryName
					$freshSave = Join-Path $freshGameDir "savedata"
					New-Item -ItemType Directory -Path $freshSave | Out-Null
					$bundledMod = Join-Path $freshGameDir "bundled_mods\DropInCoop"
					$freshMods = Join-Path $freshSave "mods"
					$installedFresh = Join-Path $freshMods "DropInCoop"
					$hashTree = { param($dir) $result = @{}; foreach ($f in Get-ChildItem -LiteralPath $dir -Recurse -File) { $sha = [Security.Cryptography.SHA256]::Create(); try { $result[$f.FullName.Substring($dir.Length).ToLowerInvariant()] = [BitConverter]::ToString($sha.ComputeHash([IO.File]::ReadAllBytes($f.FullName))) } finally { $sha.Dispose() } }; return $result }
					$bundledHashes = & $hashTree $bundledMod

					$freshProcess = Start-Process -FilePath $freshExe.FullName -WorkingDirectory $freshGameDir -PassThru
					[void]$startedProcesses.Add($freshProcess)
					$null = Wait-Until { (Test-Path -LiteralPath (Join-Path $freshMods "active-mods.json")) -or $freshProcess.HasExited } 45
					Start-Sleep -Seconds 2
					Stop-StartedProcesses
					Start-Sleep -Seconds 1
					$installedHashes = if (Test-Path -LiteralPath $installedFresh) { & $hashTree $installedFresh } else { @{} }
					$sameFiles = ($installedHashes.Count -eq $bundledHashes.Count) -and (@($bundledHashes.Keys | Where-Object { $installedHashes[$_] -ne $bundledHashes[$_] }).Count -eq 0)
					$activeText = if (Test-Path -LiteralPath (Join-Path $freshMods "active-mods.json")) { [string](Read-SharedText (Join-Path $freshMods "active-mods.json")) } else { "" }
					Add-Result "U8a" "A fresh unzipped package (no mods installed yet) installs its bundled co-op mod into its mods folder, file for file, and switches it on" ($sameFiles -and ($activeText -match '"DropInCoop"')) ("files: bundled " + $bundledHashes.Count + ", installed " + $installedHashes.Count + ", identical: $sameFiles, active-mods.json: " + $activeText.Trim().Replace("`r`n", " "))

					# Damage the installed copy (a changed file, a leftover file) and switch the mod off on purpose; the next start repairs the files and respects the choice
					$someFile = @($bundledHashes.Keys | Where-Object { $_ -match 'scripts[\\/].*\.lemon$' } | Select-Object -First 1)[0]
					Set-Content -LiteralPath (Join-Path $installedFresh $someFile.TrimStart('\')) -Value "// damaged" -Encoding ascii
					Set-Content -LiteralPath (Join-Path $installedFresh "scripts\leftover_debug_script.lemon") -Value "// leftover" -Encoding ascii
					Set-Content -LiteralPath (Join-Path $freshMods "active-mods.json") -Value '{"ActiveMods":[],"UseLegacyLoading":false}' -Encoding ascii
					$freshProcess = Start-Process -FilePath $freshExe.FullName -WorkingDirectory $freshGameDir -PassThru
					[void]$startedProcesses.Add($freshProcess)
					$null = Wait-Until { -not (Test-Path -LiteralPath (Join-Path $installedFresh "scripts\leftover_debug_script.lemon")) -or $freshProcess.HasExited } 45
					Start-Sleep -Seconds 2
					Stop-StartedProcesses
					Start-Sleep -Seconds 1
					$repairedHashes = & $hashTree $installedFresh
					$repaired = ($repairedHashes.Count -eq $bundledHashes.Count) -and (@($bundledHashes.Keys | Where-Object { $repairedHashes[$_] -ne $bundledHashes[$_] }).Count -eq 0)
					$activeAfter = [string](Read-SharedText (Join-Path $freshMods "active-mods.json"))
					Add-Result "U8b" "The next start repairs a damaged or stale installed copy and removes leftover files, and does not switch the mod on again after the player switched it off" ($repaired -and ($activeAfter -notmatch '"DropInCoop"')) ("repaired: $repaired, active-mods.json: " + $activeAfter.Trim().Replace("`r`n", " "))
				}
				catch { Add-Result "U8a" "A fresh unzipped package installs its bundled co-op mod" $false ($_.Exception.Message + " (script line " + $_.InvocationInfo.ScriptLineNumber + ": " + $_.InvocationInfo.Line.Trim() + ")") }
				finally
				{
					Stop-StartedProcesses
					Start-Sleep -Seconds 1
					if (Test-Path -LiteralPath $freshDir) { try { Remove-Item -LiteralPath $freshDir -Recurse -Force } catch { } }
				}

					# A player's computer may have leftover developer variables. The packaged game must ignore them (no marker file next to it),
					# and with the marker file the very same variables must work (so that the first result means something).
					$probeVariables = @{ 'S3AIR_NETPLAY' = ('hostlan:' + (New-SessionCode)); 'S3AIR_LOGNAME' = '_hookprobe'; 'S3AIR_NETPLAY_DUMP' = '1'; 'S3AIR_NETPLAY_DEBUG' = '1'; 'S3AIR_FORCE_ROLE' = '1' }
					$probeLog = Join-Path $appdata 'logfile_hookprobe.txt'
					$probeOnlineLog = Join-Path $appdata 'online_log_hookprobe.txt'
					$probeMarker = Join-Path $packageDir 'testhooks.enabled'
					foreach ($withMarker in @($false, $true))
					{
						Stop-StartedProcesses
						Remove-Item -LiteralPath $gameLog, $probeLog, $probeOnlineLog, $onlineLog -Force -ErrorAction SilentlyContinue
						if ($withMarker) { Set-Content -LiteralPath $probeMarker -Value 'test hooks' -Encoding ascii } elseif (Test-Path -LiteralPath $probeMarker) { Remove-Item -LiteralPath $probeMarker -Force }
						foreach ($key in $probeVariables.Keys) { [Environment]::SetEnvironmentVariable($key, [string]$probeVariables[$key], 'Process') }
						try { $probeProcess = Start-Process -FilePath $packageExe.FullName -WorkingDirectory $packageDir -PassThru }
						finally { foreach ($key in $probeVariables.Keys) { [Environment]::SetEnvironmentVariable($key, $null, 'Process') } }
						[void]$startedProcesses.Add($probeProcess)
						$null = Wait-Until { ((Read-SharedText $gameLog) -match 'Ready to go') -or ((Read-SharedText $probeLog) -match 'Ready to go') } 60
						Start-Sleep -Seconds 14		# Hosting would have written its online log by now
						$defaultLogText = Read-SharedText $gameLog
						$hookLogExists = Test-Path -LiteralPath $probeLog
						$hostingStarted = (Test-Path -LiteralPath $probeOnlineLog) -or (Test-Path -LiteralPath $onlineLog)
						if ($withMarker)
						{
							Add-Result "U5" "Control: with the marker file next to the same package, the same variables are honoured" ($hookLogExists -and $hostingStarted) ("separate log file used: $hookLogExists, hosting started: $hostingStarted")
						}
						else
						{
							Add-Result "U4" "The packaged game ignores developer environment variables (no log renaming, no hosting, no dumps)" (($defaultLogText -match 'Ready to go') -and -not $hookLogExists -and -not $hostingStarted -and -not ($defaultLogText -match 'Netplay host')) ("default log used: " + ($defaultLogText -match 'Ready to go') + ", separate log file: $hookLogExists, hosting started: $hostingStarted")
						}
					}
					Stop-StartedProcesses
					if (Test-Path -LiteralPath $probeMarker) { Remove-Item -LiteralPath $probeMarker -Force }
			}
			catch
			{
				Add-Result "U1" "The unzipped package starts without an error message" $false $_.Exception.Message
			}
			finally
			{
				Stop-StartedProcesses
				if ((Split-Path -Leaf $unzipDir) -like "s3air_pkgcheck_*" -and (Test-Path -LiteralPath $unzipDir)) { Remove-Item -LiteralPath $unzipDir -Recurse -Force -ErrorAction SilentlyContinue }
			}
			}

			if (Wants "D")
			{
			Write-Step "D. Online play, two game windows on this computer, joined only by the code"

			$session = New-SessionCode

			$good = Invoke-OnlineJoin $session @{} 15 "D1"
			$foundInNetwork = $good.Log -match "found the host in the same network"
			$loadedGame = $good.Log -match "client loaded the game state"
			$aborted = $good.Log -match "aborting"
			$onlineHost = @($good.Lines | Where-Object { $_ -match '^\[host\]' }).Count -gt 0
			$onlineFriend = @($good.Lines | Where-Object { $_ -match '^\[friend\]' }).Count -gt 0
			Add-Result "D1" "Friend joins with the code over the same network" ($good.Reached -and $foundInNetwork -and $loadedGame -and -not $aborted) `
				("in game: " + $good.Reached + ", found by code: " + $foundInNetwork + ", game state received: " + $loadedGame + ", aborted: " + $aborted)
			Add-Result "D2" "Online log is written by both host and friend" ($onlineHost -and $onlineFriend) ("host lines: " + $onlineHost + ", friend lines: " + $onlineFriend)
			$repairs = Get-FriendRepairs
			Add-Result "D3" "Game stays in sync during 15 seconds of play (no repairs)" ($good.Reached -and ($repairs -eq 0)) ("repairs: " + $repairs)

			$wrong = New-SessionCode
			Stop-StartedProcesses
			Remove-Item $friendLogFile -ErrorAction SilentlyContinue
			[void](Start-GameProcess @{ "S3AIR_NETPLAY" = "joinlan:" + $wrong; "S3AIR_LOGNAME" = "_friend" })
			$gaveUp = Wait-Until { (Read-SharedText $friendLogFile) -match "giving up" } 60
			Stop-StartedProcesses
			Save-RunLogs "D4"
			Add-Result "D4" "A wrong or unknown code ends with a message, not a hang" $gaveUp ""
			}

			if (Wants 'S')
			{
				Write-Step 'S. Join secret: a wrong code is refused by the host, the right code gets in'
				Set-ActiveMods @('DropInCoop')
				Stop-StartedProcesses
				$secret = New-Secret
				$wrongSecret = New-Secret
				while ($wrongSecret -eq $secret) { $wrongSecret = New-Secret }
				$secretHostLog = '_shost'
				$secretFriendLog = '_sfriend'
				Remove-Item (Join-Path $appdata 'logfile_shost.txt'), (Join-Path $appdata 'logfile_sfriend.txt') -ErrorAction SilentlyContinue

				$secretHost = Start-GameProcess @{ 'S3AIR_NETPLAY' = "hostdirect:$secret"; 'S3AIR_LOGNAME' = $secretHostLog }
				Start-Sleep -Seconds 12
				$wrongFriend = Start-GameProcess @{ 'S3AIR_NETPLAY' = "joindirect:$wrongSecret"; 'S3AIR_LOGNAME' = $secretFriendLog }
				$refused = Wait-Until { (Get-NetLog $secretFriendLog) -match 'Netplay failure: WRONG_CODE' } 70
				$hostText = Get-NetLog $secretHostLog
				$friendText = Get-NetLog $secretFriendLog
				Add-Result 'S1' 'A friend whose code has the wrong secret is refused by the host and told that the code is wrong' ($refused -and $hostText -match 'refused a friend, the code was wrong') ("friend told WRONG_CODE: $refused, host refused: " + ($hostText -match 'refused a friend, the code was wrong'))
				Add-Result 'S2' 'The refused friend gets no player slot and no game' (-not ($friendText -match 'game state loaded|received start game packet') -and -not ($hostText -match 'proved the code')) ''
				Stop-Process -Id $wrongFriend.Id -Force -ErrorAction SilentlyContinue
				Start-Sleep -Seconds 2
				Remove-Item (Join-Path $appdata 'logfile_sfriend.txt') -ErrorAction SilentlyContinue

				# The same host, a friend with the right code (the earlier refusal must not have hurt the host)
				$rightFriend = Start-GameProcess @{ 'S3AIR_NETPLAY' = "joindirect:$secret"; 'S3AIR_LOGNAME' = $secretFriendLog }
				$secretJoined = Wait-Until { (Get-NetLog $secretFriendLog) -match 'game state loaded, now at frame' } 90
				$hostText = Get-NetLog $secretHostLog
				$friendText = Get-NetLog $secretFriendLog
				Add-Result 'S3' 'A friend with the right secret gets in, both sides prove the code, and the game loads' ($secretJoined -and $friendText -match 'the host proved the code' -and $hostText -match 'a friend proved the code and is player 2') ("joined: $secretJoined")
				Copy-Item (Join-Path $appdata 'logfile_shost.txt') (Join-Path $checksDir 'last_logs\S-logfile_host.txt') -Force -ErrorAction SilentlyContinue
				Copy-Item (Join-Path $appdata 'logfile_sfriend.txt') (Join-Path $checksDir 'last_logs\S-logfile_friend.txt') -Force -ErrorAction SilentlyContinue
				Stop-StartedProcesses
			}

			if (Wants 'X')
			{
				Write-Step 'X. Clear reasons: what the player is told when joining does not work'
				Set-ActiveMods @('DropInCoop')
				$lastLogs = Join-Path $checksDir 'last_logs'

				function Remove-NetLogs([string[]]$suffixes)
				{
					foreach ($suffix in $suffixes) { Remove-Item (Join-Path $appdata ("logfile" + $suffix + ".txt")) -ErrorAction SilentlyContinue }
				}
				function Save-NetLog([string]$suffix, [string]$tag)
				{
					Copy-Item (Join-Path $appdata ("logfile" + $suffix + ".txt")) (Join-Path $lastLogs ("X-" + $tag + ".txt")) -Force -ErrorAction SilentlyContinue
				}

				# X1: the matchmaking server cannot be reached (all UDP traffic of this one process is dropped)
				Stop-StartedProcesses
				Remove-NetLogs @('_xfriend')
				$x1 = Start-GameProcess @{ 'S3AIR_NETPLAY' = ('joinlan:' + (New-SessionCode)); 'S3AIR_NET_LOSS' = '100'; 'S3AIR_LOGNAME' = '_xfriend' }
				$x1ok = Wait-Until { (Get-NetLog '_xfriend') -match 'Netplay failure: SERVER_UNREACHABLE' } 60
				Add-Result 'X1' 'Server unreachable: the friend is told so after about 15 seconds' $x1ok ''
				Save-NetLog '_xfriend' 'server-unreachable'
				Stop-StartedProcesses

				# X2: nobody hosts with this code (needs the public matchmaking server to answer)
				Remove-NetLogs @('_xfriend')
				$x2 = Start-GameProcess @{ 'S3AIR_NETPLAY' = ('joinlan:' + (New-SessionCode)); 'S3AIR_LOGNAME' = '_xfriend' }
				$x2ok = Wait-Until { (Get-NetLog '_xfriend') -match 'Netplay failure: (WRONG_CODE|SERVER_UNREACHABLE)' } 80
				$x2text = Get-NetLog '_xfriend'
				Add-Result 'X2' 'Wrong code: nobody hosts with it, the friend is told to check the code' ($x2ok -and $x2text -match 'Netplay failure: WRONG_CODE') $(if ($x2text -match 'Netplay failure: SERVER_UNREACHABLE') { 'the public server did not answer, so the wrong-code message could not be reached' } else { '' })
				Save-NetLog '_xfriend' 'wrong-code'
				Stop-StartedProcesses

				# X3: no host listening at the address (a blocked connection looks the same): timeout with the router and firewall hint
				Remove-NetLogs @('_xfriend')
				$x3 = Start-GameProcess @{ 'S3AIR_NETPLAY' = ('joindirect:' + (New-Secret)); 'S3AIR_LOGNAME' = '_xfriend' }
				$x3ok = Wait-Until { (Get-NetLog '_xfriend') -match 'Netplay failure: BLOCKED_OR_TIMEOUT' } 100
				Add-Result 'X3' 'Blocked or no answer: the friend is told that a router or firewall probably blocks the connection' $x3ok ''
				Save-NetLog '_xfriend' 'blocked'
				Stop-StartedProcesses

				# X4 and X5: a different game version, different mods (a developer switch makes the friend look different)
				foreach ($case in @(@{ Id = 'X4'; Switch = 'S3AIR_NETPLAY_FAKE_BUILD'; Reason = 'VERSION_MISMATCH'; Title = 'Different game version' }, @{ Id = 'X5'; Switch = 'S3AIR_NETPLAY_FAKE_MODS'; Reason = 'MOD_MISMATCH'; Title = 'Different mods' }))
				{
					Remove-NetLogs @('_xhost', '_xfriend')
					$caseSecret = New-Secret
					[void](Start-GameProcess @{ 'S3AIR_NETPLAY' = "hostdirect:$caseSecret"; 'S3AIR_LOGNAME' = '_xhost' })
					Start-Sleep -Seconds 12
					$caseFriendVars = @{ 'S3AIR_NETPLAY' = "joindirect:$caseSecret"; 'S3AIR_LOGNAME' = '_xfriend' }
					$caseFriendVars[$case.Switch] = '1'
					[void](Start-GameProcess $caseFriendVars)
					$caseOk = Wait-Until { (Get-NetLog '_xfriend') -match ('Netplay failure: ' + $case.Reason) } 80
					$caseJoined = (Get-NetLog '_xfriend') -match 'game state loaded, now at frame'
					Add-Result $case.Id ($case.Title + ': the friend is told, and does not join') ($caseOk -and -not $caseJoined) ''
					Save-NetLog '_xfriend' ($case.Reason.ToLower())
					Stop-StartedProcesses
				}

				# X6: lobby full, and Retry. The host has room for one friend only. A second friend is refused ("lobby full") and presses Retry by itself (test switch);
				# when the first friend leaves, the retry gets in.
				$lobbyCode = New-SessionCode
				Remove-NetLogs @('_xhost', '_xfriend', '_xfriend2')
				[void](Start-GameProcess @{ 'S3AIR_LOBBY_OPEN' = 'hostlan'; 'S3AIR_LOBBY_CODE' = $lobbyCode; 'S3AIR_NETPLAY_MAXCLIENTS' = '1'; 'S3AIR_LOGNAME' = '_xhost' })
				Start-Sleep -Seconds 14
				$firstFriend = Start-GameProcess @{ 'S3AIR_LOBBY_OPEN' = 'join'; 'S3AIR_LOBBY_CODE' = $lobbyCode; 'S3AIR_LOBBY_AUTOREADY' = '1'; 'S3AIR_LOGNAME' = '_xfriend' }
				$firstIn = Wait-Until { (Get-NetLog '_xhost') -match 'lobby: player 2 is ready' } 90
				$secondFriend = Start-GameProcess @{ 'S3AIR_LOBBY_OPEN' = 'join'; 'S3AIR_LOBBY_CODE' = $lobbyCode; 'S3AIR_LOBBY_AUTORETRY' = '1'; 'S3AIR_LOGNAME' = '_xfriend2' }
				$fullTold = Wait-Until { (Get-NetLog '_xfriend2') -match 'Netplay failure: LOBBY_FULL' } 90
				Start-Sleep -Seconds 3
				$secondText = Get-NetLog '_xfriend2'
				$fullShown = $secondText -match 'showing the problem to the player \(LOBBY_FULL\) with a Retry button'
				Save-ScreenPicture (Join-Path $lastLogs 'X-lobby-full-screen.png')
				Add-Result 'X6' 'Lobby full: the second friend is told that the lobby is full, with a Retry button' ($firstIn -and $fullTold -and $fullShown) ("first friend in: $firstIn, told: $fullTold, retry shown: $fullShown")
				Stop-Process -Id $firstFriend.Id -Force -ErrorAction SilentlyContinue
				$retryIn = Wait-Until { (Get-NetLog '_xfriend2') -match 'the player pressed Retry' -and (Get-NetLog '_xfriend2') -match 'lobby: you are slot 2' } 120
				Add-Result 'X7' 'Retry works: after a place became free, pressing Retry gets the friend into the lobby' $retryIn ''
				Save-NetLog '_xfriend2' 'lobby-full-retry'
				Save-NetLog '_xhost' 'lobby-host'
				Stop-StartedProcesses

				# X10: a host who cannot reach the matchmaking server gives up with a message after about 25 seconds (it used to wait forever)
					Remove-NetLogs @('_xhost')
					[void](Start-GameProcess @{ 'S3AIR_NETPLAY' = ('hostserver:' + (New-Secret)); 'S3AIR_NET_LOSS' = '100'; 'S3AIR_LOGNAME' = '_xhost' })
					$x10ok = Wait-Until { (Get-NetLog '_xhost') -match 'Netplay failure: SERVER_UNREACHABLE' } 70
					Add-Result 'X10' 'A host who cannot reach the matchmaking server is told so, instead of waiting forever' $x10ok ''
					Save-NetLog '_xhost' 'host-server-unreachable'
					Stop-StartedProcesses

					# X8: the host closes its lobby (gracefully); X9: the host disappears without a word (the process is ended)
				foreach ($case in @(@{ Id = 'X8'; Close = $true; Title = 'Host closed the lobby' }, @{ Id = 'X9'; Close = $false; Title = 'Host stopped answering' }))
				{
					$closeCode = New-SessionCode
					Remove-NetLogs @('_xhost', '_xfriend')
					$hostVars = @{ 'S3AIR_LOBBY_OPEN' = 'hostlan'; 'S3AIR_LOBBY_CODE' = $closeCode; 'S3AIR_LOGNAME' = '_xhost' }
					if ($case.Close) { $hostVars['S3AIR_NETPLAY_CLOSE_AFTER'] = '4' }
					$closingHost = Start-GameProcess $hostVars
					Start-Sleep -Seconds 14
					[void](Start-GameProcess @{ 'S3AIR_LOBBY_OPEN' = 'join'; 'S3AIR_LOBBY_CODE' = $closeCode; 'S3AIR_LOBBY_AUTOREADY' = '1'; 'S3AIR_LOGNAME' = '_xfriend' })
					$inLobby = Wait-Until { (Get-NetLog '_xfriend') -match 'lobby: you are slot 2' } 90
					if (-not $case.Close -and $inLobby) { Start-Sleep -Seconds 3; Stop-Process -Id $closingHost.Id -Force -ErrorAction SilentlyContinue }
					$closedTold = Wait-Until { (Get-NetLog '_xfriend') -match 'Netplay failure: HOST_CLOSED' } 150
					Start-Sleep -Seconds 2
					$closedText = Get-NetLog '_xfriend'
					Save-ScreenPicture (Join-Path $lastLogs ('X-' + $case.Id + '-screen.png'))
					Add-Result $case.Id ($case.Title + ': the friend in the lobby is told, with a Retry button') ($inLobby -and $closedTold -and $closedText -match 'showing the problem to the player \(HOST_CLOSED\) with a Retry button') ("in lobby: $inLobby, told: $closedTold")
					Save-NetLog '_xfriend' ('host-closed-' + $case.Id.ToLower())
					Stop-StartedProcesses
				}
			}

			if (Wants 'Y')
			{
				Write-Step 'Y. Relay fallback: the direct path is a dead end, the session completes through the relay server; abuse against the real relay program'
				Set-ActiveMods @('DropInCoop')
				$lastLogs = Join-Path $checksDir 'last_logs'
				New-Item -ItemType Directory -Path $lastLogs -Force | Out-Null
				$proxyScript = Join-Path $checksDir 'udp_lag_proxy.ps1'
				$relayExe = Join-Path $lastLogs 'relay\s3air_relay.exe'
				if (-not (Test-Path -LiteralPath $relayExe)) { & (Join-Path $checksDir 'build_relay.ps1') | Out-Null }
				$relayPort = 28850

				function Read-KeyValueFile([string]$path)
				{
					$values = @{}
					foreach ($line in @((Read-SharedText $path) -split "`r?`n")) { if ($line -match '^(\w+)=(.*)$') { $values[$Matches[1]] = $Matches[2] } }
					return $values
				}

				function Start-RelayProgram([string]$statsPath, [string]$outPath)
				{
					Remove-Item -LiteralPath $statsPath, $outPath -ErrorAction SilentlyContinue
					$relayArgs = @('--bind', '127.0.0.1', '--port', [string]$relayPort, '--stats-file', ('"' + $statsPath + '"'), '--stats-seconds', '2', '--run-seconds', '600')
					$process = Start-Process -FilePath $relayExe -ArgumentList $relayArgs -PassThru -WindowStyle Hidden -RedirectStandardOutput $outPath
					[void]$startedProcesses.Add($process)
					[void](Wait-Until { (Read-SharedText $statsPath) -match 'uptime_s' } 20)
					return $process
				}

				function Start-DeadEndProxy([string]$statsPath)
				{
					# The direct path: everything the friend sends to the host's address goes into this proxy, which drops 100 percent of it (a connection that cannot be made, like a strict NAT)
					Remove-Item -LiteralPath $statsPath -ErrorAction SilentlyContinue
					$proxyArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', ('"' + $proxyScript + '"'), '-ListenPort', '28841', '-TargetPort', '28840', '-RttMs', '0', '-JitterMs', '0', '-LossPercent', '100', '-StatsFile', ('"' + $statsPath + '"'), '-Seed', '7', '-Seconds', '500')
					$process = Start-Process -FilePath 'powershell.exe' -ArgumentList $proxyArgs -PassThru -WindowStyle Hidden
					[void]$startedProcesses.Add($process)
					[void](Wait-Until { (Read-SharedText $statsPath) -match 'uptime_s' } 30)
					return $process
				}

				$yRelayAddress = "127.0.0.1:$relayPort"

				# Y1: without a relay (the default) the blocked connection fails as it always did, and the relay code stays silent
				try
				{
					Stop-StartedProcesses
					foreach ($suffix in @('_yhost', '_yfriend')) { Remove-Item (Join-Path $appdata ("logfile" + $suffix + ".txt")), (Join-Path $appdata ("online_log" + $suffix + ".txt")) -ErrorAction SilentlyContinue }
					$y1Code = New-SessionCode
					[void](Start-DeadEndProxy (Join-Path $lastLogs 'Y-proxy-Y1.txt'))
					[void](Start-GameProcess @{ 'S3AIR_NETPLAY' = "hostlan:$y1Code"; 'S3AIR_LOGNAME' = '_yhost' })
					Start-Sleep -Seconds 10
					[void](Start-GameProcess @{ 'S3AIR_NETPLAY' = "joinintro:${y1Code}:28841"; 'S3AIR_LOGNAME' = '_yfriend' })
					$y1failed = Wait-Until { (Get-NetLog '_yfriend') -match 'Netplay failure: BLOCKED_OR_TIMEOUT' } 80
					$y1friend = Get-NetLog '_yfriend'
					Copy-Item (Join-Path $appdata 'logfile_yfriend.txt') (Join-Path $lastLogs 'Y1-friend.txt') -Force -ErrorAction SilentlyContinue
					Copy-Item (Join-Path $appdata 'logfile_yhost.txt') (Join-Path $lastLogs 'Y1-host.txt') -Force -ErrorAction SilentlyContinue
					Add-Result 'Y1' 'No relay set (the default): a blocked direct connection fails with the usual timeout message, no relay is tried' ($y1failed -and $y1friend -notmatch 'relay server') ("timeout message shown: $y1failed, relay mentioned: " + ($y1friend -match 'relay server'))
				}
				catch { Add-Result 'Y1' 'No relay set (the default): a blocked direct connection fails as before' $false $_.Exception.Message }
				finally { Stop-StartedProcesses }

				# Y2: with the same relay on both sides the same blocked connection completes through the relay and the game runs in sync
				try
				{
					Stop-StartedProcesses
					foreach ($suffix in @('_yhost', '_yfriend')) { Remove-Item (Join-Path $appdata ("logfile" + $suffix + ".txt")), (Join-Path $appdata ("online_log" + $suffix + ".txt")) -ErrorAction SilentlyContinue }
					$y2Code = New-SessionCode
					$relayStatsPath = Join-Path $lastLogs 'Y-relay-Y2.txt'
					$relayOutPath = Join-Path $lastLogs 'Y-relay-Y2-output.txt'
					$proxyStatsPath = Join-Path $lastLogs 'Y-proxy-Y2.txt'
					[void](Start-RelayProgram $relayStatsPath $relayOutPath)
					[void](Start-DeadEndProxy $proxyStatsPath)
					[void](Start-GameProcess @{ 'S3AIR_NETPLAY' = "hostlan:$y2Code"; 'S3AIR_RELAY' = $yRelayAddress; 'S3AIR_LOGNAME' = '_yhost' })
					Start-Sleep -Seconds 10
					[void](Start-GameProcess @{ 'S3AIR_NETPLAY' = "joinintro:${y2Code}:28841"; 'S3AIR_RELAY' = $yRelayAddress; 'S3AIR_LOGNAME' = '_yfriend'; 'S3AIR_NETPLAY_TEST_INPUT' = '0x1'; 'S3AIR_NETPLAY_TEST_TOGGLE' = '1' })
					$y2reached = Wait-Until { (@((Read-SharedText (Join-Path $appdata 'online_log_yfriend.txt')) -split "`r?`n" | Where-Object { $_ -match '^\[friend\].* state=5 ' })).Count -gt 0 } 120
					if ($y2reached) { Start-Sleep -Seconds 40 }
					$y2Host = Get-NetLog '_yhost'
					$y2Friend = Get-NetLog '_yfriend'
					$y2FriendLines = @((Read-SharedText (Join-Path $appdata 'online_log_yfriend.txt')) -split "`r?`n" | Where-Object { $_ -match '^\[friend\]' })
					$y2HostLines = @((Read-SharedText (Join-Path $appdata 'online_log_yhost.txt')) -split "`r?`n" | Where-Object { $_ -match '^\[host\]' })
					Copy-Item (Join-Path $appdata 'logfile_yhost.txt') (Join-Path $lastLogs 'Y2-host.txt') -Force -ErrorAction SilentlyContinue
					Copy-Item (Join-Path $appdata 'logfile_yfriend.txt') (Join-Path $lastLogs 'Y2-friend.txt') -Force -ErrorAction SilentlyContinue
					Set-Content -Path (Join-Path $lastLogs 'Y2-online-lines.txt') -Value ($y2HostLines + $y2FriendLines) -Encoding utf8
					$relayStats = Read-KeyValueFile $relayStatsPath
					$proxyStats = Read-KeyValueFile $proxyStatsPath
					Stop-StartedProcesses

					$inGame = @($y2FriendLines | Where-Object { $_ -match ' state=5 ' })
					$fpsValues = @()
					for ($i = 2; $i -lt $inGame.Count; $i++) { $m = [regex]::Match($inGame[$i], 'fps=([\d.]+)'); if ($m.Success) { $fpsValues += [double]$m.Groups[1].Value } }
					$fpsMin = if ($fpsValues.Count -gt 0) { ($fpsValues | Measure-Object -Minimum).Minimum } else { 0 }
					$repairs = 0
					foreach ($line in $y2FriendLines) { $m = [regex]::Match($line, 'repairs=(\d+)'); if ($m.Success -and [int]$m.Groups[1].Value -gt $repairs) { $repairs = [int]$m.Groups[1].Value } }
					$syncFrames = @($inGame | ForEach-Object { $m = [regex]::Match($_, 'last sync ok at frame (\d+)'); if ($m.Success) { [int]$m.Groups[1].Value } })
					$syncProgress = if ($syncFrames.Count -ge 2) { ($syncFrames | Measure-Object -Maximum).Maximum - ($syncFrames | Measure-Object -Minimum).Minimum } else { 0 }
					$aborted = ($y2Friend -match 'giving up|aborting') -or ($y2Host -match 'aborting')

					Add-Result 'Y2a' 'Direct path blocked, relay set on both sides: the friend gets into the running game anyway' ($y2reached -and -not $aborted) ("in game: $y2reached, aborted: $aborted")
					Add-Result 'Y2b' 'The friend tried the direct connection, then switched to the relay, was let in, and both sides proved the code' (($y2Friend -match 'the direct connection did not work, connecting through the relay server instead') -and ($y2Friend -match 'the relay server let us in, connecting to the host through it') -and ($y2Friend -match 'the host proved the code') -and ($y2Host -match 'a friend connects through the relay server') -and ($y2Host -match 'a friend proved the code and is player 2')) ''
					Add-Result 'Y2c' 'Nothing got through the direct path (the proxy forwarded 0 datagrams in both directions, and dropped some)' (([int64]$proxyStats['c2s_forwarded'] -eq 0) -and ([int64]$proxyStats['s2c_forwarded'] -eq 0) -and ([int64]$proxyStats['c2s_dropped'] -gt 0)) ("proxy forwarded " + $proxyStats['c2s_forwarded'] + "/" + $proxyStats['s2c_forwarded'] + ", dropped " + $proxyStats['c2s_dropped'] + "/" + $proxyStats['s2c_dropped'])
					Add-Result 'Y2d' 'The relay carried the game: it forwarded many datagrams, one lobby, one friend, no wrong keys' (([int64]$relayStats['forwarded'] -gt 500) -and ([int64]$relayStats['wrong_keys'] -eq 0) -and ([int64]$relayStats['lobbies_created'] -eq 1)) ("relay forwarded " + $relayStats['forwarded'] + ", wrong keys " + $relayStats['wrong_keys'] + ", lobbies " + $relayStats['lobbies_created'])
					Add-Result 'Y2e' 'Through the relay the game stays in sync (no repairs), the sync keeps advancing, and the friend runs at about 60 fps' (($repairs -eq 0) -and ($syncProgress -gt 600) -and ($fpsValues.Count -ge 3) -and ($fpsMin -ge 45)) ("repairs $repairs, sync advanced $syncProgress frames, fps min $fpsMin over $($fpsValues.Count) samples")
					Add-Result 'Y2f' 'The online logs say that the friend plays through the relay (friend relay=1, host relayed=1)' (($y2FriendLines -match 'state=5 .*relay=1').Count -gt 0 -and ($y2HostLines -match 'relayed=1').Count -gt 0) ''
					$relayOutput = Read-SharedText $relayOutPath
					Add-Result 'Y2g' 'The relay program prints no addresses and no lobby or key values (counters only)' (($relayOutput -match 's3air_relay') -and ($relayOutput -notmatch '\d+\.\d+\.\d+\.\d+') -and ($relayOutput -notmatch '(?i)127\.0|lobby code|key=')) ''
				}
				catch { Add-Result 'Y2a' 'Relay fallback with the direct path blocked' $false $_.Exception.Message }
				finally { Stop-StartedProcesses }

				# Y3: the lobby screens: the friend is told it plays through the relay, the host sees the warning (pictures)
				try
				{
					Stop-StartedProcesses
					Add-Type -AssemblyName System.Drawing
					Add-Type -TypeDefinition 'using System; using System.Runtime.InteropServices; public class LobbyWinY { [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd); }' -ErrorAction SilentlyContinue
					foreach ($suffix in @('_yhost', '_yfriend')) { Remove-Item (Join-Path $appdata ("logfile" + $suffix + ".txt")), (Join-Path $appdata ("online_log" + $suffix + ".txt")) -ErrorAction SilentlyContinue }
					$y3Code = New-SessionCode
					[void](Start-RelayProgram (Join-Path $lastLogs 'Y-relay-Y3.txt') (Join-Path $lastLogs 'Y-relay-Y3-output.txt'))
					[void](Start-DeadEndProxy (Join-Path $lastLogs 'Y-proxy-Y3.txt'))
					$y3Host = Start-GameProcess @{ 'S3AIR_LOBBY_OPEN' = 'hostlan'; 'S3AIR_LOBBY_CODE' = $y3Code; 'S3AIR_RELAY' = $yRelayAddress; 'S3AIR_LOGNAME' = '_yhost'; 'S3AIR_PLAYER_NAME' = 'Hosty' }
					Start-Sleep -Seconds 12
					$y3Friend = Start-GameProcess @{ 'S3AIR_NETPLAY' = "joinintro:${y3Code}:28841"; 'S3AIR_LOBBY_OPEN' = 'show'; 'S3AIR_LOBBY_AUTOREADY' = '1'; 'S3AIR_RELAY' = $yRelayAddress; 'S3AIR_LOGNAME' = '_yfriend'; 'S3AIR_PLAYER_NAME' = 'Relayed' }
					$y3In = Wait-Until { (Get-NetLog '_yhost') -match 'a friend proved the code and is player 2' } 120
					$y3Drawn = Wait-Until { (Get-NetLog '_yhost') -match 'Lobby: on screen: relay warning' } 30		# the host screen really drew the relay warning
					Start-Sleep -Seconds 6
					foreach ($shot in @(@($y3Host, 'Y3-lobby-host.png', $y3Friend), @($y3Friend, 'Y3-lobby-friend.png', $y3Host)))
					{
						$shot[2].Refresh(); [void][LobbyWinY]::ShowWindow($shot[2].MainWindowHandle, 7)
						Start-Sleep -Milliseconds 700
						$shot[0].Refresh(); [void][LobbyWinY]::ShowWindow($shot[0].MainWindowHandle, 9)
						Start-Sleep -Milliseconds 700
						[void](Save-GameScreenshot $shot[0] (Join-Path $lastLogs $shot[1]))
						[void][LobbyWinY]::ShowWindow($shot[2].MainWindowHandle, 4)
					}
					$y3HostText = Get-NetLog '_yhost'
					$y3FriendText = Get-NetLog '_yfriend'
					Copy-Item (Join-Path $appdata 'logfile_yhost.txt') (Join-Path $lastLogs 'Y3-host.txt') -Force -ErrorAction SilentlyContinue
					Copy-Item (Join-Path $appdata 'logfile_yfriend.txt') (Join-Path $lastLogs 'Y3-friend.txt') -Force -ErrorAction SilentlyContinue
					Add-Result 'Y3' 'Lobby through the relay: the friend joins with its name and sees the lobby, the host sees it as player 2 (see Y3-lobby-host.png and Y3-lobby-friend.png for the relay warnings)' ($y3In -and $y3Drawn -and ($y3FriendText -match 'lobby: you are slot 2') -and ($y3HostText -match 'player 2 is called "Relayed"')) "joined: $y3In, host screen drew the relay warning: $y3Drawn"
				}
				catch { Add-Result 'Y3' 'Lobby through the relay' $false $_.Exception.Message }
				finally { Stop-StartedProcesses }

				# Y4: abuse against the real relay program (forged datagrams over the loopback)
				try
				{
					Stop-StartedProcesses
					$abuseStats = Join-Path $lastLogs 'Y-relay-Y4.txt'
					$abuseOut = Join-Path $lastLogs 'Y-relay-Y4-output.txt'
					$relayProcess = Start-RelayProgram $abuseStats $abuseOut
					Add-Type -TypeDefinition @'
using System;
using System.Diagnostics;
using System.Net;
using System.Net.Sockets;
public class RelayFlood
{
	public static long Send(Socket socket, int port, byte[] data, int count)
	{
		IPEndPoint target = new IPEndPoint(IPAddress.Loopback, port);
		Stopwatch watch = Stopwatch.StartNew();
		for (int i = 0; i < count; i++) socket.SendTo(data, target);
		return watch.ElapsedMilliseconds;
	}
}
'@ -ErrorAction SilentlyContinue

					function New-RelayClient([string]$localIp = '') { if ($localIp) { $c = New-Object System.Net.Sockets.UdpClient((New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Parse($localIp), 0))) } else { $c = New-Object System.Net.Sockets.UdpClient(0, [System.Net.Sockets.AddressFamily]::InterNetwork) }; $c.Client.ReceiveTimeout = 1500; return $c }
					function Send-Relay($c, [byte[]]$bytes) { [void]$c.Send($bytes, $bytes.Length, (New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Loopback, $relayPort))) }
					function Receive-Relay($c) { $remote = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0); try { return ,($c.Receive([ref]$remote)) } catch { return $null } }
					function Get-BigEndian([uint64]$value, [int]$bytes) { $result = New-Object byte[] $bytes; for ($i = 0; $i -lt $bytes; $i++) { $result[$bytes - 1 - $i] = [byte](($value -shr (8 * $i)) -band 0xff) }; return $result }
					function New-Hello([byte]$type, [uint64]$lobby, [uint64]$key) { $hello = @(0x53, 0x33, 0x52, 0x4c, 2, $type) + (Get-BigEndian $lobby 8) + (Get-BigEndian $key 8); if ($type -eq 1) { $hello += (Get-BigEndian ([uint64]0x5ec2e75ec2e75ec2) 8) }; return [byte[]]$hello }
					$lobbyId = [uint64]0x5eed5eed01; $goodKey = [uint64]0x1122334455667788

					# A host registers, a friend joins, data goes both ways through the real program
					$hostClient = New-RelayClient; $friendClient = New-RelayClient
					Send-Relay $hostClient (New-Hello 1 $lobbyId $goodKey)
					$registered = Receive-Relay $hostClient
					Send-Relay $friendClient (New-Hello 3 $lobbyId $goodKey)
					$joined = Receive-Relay $friendClient
					$hostToken = if ($registered -and $registered.Length -eq 18 -and $registered[5] -eq 2) { $registered[14..17] } else { $null }
					$friendToken = if ($joined -and $joined.Length -eq 10 -and $joined[5] -eq 4) { $joined[6..9] } else { $null }
					Add-Result 'Y4a' 'Real relay program: a host registers and a friend with the key joins' (($null -ne $hostToken) -and ($null -ne $friendToken)) ''
					$forwardedOk = $false
					if ($hostToken -and $friendToken)
					{
						Send-Relay $friendClient ([byte[]](@(0x53, 0x33, 0x52, 0x4c, 2, 6) + $friendToken + @(11, 22, 33)))
						$atHost = Receive-Relay $hostClient
						Send-Relay $hostClient ([byte[]](@(0x53, 0x33, 0x52, 0x4c, 2, 8) + $hostToken + @(0, 1) + @(44, 55)))
						$atFriend = Receive-Relay $friendClient
						$forwardedOk = ($atHost -and $atHost.Length -eq 15 -and $atHost[5] -eq 9 -and $atHost[12] -eq 11 -and $atHost[14] -eq 33) -and ($atFriend -and $atFriend.Length -eq 12 -and $atFriend[5] -eq 7 -and $atFriend[10] -eq 44 -and $atFriend[11] -eq 55)
					}
					Add-Result 'Y4b' 'Real relay program: datagrams are forwarded in both directions with the payload unchanged' $forwardedOk ''

					# Wrong keys: no answer at all, and after 8 attempts the address is ignored even with the right key
					$stranger = New-RelayClient; $stranger.Client.ReceiveTimeout = 400
					$answers = 0
					for ($i = 0; $i -lt 9; $i++) { Send-Relay $stranger (New-Hello 3 $lobbyId ([uint64](0x4000 + $i))); if ($null -ne (Receive-Relay $stranger)) { $answers++ } }
					Send-Relay $stranger (New-Hello 3 $lobbyId $goodKey)
					$answeredAfterBlock = ($null -ne (Receive-Relay $stranger))
					$otherPort = New-RelayClient; $otherPort.Client.ReceiveTimeout = 400		# same IP address, other source port: must not escape the block
					Send-Relay $otherPort (New-Hello 3 $lobbyId $goodKey)
					$answeredOtherPort = ($null -ne (Receive-Relay $otherPort))
					$otherPort.Close()
					$other = New-RelayClient '127.0.0.2'		# another IP address: the wrong-key block is per IP address, not per port
					Send-Relay $other (New-Hello 3 $lobbyId $goodKey)
					$otherJoined = ($null -ne (Receive-Relay $other))
					Add-Result 'Y4c' 'Real relay program: wrong keys get no answer; after 8 the IP address is ignored even with the right key (also from another source port); other IP addresses are not affected' (($answers -eq 0) -and (-not $answeredAfterBlock) -and (-not $answeredOtherPort) -and $otherJoined) "answers to wrong keys: $answers, answered after block: $answeredAfterBlock, answered on another source port of the same IP: $answeredOtherPort, other IP address joined: $otherJoined"

					# A flood of valid data from the friend: the relay caps it and stays responsive
					$before = Read-KeyValueFile $abuseStats
					$floodData = [byte[]](@(0x53, 0x33, 0x52, 0x4c, 2, 6) + $friendToken + (1..100 | ForEach-Object { 7 }))
					$elapsedMs = [RelayFlood]::Send($friendClient.Client, $relayPort, $floodData, 40000)
					Start-Sleep -Seconds 3
					$hostClient.Client.ReceiveTimeout = 300
					$after = Read-KeyValueFile $abuseStats
					Add-Result 'Y4d' 'Real relay program: a flood of 40000 datagrams from one address is rate limited (the relay forwarded only what the limit allows and dropped the rest)' (([int64]$after['dropped_rate'] -gt 0) -and ([int64]$after['forwarded'] -le 6000 + 3000 * ($elapsedMs / 1000.0 + 2) + 500)) ("sent 40000 in $elapsedMs ms (from the friend's own address and token); relay received " + $after['received'] + ", forwarded " + $after['forwarded'] + ", dropped by rate " + $after['dropped_rate'])
					$relayAlive = -not $relayProcess.HasExited
					$fresh = New-RelayClient '127.0.0.2'
					Send-Relay $fresh (New-Hello 3 $lobbyId $goodKey)
					$freshJoined = ($null -ne (Receive-Relay $fresh))
					Add-Result 'Y4e' 'Real relay program: still running and still serving a new friend after the flood' ($relayAlive -and $freshJoined) "running: $relayAlive, new friend joined: $freshJoined"

					# Garbage and oversized datagrams: no answer, no crash
					$garbage = New-RelayClient; $garbage.Client.ReceiveTimeout = 400
					Send-Relay $garbage ([byte[]](1..200 | ForEach-Object { 0xaa }))
					Send-Relay $garbage ([byte[]](1..5000 | ForEach-Object { 0x53 }))
					Send-Relay $garbage ([byte[]]@(0x53, 0x33, 0x52, 0x4c, 2, 3))
					$garbageAnswer = Receive-Relay $garbage
					Start-Sleep -Milliseconds 2500		# (the counter file is rewritten once per second)
					$final = Read-KeyValueFile $abuseStats
					Add-Result 'Y4f' 'Real relay program: garbage, oversized and truncated datagrams get no answer and do not crash it' (($null -eq $garbageAnswer) -and (-not $relayProcess.HasExited) -and ([int64]$final['dropped_invalid'] -ge [int64]$after['dropped_invalid'] + 2)) ("(a datagram bigger than the relay's receive buffer, like the 5000 byte one, is discarded by the operating system before the relay can count it, so 2 of the 3 are counted) dropped invalid: " + $after['dropped_invalid'] + " -> " + $final['dropped_invalid'])
					foreach ($client in @($hostClient, $friendClient, $stranger, $other, $fresh, $garbage)) { $client.Close() }
				}
				catch { Add-Result 'Y4' 'Relay abuse tests' $false $_.Exception.Message }
				finally { Stop-StartedProcesses }
			}

			if (Wants 'M')
			{
				Write-Step 'M. Public matchmaking server registration and code join'
				$randomBytes = New-Object byte[] 5
				$random = [Security.Cryptography.RandomNumberGenerator]::Create()
				try { $random.GetBytes($randomBytes) } finally { $random.Dispose() }
				$serverSession = ([BitConverter]::ToString($randomBytes)).Replace('-', '').ToLowerInvariant()
				$serverHostLog = Join-Path $appdata 'logfile_serverhost.txt'
				$serverFriendLog = Join-Path $appdata 'logfile_serverfriend.txt'
				$serverHostOnline = Join-Path $appdata 'online_log_serverhost.txt'
				$serverFriendOnline = Join-Path $appdata 'online_log_serverfriend.txt'
				Remove-Item $serverHostLog, $serverFriendLog, $serverHostOnline, $serverFriendOnline -ErrorAction SilentlyContinue
				Stop-StartedProcesses
				$serverHost = Start-GameProcess @{ 'S3AIR_NETPLAY'="hostserver:$serverSession"; 'S3AIR_LOGNAME'='_serverhost' }
				$hostRegistered = Wait-Until { (Read-SharedText $serverHostLog) -match 'game server answered, success = 1' -or $serverHost.HasExited } 60
				$hostRegistered = $hostRegistered -and (Read-SharedText $serverHostLog) -match 'game server answered, success = 1'
				Add-Result 'M1' 'Host registers its temporary session with the public matchmaking server' $hostRegistered $(if ($hostRegistered) { "session $serverSession" } else { 'no successful registration response within 60 seconds' })
				$serverJoinReached = $false
				$serverJoinDetail = 'host did not register; friend join was not attempted'
				if ($hostRegistered)
				{
					$serverFriend = Start-GameProcess @{ 'S3AIR_NETPLAY'="joinserver:$serverSession"; 'S3AIR_LOGNAME'='_serverfriend' }
					$serverJoinReached = Wait-Until { (Read-SharedText $serverFriendOnline) -match '^\[friend\].* state=5 ' -or (Read-SharedText $serverFriendLog) -match 'giving up|Could not reach the matchmaking server' -or $serverFriend.HasExited } 120
					$friendText = Read-SharedText $serverFriendLog
					$friendOnlineText = Read-SharedText $serverFriendOnline
					$loadedByCode = $friendText -match 'connected to host' -and $friendText -match 'game state loaded, now at frame' -and $friendOnlineText -match '^\[friend\].* state=5 '
					$serverJoinReached = $serverJoinReached -and $loadedByCode
					$serverJoinDetail = "connected through server and loaded game: $loadedByCode"
				}
				Add-Result 'M2' 'Friend joins the host through public matchmaking by session code' $serverJoinReached $serverJoinDetail
				Stop-StartedProcesses
				Save-RunLogs 'M'
			}

			# ---------------------------------------------------------------------------------------------------
			if (Wants 'N') {
				Write-Step 'N. Four online players and reconnecting a middle player'
				$netPartyTarget = Join-Path $appdata 'mods\NetPartyHarness'
				if (Test-Path $netPartyTarget) { Remove-Item -LiteralPath $netPartyTarget -Recurse -Force }
				Copy-Item (Join-Path $checksDir 'harness\NetPartyHarness') $netPartyTarget -Recurse
				# The harness imports DropInCoop globals and functions; load the production mod first.
				Set-ActiveMods @('DropInCoop', 'NetPartyHarness')
				$configText = $savedConfigText.Replace('"StartPhase": "2"', '"StartPhase": "3"')
				$configText = [regex]::Replace($configText, '(?m)^(\s*)"LoadSaveState": "([^"]*)",', '$1//"LoadSaveState": "$2",')
				Set-Content -Path $configPath -Value $configText -Encoding utf8
				$code = New-SessionCode
				$networkLogs = @()
				foreach ($slot in 1..4) {
					$networkLogs += (Join-Path $appdata "logfile_party$slot.txt")
					Remove-Item (Join-Path $appdata "logfile_party$slot.txt"), (Join-Path $appdata "online_log_party$slot.txt") -ErrorAction SilentlyContinue
				}
				[void](Start-GameProcess @{ 'S3AIR_NETPLAY'="hostlan:$code"; 'S3AIR_LOGNAME'='_party1'; 'S3AIR_NETPLAY_DEBUG'='1'; 'S3AIR_CHECK_CAPTURE_DIR'=(Join-Path $checksDir 'last_logs'); 'S3AIR_CHECK_CAPTURE_PREFIX'='N4' })
				Start-Sleep -Seconds 8
				foreach ($slot in 2..4) {
					$inputMask = if ($slot -eq 2) { '128' } elseif ($slot -eq 3) { '136' } else { '132' }
					$variables = @{ 'S3AIR_NETPLAY'="joinlan:$code"; 'S3AIR_LOGNAME'="_party$slot"; 'S3AIR_NETPLAY_DEBUG'='1'; 'S3AIR_NETPLAY_TEST_INPUT'=$inputMask }
					if ($slot -eq 3) { $variables['S3AIR_NETPLAY_RECONNECT_TEST']='1' }
					[void](Start-GameProcess $variables)
					$slotLog = $networkLogs[$slot - 1]
					$joined = Wait-Until { (Read-SharedText $slotLog) -match 'game state loaded, now at frame|Script compile error|giving up' } 90
					$slotText = Read-SharedText $slotLog
					Add-Result "N$slot" "Online friend receives player slot $slot and a running-game snapshot" ($joined -and $slotText -match "assigned player $slot" -and $slotText -match 'game state loaded, now at frame') ''
				}
				$fourReady = Wait-Until { (Read-SharedText $networkLogs[0]) -match 'CHECK\|network party\|PASS\|players4 input20080 input30088 input40084' } 60
				if ($fourReady) { Start-Sleep -Seconds 20 }
				$allNetworkText = ''
				for ($slot = 1; $slot -le 4; ++$slot) {
					$slotText = Read-SharedText $networkLogs[$slot - 1]
					$allNetworkText += $slotText
					Set-Content -Path (Join-Path $checksDir "last_logs\N4-player$slot.txt") -Value $slotText -Encoding utf8
				}
				$middleReconnected = (Read-SharedText $networkLogs[2]) -match 'reconnected and restored the existing session'
				$hostText = Read-SharedText $networkLogs[0]
				$lastInputs = @([regex]::Matches($hostText, 'CHECK\|network party\|PASS\|([^\r\n]*)'))
				$keptSlot4 = $lastInputs.Count -gt 0 -and $lastInputs[-1].Groups[1].Value -match 'players4 input20080 input30088 input40084'
				$featureReplicas = 0
				foreach ($slotText in $networkLogs) { if ((Read-SharedText $slotText) -match 'CHECK\|network shared monitor\|PASS\|net 1 features 1 players 4 allocated 1 clones 3') { ++$featureReplicas } }
				$moveReplicas = 0
				$bossReplicas = 0
				foreach ($slotText in $networkLogs) {
					$peerText = Read-SharedText $slotText
					if ($peerText -match 'CHECK\|network team move\|PASS\|net 1 enabled 1 launched 1 velocity 1 award 1') { ++$moveReplicas }
					if ($peerText -match 'CHECK\|network boss phase\|PASS\|net 1 allocated 1 first 1 duplicate 1 partner 1') { ++$bossReplicas }
				}
				$desync = $allNetworkText -match "game drifted away|fingerprint mismatch|Script assertion failed|Invalid memory access|Script compile error"
				Add-Result 'N5' 'All four real input slots drive the shared running game' $fourReady ''
				Add-Result 'N6' 'Player three reconnects while player four retains their own input slot' ($middleReconnected -and $keptSlot4) ("middle reconnected: $middleReconnected, final slot inputs correct: $keptSlot4")
				Add-Result 'N7' 'Four-player simulation runs without synchronization repair or script failure' ($fourReady -and -not $desync) ''
				Add-Result 'N8' 'The online team feature creates three deterministic teammate monitor effects' ($fourReady -and $featureReplicas -eq 4) ("peer copies reporting three clones: $featureReplicas/4")
				Add-Result 'N9' 'The online team move launches a teammate with synchronized velocity' ($fourReady -and $moveReplicas -eq 4) ("peer copies reporting a launch: $moveReplicas/4")
				Add-Result 'N10' 'Two online players complete a coordinated boss phase' ($fourReady -and $bossReplicas -eq 4) ("peer copies reporting a boss phase: $bossReplicas/4")
				Stop-StartedProcesses
			}

			if ((Wants 'P') -or ($Only -contains 'P3') -or ($Only -contains 'P4') -or ($Only -contains 'T'))
			{
				Write-Step 'P. Three/four-player characters, independent movement, jumping and rendering'
				$partyCounts = @(3, 4)
				if ($Only -contains 'P3' -or $Only -contains 'T') { $partyCounts = @(3) }
				elseif ($Only -contains 'P4') { $partyCounts = @(4) }
				foreach ($count in $partyCounts) {
					$partyTarget = Join-Path $appdata 'mods\PartyHarness'
					if (Test-Path $partyTarget) { Remove-Item -LiteralPath $partyTarget -Recurse -Force }
					Copy-Item (Join-Path $checksDir 'harness\PartyHarness') $partyTarget -Recurse
					$partyMain = Join-Path $partyTarget 'scripts\main.lemon'
					Set-Content -Path $partyMain -Value (Get-Content -Raw $partyMain).Replace('__PLAYER_COUNT__', [string]$count) -Encoding utf8 -NoNewline
					Set-ActiveMods @('PartyHarness', 'DropInCoop')
					$configText = $savedConfigText.Replace('"StartPhase": "2"', '"StartPhase": "3"')
					$configText = [regex]::Replace($configText, '(?m)^(\s*)"LoadSaveState": "([^"]*)",', '$1//"LoadSaveState": "$2",')
					Set-Content -Path $configPath -Value $configText -Encoding utf8
					Remove-Item $logFile -ErrorAction SilentlyContinue
					$partyGame = Start-GameProcess @{ 'S3AIR_CHECK_CAPTURE_DIR' = (Join-Path $checksDir 'last_logs'); 'S3AIR_CHECK_CAPTURE_PREFIX' = "P$count" }
					[void](Wait-Until { (Read-SharedText $logFile) -match 'CHECK\|run complete\||Invalid memory access|Script assertion failed|Script compile error' -or $partyGame.HasExited } 90)
					$partyLog = Read-SharedText $logFile
					Stop-StartedProcesses
					Set-Content -Path (Join-Path $checksDir "last_logs\P$count-logfile.txt") -Value $partyLog -Encoding utf8
					$errors = [regex]::Match($partyLog, '(?im)^.*(compile error|Unable to resolve|Invalid memory access|Script assertion failed|Invalid VRAM access).*$')
					Add-Result "P${count}L" "$count-player scripts load and finish without errors" (-not $errors.Success -and $partyLog -match 'CHECK\|run complete\|') $(if ($errors.Success) { $errors.Value } else { '' })
					$number = 0
					foreach ($name in @('party actors', 'party control3', 'party control4', 'party jumping', 'party camera', 'party picture', 'party alive', 'party platforms', 'party score', 'party damage', 'party respawn')) {
						++$number
						$entry = [regex]::Match($partyLog, ('CHECK\|' + [regex]::Escape($name) + '\|(PASS|FAIL)\|(.*)'))
						Add-Result "P${count}.$number" "$count players: $name" ($entry.Success -and $entry.Groups[1].Value -eq 'PASS') $(if ($entry.Success) { $entry.Groups[2].Value } else { 'not reported' })
					}
				}
			}

			if (Wants 'Q')
			{
				Write-Step 'Q. Four-player quadrant split-screen (extended local engine)'
				$quadTarget = Join-Path $appdata 'mods\PartyHarness'
				if (Test-Path $quadTarget) { Remove-Item -LiteralPath $quadTarget -Recurse -Force }
				Copy-Item (Join-Path $checksDir 'harness\PartyHarness') $quadTarget -Recurse
				$quadMain = Join-Path $quadTarget 'scripts\main.lemon'
				$quadText = (Get-Content -Raw $quadMain).Replace('__PLAYER_COUNT__', '4').Replace('constant bool PARTY_QUAD_TEST = false', 'constant bool PARTY_QUAD_TEST = true')
				Set-Content -Path $quadMain -Value $quadText -Encoding utf8 -NoNewline
				Set-ActiveMods @('PartyHarness', 'DropInCoop')
				$configText = $savedConfigText.Replace('"StartPhase": "2"', '"StartPhase": "3"')
				$configText = [regex]::Replace($configText, '(?m)^(\s*)"LoadSaveState": "([^"]*)",', '$1//"LoadSaveState": "$2",')
				Set-Content -Path $configPath -Value $configText -Encoding utf8
				Remove-Item $logFile -ErrorAction SilentlyContinue
				$quadGame = Start-GameProcess @{ 'S3AIR_CHECK_CAPTURE_DIR' = (Join-Path $checksDir 'last_logs'); 'S3AIR_CHECK_CAPTURE_PREFIX' = 'Q4' }
				[void](Wait-Until { (Read-SharedText $logFile) -match 'CHECK\|run complete\||Invalid memory access|Script assertion failed|Script compile error|Unable to resolve identifier' -or $quadGame.HasExited } 100)
				$quadLog = Read-SharedText $logFile
				Stop-StartedProcesses
				Set-Content -Path (Join-Path $checksDir 'last_logs\Q4-logfile.txt') -Value $quadLog -Encoding utf8
				$errors = [regex]::Match($quadLog, '(?im)^.*(compile error|Unable to resolve|Invalid memory access|Script assertion failed|Invalid VRAM access).*$')
				Add-Result 'Q1' 'Four-view scripts load and complete without engine errors' (-not $errors.Success -and $quadLog -match 'CHECK\|run complete\|') $(if ($errors.Success) { $errors.Value } else { '' })
				$quadChecks = @(@{ Name='party actors'; Id='Q2'; Title='All four actors are active' }, @{ Name='party camera'; Id='Q3'; Title='Four-way split is active' }, @{ Name='four independent cameras'; Id='Q4'; Title='Players three and four have independent cameras' }, @{ Name='party players spaced'; Id='Q5'; Title='Four-player visual capture has readable character spacing' }, @{ Name='party picture'; Id='Q6'; Title='Engine framebuffer captured' }, @{ Name='party alive'; Id='Q7'; Title='All four players remain alive' }, @{ Name='quad per-view scroll data'; Id='Q9'; Title='Each viewport retains its own line-scroll data' })
				foreach ($contract in $quadChecks) {
					$entry = [regex]::Match($quadLog, ('CHECK\|' + [regex]::Escape($contract.Name) + '\|(PASS|FAIL)\|(.*)'))
					Add-Result $contract.Id $contract.Title ($entry.Success -and $entry.Groups[1].Value -eq 'PASS') $(if ($entry.Success) { $entry.Groups[2].Value } else { 'not reported' })
				}
				$captureStats = Get-QuadCaptureStats (Join-Path $checksDir 'last_logs\Q4-party-quad.bmp')
				$allQuadrantsVisible = $null -ne $captureStats -and $captureStats.Width -ge 320 -and $captureStats.Height -ge 200 -and @($captureStats.Quadrants | Where-Object { $_ -lt 200 }).Count -eq 0
				Add-Result 'Q8' 'Captured frame has visible level pixels in all four quadrants' $allQuadrantsVisible $(if ($null -ne $captureStats) { 'sampled pixels: ' + ($captureStats.Quadrants -join '/') } else { 'engine framebuffer capture is missing' })
			}

			if (Wants "R")
			{
				Write-Step "R. Reconnect to the existing online session after a transport disconnect"
				$resumed = Invoke-OnlineJoin (New-SessionCode) @{ 'S3AIR_NETPLAY_RECONNECT_TEST' = '1' } 35 'R'
				$forced = $resumed.Log -match 'automated check disconnecting the transport'
				$restored = $resumed.Log -match 'reconnected and restored the existing session'
				$loadedCount = ([regex]::Matches($resumed.Log, 'game state loaded, now at frame')).Count
				Add-Result 'R1' 'A dropped friend reconnects and receives a fresh game snapshot' ($resumed.Reached -and $forced -and $restored -and $loadedCount -ge 2) ("forced disconnect: $forced, restored: $restored, snapshots: $loadedCount")
				$progress = @(Get-FriendFps | Where-Object { $_ -gt 0 })
				Add-Result 'R2' 'The resumed online game continues advancing without sync repairs' ($restored -and $progress.Count -gt 1 -and (Get-FriendRepairs) -eq 0) ("positive frame samples: " + $progress.Count + ", repairs: " + (Get-FriendRepairs))
			}

			if (Wants "E")
			{
			Write-Step "E. Online play with a bad connection (250 ms delay, 5 percent packet loss)"

			$lagged = Invoke-OnlineJoin (New-SessionCode) @{ "S3AIR_NET_LAG" = "250"; "S3AIR_NET_LOSS" = "5" } 25 "E1"
			$fps = @(Get-FriendFps)
			$minFps = if ($fps.Count -gt 0) { ($fps | Measure-Object -Minimum).Minimum } else { 0 }
			$lagRepairs = Get-FriendRepairs
			$lagAborted = $lagged.Log -match "aborting"
			Add-Result "E1" "Connection and game survive a bad connection" ($lagged.Reached -and -not $lagAborted) ("in game: " + $lagged.Reached + ", aborted: " + $lagAborted)
			Add-Result "E2" "Game keeps at least 30 frames per second with the bad connection" ($fps.Count -gt 0 -and $minFps -ge 30) $(if ($fps.Count -gt 0) { "lowest friend fps: " + [math]::Round($minFps, 1) + "; complete steady-state samples: " + $fps.Count } else { 'No complete steady-state FPS samples; performance could not be measured.' })
			Add-Result "E3" "No repairs needed with the bad connection" ($lagged.Reached -and ($lagRepairs -eq 0)) ("repairs: " + $lagRepairs)
			}

			# ---------------------------------------------------------------------------------------------------
			if (Wants "F")
			{
			Write-Step "F. Split screen, score, rings and the start of Angel Island (game scripts, one window)"

			$harnessTarget = Join-Path $appdata "mods\ChecksHarness"
			if (Test-Path $harnessTarget) { Remove-Item $harnessTarget -Recurse -Force }
			Copy-Item $harnessSource $harnessTarget -Recurse
			if ($Only -contains 'T') {
				$probeMain = Join-Path $harnessTarget 'scripts\main.lemon'
				$probeText = (Get-Content -Raw $probeMain).Replace('constant bool Checks.THREE_PLAYER_PROBE = false', 'constant bool Checks.THREE_PLAYER_PROBE = true')
				Set-Content -Path $probeMain -Value $probeText -Encoding utf8 -NoNewline
			}
			Set-ActiveMods @("ChecksHarness", "DropInCoop")
			# The checks start Angel Island 1 themselves, so no developer save state may be loaded instead
			$configText = $savedConfigText.Replace('"StartPhase": "2"', '"StartPhase": "3"')
			$configText = [regex]::Replace($configText, '(?m)^(\s*)"LoadSaveState": "([^"]*)",', '$1//"LoadSaveState": "$2",')
			Set-Content -Path $configPath -Value $configText -Encoding utf8
			Remove-Item $logFile -ErrorAction SilentlyContinue
			Stop-StartedProcesses

			$game1 = Start-GameProcess @{}
			$finishedRun = Wait-Until { (Read-SharedText $logFile) -match 'CHECK\|run complete\||Invalid memory access|Script assertion failed|Script compile error|Unable to resolve identifier' -or $game1.HasExited } 300
			$logText = Read-SharedText $logFile
			Stop-StartedProcesses

			$reports = @{}
			foreach ($line in @($logText -split "`r?`n"))
			{
				$match = [regex]::Match($line, 'CHECK\|([^|]*)\|(PASS|FAIL)\|(.*)$')
				if ($match.Success) { $reports[$match.Groups[1].Value] = [pscustomobject]@{ Passed = ($match.Groups[2].Value -eq "PASS"); Detail = $match.Groups[3].Value } }
			}

			$compileProblems = @([regex]::Matches($logText, '(?im)^.*(compile error|Unable to resolve|Unknown function|Parenthesis not matching|Caused in file|Failed to load|Invalid memory access|Script assertion failed|Invalid VRAM access).*$') | ForEach-Object { $_.Value.Trim() })
			Add-Result "F1" "Co-op scripts load in the game without errors" ($finishedRun -and $reports.ContainsKey("run complete") -and ($compileProblems.Count -eq 0)) `
				$(if ($compileProblems.Count -gt 0) { $compileProblems[0] } elseif (-not $finishedRun) { "the game did not finish the checks (check for an error window)" } else { "" })

			$gameChecks = @(
				@{ Id = "F2"; Name = "aiz start join"; Title = "Player 2 joins at the start of Angel Island while Sonic has not moved" },
				@{ Id = "F3"; Name = "own view from the start"; Title = "Player 2 has his own view from the start of Angel Island" },
				@{ Id = "F4"; Name = "score separate per player"; Title = "Score is kept separately for each player" },
				@{ Id = "F5"; Name = "rings separate per player"; Title = "Getting hurt only costs the rings of the player who got hurt" },
				@{ Id = "F6"; Name = "off screen ghost hidden while joined"; Title = "The off screen indicator for player 2 is hidden while he is playing" },
				@{ Id = "F7"; Name = "empty plane queue"; Title = "A clipped empty tile update emits no malformed queue entry" },
				@{ Id = "F8"; Name = "partner rescue"; Title = "Both players can request rescue with hold, cancel and cooldown" },
				@{ Id = "F9"; Name = "rescue safety"; Title = "Rescue rejects airborne, attached and scripted destinations" },
				@{ Id = "F10"; Name = "shared checkpoint"; Title = "Player 2 saves a shared checkpoint using their camera and layer" },
				@{ Id = "F11"; Name = "independent respawn"; Title = "Both players independently respawn at the shared checkpoint" },
				@{ Id = "F12"; Name = "boss regroup"; Title = "Boss entry regroups an off-screen partner once" },
				@{ Id = "F13"; Name = "team carry"; Title = "Tails carry requires consent and has an explicit release" },
				@{ Id = "F14"; Name = "character move controls"; Title = "Character moves use the configured inputs and velocity profiles" },
				@{ Id = "F15"; Name = "shared power-up eligibility"; Title = "Positive monitor effects can be shared without sharing Robotnik effects" },
				@{ Id = "F16"; Name = "coordinated boss hits"; Title = "Two different players complete a coordinated boss phase" },
				@{ Id = "F17"; Name = "shared monitor effect applied"; Title = "A shared rings monitor effect is applied only to player two" }
				@{ Id = "F18"; Name = "player two opens monitor"; Title = "Player two opens a monitor collision and owns its effect" }
			)
			if ($Only -contains 'T') {
				$gameChecks += @(
					@{ Id = 'T1'; Name = 'three player Sonic'; Title = 'Player 1 is Sonic' },
					@{ Id = 'T2'; Name = 'three player Tails'; Title = 'Player 2 is Tails' },
					@{ Id = 'T3'; Name = 'three player Knuckles'; Title = 'A third playable Knuckles exists alongside Sonic and Tails' }
				)
			}
			foreach ($check in $gameChecks)
			{
				if ($reports.ContainsKey($check.Name))
				{
					Add-Result $check.Id $check.Title $reports[$check.Name].Passed $reports[$check.Name].Detail
				}
				else
				{
					Add-Result $check.Id $check.Title $false "not reported by the game"
				}
			}
			Save-RunLogs "F"
			}

			# ---------------------------------------------------------------------------------------------------
			# Vanilla check (stage V): the IceCap 1 end teleport with only the game loaded (no co-op mod). Tells whether a crash there is the game's own.
			if (Wants "V")
			{
			Write-Step "V. Vanilla game (no co-op mod): IceCap 1 end teleport"
			$vanillaTarget = Join-Path $appdata "mods\TeleportHarness"
			if (Test-Path $vanillaTarget) { Remove-Item $vanillaTarget -Recurse -Force }
			Copy-Item (Join-Path $checksDir "harness\TeleportHarness") $vanillaTarget -Recurse
			$vanillaMain = Join-Path $vanillaTarget "scripts\main.lemon"
			Set-Content -Path $vanillaMain -Value (Get-Content -Raw $vanillaMain).Replace("__ZONE_ACT__", "0x0500") -Encoding utf8 -NoNewline
			Set-ActiveMods @("TeleportHarness")
			$configText = $savedConfigText.Replace('"StartPhase": "2"', '"StartPhase": "3"')
			$configText = [regex]::Replace($configText, '(?m)^(\s*)"LoadSaveState": "([^"]*)",', '$1//"LoadSaveState": "$2",')
			Set-Content -Path $configPath -Value $configText -Encoding utf8
			Remove-Item $logFile -ErrorAction SilentlyContinue
			Stop-StartedProcesses

			$vanillaGame = Start-GameProcess @{}
			$vanillaFinished = Wait-Until { $t = Read-SharedText $logFile; ($t -match 'CHECK\|run complete\|') -or ($t -match 'Invalid VRAM') -or $vanillaGame.HasExited } 150
			$vanillaLog = Read-SharedText $logFile
			Stop-StartedProcesses
			Set-Content -Path (Join-Path $checksDir "last_logs\V1-logfile.txt") -Value $vanillaLog -Encoding utf8
			$vanillaCrashed = $vanillaLog -match 'Invalid VRAM'
			$vanillaDone = $vanillaLog -match 'CHECK\|run complete\|'
			Add-Result "V1" "Vanilla game: the IceCap 1 end teleport runs without a VRAM error" ($vanillaDone -and -not $vanillaCrashed) $(if ($vanillaCrashed) { "the game itself crashes: " + [regex]::Match($vanillaLog, 'Invalid VRAM[^\r\n]*').Value } elseif (-not $vanillaDone) { "did not finish (an error window?)" } else { "" })
			}

			# Boss test (stage H): Sonic is moved to the end of each act and runs into the boss. Pass: the boss starts, the screen is one
			# shared view there, and both players are alive. Screenshots are saved next to the logs for a look at the picture.
			if ((Wants "H") -or (@($Only | Where-Object { $_ -match '^H\d\d$' }).Count -gt 0))
			{
			Write-Step "H. Boss at the end of each act (one game window per act, with screenshots)"

			$bossHarnessSource = Join-Path $checksDir "harness\BossHarness"
			$bossLogFolder = Join-Path $checksDir "last_logs"
			New-Item -ItemType Directory -Force -Path $bossLogFolder | Out-Null
			$bossTargets = @(
				@{ Id = "H01"; Code = "0x0001"; Name = "Angel Island 1" },
				@{ Id = "H02"; Code = "0x0001"; Name = "Angel Island 2" },
				@{ Id = "H03"; Code = "0x0100"; Name = "Hydrocity 1" },
				@{ Id = "H04"; Code = "0x0101"; Name = "Hydrocity 2" },
				@{ Id = "H05"; Code = "0x0200"; Name = "Marble Garden 1" },
				@{ Id = "H06"; Code = "0x0201"; Name = "Marble Garden 2" },
				@{ Id = "H07"; Code = "0x0300"; Name = "Carnival Night 1" },
				@{ Id = "H08"; Code = "0x0301"; Name = "Carnival Night 2" },
				@{ Id = "H09"; Code = "0x0400"; Name = "Flying Battery 1" },
				@{ Id = "H10"; Code = "0x0401"; Name = "Flying Battery 2" },
				@{ Id = "H11"; Code = "0x0501"; Apparent = "0x0500"; Name = "IceCap 1" },
				@{ Id = "H12"; Code = "0x0501"; Name = "IceCap 2" },
				@{ Id = "H13"; Code = "0x0600"; Name = "Launch Base 1" },
				@{ Id = "H14"; Code = "0x0601"; Name = "Launch Base 2" },
				@{ Id = "H15"; Code = "0x0700"; Name = "Mushroom Hill 1" },
				@{ Id = "H16"; Code = "0x0701"; Name = "Mushroom Hill 2" },
				@{ Id = "H17"; Code = "0x0800"; Name = "Sandopolis 1" },
				@{ Id = "H18"; Code = "0x0801"; Name = "Sandopolis 2" },
				@{ Id = "H19"; Code = "0x0900"; Name = "Lava Reef 1" },
				@{ Id = "H20"; Code = "0x1600"; Name = "Lava Reef boss" },
				@{ Id = "H21"; Code = "0x0a00"; Name = "Sky Sanctuary" },
				@{ Id = "H22"; Code = "0x0b00"; Name = "Death Egg 1" },
				@{ Id = "H23"; Code = "0x0b01"; Name = "Death Egg 2" }
			)
			$bossFilter = @($Only | Where-Object { $_ -match '^H\d\d$' })
			foreach ($act in $bossTargets)
			{
				if ($bossFilter.Count -gt 0 -and -not ($bossFilter -contains $act.Id)) { continue }
				$bossHarnessTarget = Join-Path $appdata "mods\BossHarness"
				if (Test-Path $bossHarnessTarget) { Remove-Item $bossHarnessTarget -Recurse -Force }
				Copy-Item $bossHarnessSource $bossHarnessTarget -Recurse
				$bossMainPath = Join-Path $bossHarnessTarget "scripts\main.lemon"
				$bossFixtures = Get-Content -Raw (Join-Path $checksDir 'boss_fixtures.json') | ConvertFrom-Json
				$bossFixture = $bossFixtures.($act.Id)
				if (-not $bossFixture) {
					Add-Result $act.Id ("Boss at the end of " + $act.Name) $false 'No source-derived approach fixture; boss coverage remains incomplete.'
					continue
				}
				Set-Content -Path $bossMainPath -Value (Get-Content -Raw $bossMainPath).Replace("__ZONE_ACT__", $act.Code).Replace("__END_FRAME__", "900").Replace('__BOSS_START_X__', $bossFixture.x).Replace('__BOSS_START_Y__', $bossFixture.y).Replace('__BOSS_ADDRESS__', $bossFixture.address).Replace('__BOSS_TARGET_X__', $bossFixture.targetX).Replace('__BOSS_TARGET_Y__', $bossFixture.targetY) -Encoding utf8 -NoNewline
				Set-ActiveMods @("BossHarness", "DropInCoop")
				$configText = $savedConfigText.Replace('"StartPhase": "2"', '"StartPhase": "3"')
				$configText = [regex]::Replace($configText, '(?m)^(\s*)"LoadSaveState": "([^"]*)",', '$1//"LoadSaveState": "$2",')
				Set-Content -Path $configPath -Value $configText -Encoding utf8
				Remove-Item $logFile -ErrorAction SilentlyContinue
				Stop-StartedProcesses

				$bossGame = Start-GameProcess @{ 'S3AIR_CHECK_CAPTURE_DIR' = (Join-Path $checksDir 'last_logs'); 'S3AIR_CHECK_CAPTURE_PREFIX' = $act.Id }
				$roamShot = $false
				$bossShot = $false
				$deadline = (Get-Date).AddSeconds(180)
				while ((Get-Date) -lt $deadline -and -not $bossGame.HasExited)
				{
					$bossText = Read-SharedText $logFile
					if ($bossText -match 'CHECK\|run complete\|') { break }
					if (-not $roamShot -and $bossText -match 'diag boss\|PASS\|f300 ')
					{
						$roamShot = Save-GameScreenshot $bossGame (Join-Path $bossLogFolder ($act.Id + "-roaming.png"))
					}
					if ($bossText -match 'Invalid VRAM|Script assertion failed|Script compile error|Unable to resolve identifier')
					{
						# The error window is shown on top of the game: keep its picture, the game cannot continue
						Start-Sleep -Milliseconds 800
						Save-ScreenPicture (Join-Path $bossLogFolder ($act.Id + "-error.png"))
						break
					}
					if (-not $bossShot -and $bossText -match 'diag boss\|PASS\|f\d+ boss 1 [^\r\n]*hits[1-9]')
					{
						$bossShot = Save-GameScreenshot $bossGame (Join-Path $bossLogFolder ($act.Id + "-boss.png"))
					}
					Start-Sleep -Milliseconds 300
				}
				$bossFinished = ((Read-SharedText $logFile) -match 'CHECK\|run complete\|')
				$bossLog = Read-SharedText $logFile
				Stop-StartedProcesses
				Set-Content -Path (Join-Path $bossLogFolder ($act.Id + "-logfile.txt")) -Value $bossLog -Encoding utf8

				$bossMatch = [regex]::Match($bossLog, 'CHECK\|boss test\|(PASS|FAIL)\|(.*)')
				$bossErrors = @([regex]::Matches($bossLog, '(?im)^.*(Invalid VRAM access|Script assertion failed|Caused during|compile error|Unable to resolve|Failed to load).*$') | ForEach-Object { $_.Value.Trim() })
				if ($bossMatch.Success)
				{
					$bossPassed = ($bossMatch.Groups[1].Value -eq "PASS") -and ($bossErrors.Count -eq 0)
					$bossDetail = $bossMatch.Groups[2].Value
					if ($bossErrors.Count -gt 0) { $bossDetail = $bossErrors[0] + " -- " + $bossDetail }
				}
				else
				{
					$bossPassed = $false
					$bossDetail = $(if ($bossErrors.Count -gt 0) { $bossErrors[0] } elseif (-not $bossFinished) { "the game did not finish (an error window?)" } else { "no result reported" })
				}
					Add-Result $act.Id ("Boss scene in " + $act.Name + " (four players, one shared screen; route not covered)") $bossPassed $bossDetail
					$captureMatch = [regex]::Match($bossLog, 'CHECK\|boss render capture\|(PASS|FAIL)\|(.*)')
					Add-Result ($act.Id + 'V') ($act.Name + ': engine framebuffer captured during the boss view') ($captureMatch.Success -and $captureMatch.Groups[1].Value -eq 'PASS') $(if ($captureMatch.Success) { $captureMatch.Groups[2].Value } else { 'boss view capture was not reported' })
			}
			}

			# -Only G04,G11 runs just those acts of stage G
			if ((Wants "G") -or (@($Only | Where-Object { $_ -match '^G\d\d$' }).Count -gt 0))
			{
			Write-Step "G. Split screen in every act (one game window per act)"

			$actHarnessSource = Join-Path $checksDir "harness\ActHarness"
			$actTargets = @(
				@{ Id = "G01"; Code = "0x0000"; Name = "Angel Island 1" },
				@{ Id = "G02"; Code = "0x0001"; Name = "Angel Island 2" },
				@{ Id = "G03"; Code = "0x0100"; Name = "Hydrocity 1" },
				@{ Id = "G04"; Code = "0x0101"; Name = "Hydrocity 2"; End = 1400; TeleportX = "0x0d00"; TeleportY = "0x0400" },
				@{ Id = "G05"; Code = "0x0200"; Name = "Marble Garden 1" },
				@{ Id = "G06"; Code = "0x0201"; Name = "Marble Garden 2" },
				@{ Id = "G07"; Code = "0x0300"; Name = "Carnival Night 1" },
				@{ Id = "G08"; Code = "0x0301"; Name = "Carnival Night 2" },
				@{ Id = "G09"; Code = "0x0400"; Name = "Flying Battery 1" },
				@{ Id = "G10"; Code = "0x0401"; Name = "Flying Battery 2" },
				@{ Id = "G11"; Code = "0x0500"; Name = "IceCap 1"; End = 2200 },
				@{ Id = "G12"; Code = "0x0501"; Name = "IceCap 2" },
				@{ Id = "G13"; Code = "0x0600"; Name = "Launch Base 1" },
				@{ Id = "G14"; Code = "0x0601"; Name = "Launch Base 2" },
				@{ Id = "G15"; Code = "0x0700"; Name = "Mushroom Hill 1" },
				@{ Id = "G16"; Code = "0x0701"; Name = "Mushroom Hill 2" },
				@{ Id = "G17"; Code = "0x0800"; Name = "Sandopolis 1" },
				@{ Id = "G18"; Code = "0x0801"; Name = "Sandopolis 2" },
				@{ Id = "G19"; Code = "0x0900"; Name = "Lava Reef 1" },
				@{ Id = "G20"; Code = "0x0901"; Name = "Lava Reef 2" },
				@{ Id = "G21"; Code = "0x0a00"; Name = "Sky Sanctuary" },
				@{ Id = "G22"; Code = "0x0b00"; Name = "Death Egg 1" },
				@{ Id = "G23"; Code = "0x0b01"; Name = "Death Egg 2" },
				@{ Id = "G24"; Code = "0x1601"; Name = "Hidden Palace" },
				@{ Id = "G25"; Code = "0x1600"; Name = "Lava Reef boss"; Boss = $true },
				@{ Id = "G26"; Code = "0x1700"; Name = "Death Egg boss"; Boss = $true }
			)
			foreach ($act in $actTargets)
			{
				$actFilter = @($Only | Where-Object { $_ -match '^G\d\d$' })
				if ($actFilter.Count -gt 0 -and -not ($actFilter -contains $act.Id)) { continue }
				$actHarnessTarget = Join-Path $appdata "mods\ActHarness"
				if (Test-Path $actHarnessTarget) { Remove-Item $actHarnessTarget -Recurse -Force }
				Copy-Item $actHarnessSource $actHarnessTarget -Recurse
				$actMainPath = Join-Path $actHarnessTarget "scripts\main.lemon"
				Set-Content -Path $actMainPath -Value (Get-Content -Raw $actMainPath).Replace("__ZONE_ACT__", $act.Code).Replace("__EXPECT_SPLIT__", $(if ($act.Boss) { "0" } else { "1" })).Replace("__END_FRAME__", $(if ($act.End) { [string]$act.End } else { "560" })).Replace("__TELEPORT_ON__", $(if ($act.TeleportX) { "1" } else { "0" })).Replace("__TELEPORT_X__", $(if ($act.TeleportX) { $act.TeleportX } else { "0" })).Replace("__TELEPORT_Y__", $(if ($act.TeleportY) { $act.TeleportY } else { "0" })) -Encoding utf8 -NoNewline
				Set-ActiveMods @("ActHarness", "DropInCoop")
				# Same start as the F stage: no developer save state, the level is started directly
				$configText = $savedConfigText.Replace('"StartPhase": "2"', '"StartPhase": "3"')
				$configText = [regex]::Replace($configText, '(?m)^(\s*)"LoadSaveState": "([^"]*)",', '$1//"LoadSaveState": "$2",')
				Set-Content -Path $configPath -Value $configText -Encoding utf8
				Remove-Item $logFile -ErrorAction SilentlyContinue
				Stop-StartedProcesses

				$actGame = Start-GameProcess @{ 'S3AIR_CHECK_CAPTURE_DIR' = (Join-Path $checksDir 'last_logs'); 'S3AIR_CHECK_CAPTURE_PREFIX' = $act.Id }
				# Waits for the end of the check; takes a screenshot of the roaming part (split screen expected) and of any error window
				$actFinished = $false
				$actShot = $false
				$actDeadline = (Get-Date).AddSeconds(150)
				while ((Get-Date) -lt $actDeadline -and -not $actGame.HasExited)
				{
					$actText = Read-SharedText $logFile
					if ($actText -match 'CHECK\|run complete\|') { $actFinished = $true; break }
					if ($actText -match 'Invalid VRAM')
					{
						Start-Sleep -Milliseconds 800
						Save-ScreenPicture (Join-Path $checksDir ("last_logs\" + $act.Id + "-error.png"))
						break
					}
					if (-not $actShot -and $actText -match 'diag level\|PASS\|[^\r\n]* split 1')
					{
						$actShot = Save-GameScreenshot $actGame (Join-Path $checksDir ("last_logs\" + $act.Id + "-roaming.png"))
					}
					Start-Sleep -Milliseconds 300
				}
				$actLog = Read-SharedText $logFile
				Stop-StartedProcesses

				$actMatch = [regex]::Match($actLog, 'CHECK\|split screen\|(PASS|FAIL)\|(.*)')
				$actErrors = @([regex]::Matches($actLog, '(?im)^.*(Invalid VRAM access|Script assertion failed|Caused during|compile error|Unable to resolve|Failed to load).*$') | ForEach-Object { $_.Value.Trim() })
				if ($actMatch.Success)
				{
					$actPassed = ($actMatch.Groups[1].Value -eq "PASS") -and ($actErrors.Count -eq 0)
					$actDetail = $actMatch.Groups[2].Value
					if ($actErrors.Count -gt 0) { $actDetail = $actErrors[0] + " -- " + $actDetail }
				}
				else
				{
					$actPassed = $false
					$actDetail = $(if ($actErrors.Count -gt 0) { $actErrors[0] } elseif (-not $actFinished) { "the game did not finish (an error window?)" } else { "no result reported" })
				}
				$actTitle = $(if ($act.Boss) { "Boss arena in " + $act.Name + " is one shared screen, player 2 keeps playing" } else { "Split screen in " + $act.Name })
				Add-Result $act.Id $actTitle $actPassed $actDetail
				if (-not $act.Boss) {
					foreach ($contract in @(@{ Name='boss screen contract'; Id='B'; Title='Encounter input switches to shared screen' }, @{ Name='resume screen contract'; Id='R'; Title='Roaming view resumes after encounter input clears' }, @{ Name='render capture'; Id='P'; Title='Engine framebuffer captured' })) {
						$contractMatch = [regex]::Match($actLog, ('CHECK\|' + [regex]::Escape($contract.Name) + '\|(PASS|FAIL)\|(.*)'))
						Add-Result ($act.Id + $contract.Id) ($act.Name + ': ' + $contract.Title) ($contractMatch.Success -and $contractMatch.Groups[1].Value -eq 'PASS') $(if ($contractMatch.Success) { $contractMatch.Groups[2].Value } else { 'not reported by the game' })
					}
				}
				# Each act's own game log (the diagnostics every half second are in it)
				$actLogFolder = Join-Path $checksDir "last_logs"
				New-Item -ItemType Directory -Force -Path $actLogFolder | Out-Null
				Set-Content -Path (Join-Path $actLogFolder ($act.Id + "-logfile.txt")) -Value $actLog -Encoding utf8
			}
			}

			if ((Wants 'QG') -or (@($Only | Where-Object { $_ -match '^QG\d\d$' }).Count -gt 0))
			{
				Write-Step 'QG. Four-player view coverage in every act (shared-camera fallback where needed)'
				$quadTargets = @(
					@{ Id='QG01'; Code='0x0000'; Name='Angel Island 1'; End=2200 }, @{ Id='QG02'; Code='0x0001'; Name='Angel Island 2' },
					@{ Id='QG03'; Code='0x0100'; Name='Hydrocity 1' }, @{ Id='QG04'; Code='0x0101'; Name='Hydrocity 2'; End=1400; TeleportX='0x0d00'; TeleportY='0x0400' },
					@{ Id='QG05'; Code='0x0200'; Name='Marble Garden 1' }, @{ Id='QG06'; Code='0x0201'; Name='Marble Garden 2' },
					@{ Id='QG07'; Code='0x0300'; Name='Carnival Night 1' }, @{ Id='QG08'; Code='0x0301'; Name='Carnival Night 2' },
					@{ Id='QG09'; Code='0x0400'; Name='Flying Battery 1' }, @{ Id='QG10'; Code='0x0401'; Name='Flying Battery 2' },
					@{ Id='QG11'; Code='0x0500'; Name='IceCap 1'; End=2200 }, @{ Id='QG12'; Code='0x0501'; Name='IceCap 2' },
					@{ Id='QG13'; Code='0x0600'; Name='Launch Base 1' }, @{ Id='QG14'; Code='0x0601'; Name='Launch Base 2' },
					@{ Id='QG15'; Code='0x0700'; Name='Mushroom Hill 1' }, @{ Id='QG16'; Code='0x0701'; Name='Mushroom Hill 2' },
					@{ Id='QG17'; Code='0x0800'; Name='Sandopolis 1' }, @{ Id='QG18'; Code='0x0801'; Name='Sandopolis 2' },
					@{ Id='QG19'; Code='0x0900'; Name='Lava Reef 1' }, @{ Id='QG20'; Code='0x0901'; Name='Lava Reef 2' },
					@{ Id='QG21'; Code='0x0a00'; Name='Sky Sanctuary' }, @{ Id='QG22'; Code='0x0b00'; Name='Death Egg 1' },
					@{ Id='QG23'; Code='0x0b01'; Name='Death Egg 2' }, @{ Id='QG24'; Code='0x1601'; Name='Hidden Palace' }
				)
				$quadActFilter = @($Only | Where-Object { $_ -match '^QG\d\d$' })
				foreach ($act in $quadTargets)
				{
					if ($quadActFilter.Count -gt 0 -and -not ($quadActFilter -contains $act.Id)) { continue }
					$quadActTarget = Join-Path $appdata 'mods\ActHarness'
					if (Test-Path $quadActTarget) { Remove-Item -LiteralPath $quadActTarget -Recurse -Force }
					Copy-Item (Join-Path $checksDir 'harness\ActHarness') $quadActTarget -Recurse
					$quadActMain = Join-Path $quadActTarget 'scripts\main.lemon'
					$quadActText = (Get-Content -Raw $quadActMain).Replace('constant bool QUAD_ACT_TEST = false', 'constant bool QUAD_ACT_TEST = true').Replace('__ZONE_ACT__', $act.Code).Replace('__EXPECT_SPLIT__', $(if ($act.Boss) { '0' } else { '1' })).Replace('__END_FRAME__', $(if ($act.End) { [string]$act.End } else { '560' })).Replace('__TELEPORT_ON__', $(if ($act.TeleportX) { '1' } else { '0' })).Replace('__TELEPORT_X__', $(if ($act.TeleportX) { $act.TeleportX } else { '0' })).Replace('__TELEPORT_Y__', $(if ($act.TeleportY) { $act.TeleportY } else { '0' }))
					Set-Content -Path $quadActMain -Value $quadActText -Encoding utf8 -NoNewline
					Set-ActiveMods @('ActHarness', 'DropInCoop')
					$configText = $savedConfigText.Replace('"StartPhase": "2"', '"StartPhase": "3"')
					$configText = [regex]::Replace($configText, '(?m)^(\s*)"LoadSaveState": "([^"]*)",', '$1//"LoadSaveState": "$2",')
					Set-Content -Path $configPath -Value $configText -Encoding utf8
					Remove-Item $logFile -ErrorAction SilentlyContinue
					Stop-StartedProcesses
					$quadActGame = Start-GameProcess @{ 'S3AIR_CHECK_CAPTURE_DIR' = (Join-Path $checksDir 'last_logs'); 'S3AIR_CHECK_CAPTURE_PREFIX' = $act.Id }
					$quadActFinished = Wait-Until { (Read-SharedText $logFile) -match 'CHECK\|run complete\||Invalid VRAM|Script assertion failed|Script compile error|Unable to resolve identifier' -or $quadActGame.HasExited } 180
					$quadActLog = Read-SharedText $logFile
					Stop-StartedProcesses
					Set-Content -Path (Join-Path $checksDir ('last_logs\' + $act.Id + '-logfile.txt')) -Value $quadActLog -Encoding utf8
					$quadActMatch = [regex]::Match($quadActLog, 'CHECK\|split screen\|(PASS|FAIL)\|(.*)')
					$quadActErrors = [regex]::Match($quadActLog, '(?im)^.*(Invalid VRAM access|Script assertion failed|compile error|Unable to resolve|Failed to load).*$')
					$quadActPassed = $quadActMatch.Success -and $quadActMatch.Groups[1].Value -eq 'PASS' -and -not $quadActErrors.Success
					$quadActDetail = if ($quadActErrors.Success) { $quadActErrors.Value } elseif ($quadActMatch.Success) { $quadActMatch.Groups[2].Value } elseif (-not $quadActFinished) { 'four-player scene run did not finish' } else { 'four-player scene result missing' }
					Add-Result $act.Id ('Four-player scene: ' + $act.Name) $quadActPassed $quadActDetail
					if ($quadActMatch.Success -and $quadActMatch.Groups[2].Value -match 'quad 1 req 1 players 4 frames \d+ seen 1')
					{
						$quadImage = Join-Path $checksDir ('last_logs\' + $act.Id + '-roaming.bmp')
						$quadPixels = Get-QuadCaptureStats $quadImage
						$quadPicturePassed = $null -ne $quadPixels -and @($quadPixels.Quadrants | Where-Object { $_ -lt 200 }).Count -eq 0
						Add-Result ($act.Id + 'V') ($act.Name + ': four rendered quadrants') $quadPicturePassed $(if ($null -ne $quadPixels) { $quadPixels.Quadrants -join '/' } else { 'missing framebuffer capture' })
					}
				}
			}

			if (Wants 'Z')
			{
				Write-Step 'Z. AIZ zip line remains loaded and rendered in P2-only view'
				$ziplineTarget = Join-Path $appdata 'mods\ActHarness'
				if (Test-Path -LiteralPath $ziplineTarget) { Remove-Item -LiteralPath $ziplineTarget -Recurse -Force }
				Copy-Item (Join-Path $checksDir 'harness\ActHarness') $ziplineTarget -Recurse
				$ziplineMain = Join-Path $ziplineTarget 'scripts\main.lemon'
				$ziplineText = (Get-Content -Raw $ziplineMain).Replace('constant bool ZIPLINE_TEST = false', 'constant bool ZIPLINE_TEST = true').Replace('__ZONE_ACT__', '0x0000').Replace('__EXPECT_SPLIT__', '1').Replace('__END_FRAME__', '4500').Replace('__TELEPORT_ON__', '0').Replace('__TELEPORT_X__', '0').Replace('__TELEPORT_Y__', '0')
				Set-Content -Path $ziplineMain -Value $ziplineText -Encoding utf8 -NoNewline
				Set-ActiveMods @('ActHarness', 'DropInCoop')
				$configText = $savedConfigText.Replace('"StartPhase": "2"', '"StartPhase": "3"')
				$configText = [regex]::Replace($configText, '(?m)^(\s*)"LoadSaveState": "([^"]*)",', '$1//"LoadSaveState": "$2",')
				Set-Content -Path $configPath -Value $configText -Encoding utf8
				Remove-Item $logFile -ErrorAction SilentlyContinue
				Stop-StartedProcesses
				$ziplineGame = Start-GameProcess @{ 'S3AIR_CHECK_CAPTURE_DIR' = (Join-Path $checksDir 'last_logs'); 'S3AIR_CHECK_CAPTURE_PREFIX' = 'Z1' }
				$ziplineFinished = Wait-Until { (Read-SharedText $logFile) -match 'CHECK\|run complete\||Invalid VRAM|Script assertion failed|Script compile error|Unable to resolve identifier' -or $ziplineGame.HasExited } 180
				$ziplineLog = Read-SharedText $logFile
				Stop-StartedProcesses
				Set-Content -Path (Join-Path $checksDir 'last_logs\Z1-logfile.txt') -Value $ziplineLog -Encoding utf8
				$ziplineErrors = [regex]::Match($ziplineLog, '(?im)^.*(Invalid VRAM access|Script assertion failed|compile error|Unable to resolve|Failed to load).*$')
				$ziplineMatch = [regex]::Match($ziplineLog, 'CHECK\|zipline p2-only view\|(PASS|FAIL)\|(.*)')
				$ziplinePassed = $ziplineMatch.Success -and $ziplineMatch.Groups[1].Value -eq 'PASS' -and -not $ziplineErrors.Success
				$ziplineDetail = if ($ziplineErrors.Success) { $ziplineErrors.Value } elseif ($ziplineMatch.Success) { $ziplineMatch.Groups[2].Value } elseif (-not $ziplineFinished) { 'zip-line scene did not finish' } else { 'zip-line result missing' }
				Add-Result 'Z1' 'AIZ zip-line objects render in P2 view while outside P1 view' $ziplinePassed $ziplineDetail
				$captureMatch = [regex]::Match($ziplineLog, 'CHECK\|zipline framebuffer\|(PASS|FAIL)\|(.*)')
				Add-Result 'Z1V' 'Engine framebuffer captured with P2 centered on the zip line' ($captureMatch.Success -and $captureMatch.Groups[1].Value -eq 'PASS') $(if ($captureMatch.Success) { $captureMatch.Groups[2].Value } else { 'framebuffer capture was not reported' })
			}
		}
	}
	finally
	{
		Stop-StartedProcesses
		if ($null -ne $savedModsText) { Set-Content -Path $savedMods -Value $savedModsText -Encoding utf8 } elseif (Test-Path $savedMods) { Remove-Item $savedMods -Force }
		Set-Content -Path $configPath -Value $savedConfigText -Encoding utf8 -NoNewline
		if (Test-Path -LiteralPath $recoveryPath) { Remove-Item -LiteralPath $recoveryPath -Force }
		$harnessInstalled = Join-Path $appdata "mods\ChecksHarness"
		if (Test-Path $harnessInstalled) { Remove-Item $harnessInstalled -Recurse -Force }
		$vanillaInstalled = Join-Path $appdata "mods\TeleportHarness"
		if (Test-Path $vanillaInstalled) { Remove-Item $vanillaInstalled -Recurse -Force }
		$bossHarnessInstalled = Join-Path $appdata "mods\BossHarness"
		if (Test-Path $bossHarnessInstalled) { Remove-Item $bossHarnessInstalled -Recurse -Force }
		$actHarnessInstalled = Join-Path $appdata "mods\ActHarness"
		if (Test-Path $actHarnessInstalled) { Remove-Item $actHarnessInstalled -Recurse -Force }
		$partyHarnessInstalled = Join-Path $appdata 'mods\PartyHarness'
		if (Test-Path $partyHarnessInstalled) { Remove-Item -LiteralPath $partyHarnessInstalled -Recurse -Force }
		$netPartyHarnessInstalled = Join-Path $appdata 'mods\NetPartyHarness'
		if (Test-Path $netPartyHarnessInstalled) { Remove-Item -LiteralPath $netPartyHarnessInstalled -Recurse -Force }
		if (Test-Path -LiteralPath $testHooksMarker) { Remove-Item -LiteralPath $testHooksMarker -Force }
		Write-Host ""
		Write-Host "Your mod list, config and settings were put back." -ForegroundColor DarkGray
	}
}
catch
{
	Add-Result "X1" "The check run completed" $false $_.Exception.Message
}

# ---------------------------------------------------------------------------------------------------------------
# Report

$passedCount = @($results | Where-Object { $_.Passed }).Count
$total = $results.Count
$allPassed = ($passedCount -eq $total) -and ($total -gt 0)

$lines = @()
$lines += "Sonic 3 A.I.R. Drop-In Co-op automated checks - " + (Get-Date -Format "yyyy-MM-dd HH:mm")
$lines += ""
foreach ($result in $results)
{
	$mark = if ($result.Passed) { "PASS" } else { "FAIL" }
	$lines += ($mark + "  " + $result.Id.PadRight(4) + " " + $result.Title + $(if ($result.Detail) { "  -- " + $result.Detail } else { "" }))
}
$lines += ""
$lines += ("Result: " + $passedCount + " of " + $total + " passed. " + $(if ($allPassed) { "EXECUTED CHECKS PASSED. Coverage limits are listed in CHECKLIST.md." } else { "NOT READY, see the FAIL lines." }))
Set-Content -Path $reportPath -Value $lines -Encoding utf8

Write-Host ""
Write-Host ("Result: " + $passedCount + " of " + $total + " checks passed.") -ForegroundColor $(if ($allPassed) { "Green" } else { "Red" })
Write-Host ("Report: " + $reportPath)
Write-Host ""

$checkRunMutex.ReleaseMutex()
$checkRunMutex.Dispose()
if ($allPassed) { exit 0 } else { exit 1 }
