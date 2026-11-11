# Stage T: checks of the in-game updater and the release tooling.
#
#   checks\run_update_checks.ps1            (needs the built game and the package folder: run checks\build_online_stable.ps1 and checks\make_package.ps1 first)
#
# H  the install helper (update_helper.ps1) alone, on throw-away folders: happy path, protected files, rollback, locked program file, bad staging
# T  the real game (Sonic3AIR.exe of the package, in a copy of the package folder) against a loopback test server that plays GitHub:
#    happy path, wrong hash, zip-slip, forbidden content, same version, older version, no network, rate limit, partial download, cancel
# R  release.ps1 -DryRun, and the real release zip installed over an older copy
# Only one thing runs at a time; the test server listens on 127.0.0.1 only and is stopped at the end.
param(
	[string[]]$Only = @(),		# for example: -Only H  or  -Only T,R
	[switch]$Offline,			# skip T14 (two read-only requests to the real api.github.com)
	[switch]$SkipReal,			# skip R2 (installing the real release zip, which takes a few minutes)
	[switch]$KeepWork,			# for looking into a failure: keep the work folder and the updater files in the data folder
	[string]$OnlyCase = ""		# with -Only T: run only the happy path (T1), for looking into it
)

$ErrorActionPreference = "Stop"
$checksDir = $PSScriptRoot
$root = Split-Path -Parent $checksDir
$packageDir = Join-Path $root "Sonic3AIR_Online"
$helperSource = Join-Path $checksDir "package_files\update_helper.ps1"
$serverScript = Join-Path $checksDir "update_test_server.ps1"
$appdata = Join-Path $env:APPDATA "Sonic3AIR"
$updateDir = Join-Path $appdata "update"
$lastLogs = Join-Path $checksDir "last_logs"
$work = Join-Path $lastLogs "update_check_work"
$reportPath = Join-Path $checksDir "update_checks_report.txt"
$results = New-Object System.Collections.ArrayList
$startedProcesses = New-Object System.Collections.ArrayList
$modsBackup = Join-Path $lastLogs "update_check_mods_backup"		# the developer's own installed DropInCoop, put back after R2 (the real release installs its bundled copy there)

function Restore-DeveloperMod
{
	$modInstalled = Join-Path $appdata "mods\DropInCoop"
	if (Test-Path -LiteralPath (Join-Path $modsBackup "DropInCoop"))
	{
		if (Test-Path -LiteralPath $modInstalled) { Remove-Item -LiteralPath $modInstalled -Recurse -Force }
		Copy-Item -LiteralPath (Join-Path $modsBackup "DropInCoop") -Destination $modInstalled -Recurse -Force
		Remove-Item -LiteralPath $modsBackup -Recurse -Force
	}
}
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

function Q([string]$text) { return ([char]34 + $text + [char]34) }

function Wants([string]$id) { return ($Only.Count -eq 0) -or ($Only -contains $id) }

function Add-Result([string]$id, [string]$title, [bool]$passed, [string]$detail)
{
	[void]$results.Add([pscustomobject]@{ Id = $id; Title = $title; Passed = $passed; Detail = $detail })
	$mark = if ($passed) { "PASS" } else { "FAIL" }
	$color = if ($passed) { "Green" } else { "Red" }
	Write-Host ("  [" + $mark + "] " + $id + "  " + $title + $(if ($detail) { "  -- " + $detail } else { "" })) -ForegroundColor $color
}

function Write-Step([string]$text) { Write-Host ""; Write-Host $text -ForegroundColor Cyan }

function Read-SharedText([string]$path)
{
	if (-not (Test-Path -LiteralPath $path)) { return "" }
	try
	{
		$stream = [IO.File]::Open($path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
		try { return (New-Object IO.StreamReader($stream)).ReadToEnd() } finally { $stream.Dispose() }
	}
	catch { return "" }
}

function Wait-Until([scriptblock]$condition, [int]$seconds)
{
	$deadline = (Get-Date).AddSeconds($seconds)
	while ((Get-Date) -lt $deadline) { if (& $condition) { return $true }; Start-Sleep -Milliseconds 400 }
	return $false
}

function Get-Sha256([string]$path)
{
	$sha = [Security.Cryptography.SHA256]::Create()
	try
	{
		$stream = [IO.File]::Open($path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
		try { return ([BitConverter]::ToString($sha.ComputeHash($stream)).Replace("-", "").ToLowerInvariant()) } finally { $stream.Dispose() }
	}
	finally { $sha.Dispose() }
}

function Get-BytesSha256([byte[]]$bytes)
{
	$sha = [Security.Cryptography.SHA256]::Create()
	try { return ([BitConverter]::ToString($sha.ComputeHash($bytes)).Replace("-", "").ToLowerInvariant()) } finally { $sha.Dispose() }
}

# "path|size|sha256" of every file of a folder (sorted), as one string; $exclude are regular expressions on the relative path
function Get-TreeSignature([string]$dir, [string[]]$exclude = @())
{
	$base = (Resolve-Path -LiteralPath $dir).Path.TrimEnd('\') + '\'
	$lines = New-Object System.Collections.ArrayList
	foreach ($file in (Get-ChildItem -LiteralPath $dir -Recurse -File -Force | Sort-Object FullName))
	{
		$relative = $file.FullName.Substring($base.Length).Replace('\', '/')
		$skip = $false
		foreach ($pattern in $exclude) { if ($relative -match $pattern) { $skip = $true; break } }
		if ($skip) { continue }
		[void]$lines.Add($relative + "|" + $file.Length + "|" + (Get-Sha256 $file.FullName))
	}
	return ($lines -join "`n")
}

function Remove-CheckedTree([string]$path)
{
	# Only ever deletes below this stage's own work folder
	$full = [IO.Path]::GetFullPath($path)
	$allowed = [IO.Path]::GetFullPath($work).TrimEnd('\') + '\'
	if (-not ($full + '\').StartsWith($allowed, [StringComparison]::OrdinalIgnoreCase) -and $full -ne [IO.Path]::GetFullPath($work)) { throw "refusing to delete outside the work folder: $full" }
	if (Test-Path -LiteralPath $full) { Remove-Item -LiteralPath $full -Recurse -Force }
}

function Stop-TestGames
{
	foreach ($process in @(Get-Process -Name "Sonic3AIR", "AirNet" -ErrorAction SilentlyContinue))
	{
		$path = $null
		try { $path = $process.Path } catch { }
		if ($path -and $path.StartsWith($work, [StringComparison]::OrdinalIgnoreCase)) { Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue }
	}
	Start-Sleep -Milliseconds 800
}

function Stop-StartedProcesses
{
	foreach ($process in $startedProcesses) { if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue } }
	$startedProcesses.Clear()
	Stop-TestGames
}

# ---------------------------------------------------------------------------------------------------------------------------------
# Test installs and test packages
# ---------------------------------------------------------------------------------------------------------------------------------
function New-OldInstall([string]$dir)
{
	Remove-CheckedTree $dir
	& robocopy $packageDir $dir /E /NFL /NDL /NJH /NJS /NP /R:1 /W:1 | Out-Null
	if ($LASTEXITCODE -ge 8) { throw "robocopy failed ($LASTEXITCODE)" }
	Set-Content -LiteralPath (Join-Path $dir "version.txt") -Value "1.0.0" -Encoding ascii -NoNewline
	Set-Content -LiteralPath (Join-Path $dir "testhooks.enabled") -Value "test hooks" -Encoding ascii
	if (-not (Test-Path -LiteralPath (Join-Path $dir "update_helper.ps1"))) { Copy-Item -LiteralPath $helperSource -Destination (Join-Path $dir "update_helper.ps1") }
	Set-Content -LiteralPath (Join-Path $dir "data\update_test_changed.txt") -Value "old content" -Encoding ascii
	Set-Content -LiteralPath (Join-Path $dir "data\update_test_removed.txt") -Value "to be removed by the update" -Encoding ascii
	Set-Content -LiteralPath (Join-Path $dir "data\my_user_file.txt") -Value "a file of the player, not part of any package" -Encoding ascii
	New-Item -ItemType Directory -Path (Join-Path $dir "saves") -Force | Out-Null
	Set-Content -LiteralPath (Join-Path $dir "saves\precious.sav") -Value "my save game" -Encoding ascii
	New-Item -ItemType Directory -Path (Join-Path $dir "mods\MyUserMod") -Force | Out-Null
	Set-Content -LiteralPath (Join-Path $dir "mods\MyUserMod\mod.json") -Value '{"Title":"my mod"}' -Encoding ascii
	Set-Content -LiteralPath (Join-Path $dir "installed_files.txt") -Value @("Sonic3AIR.exe", "version.txt", "update_helper.ps1", "config.json", "data/update_test_changed.txt", "data/update_test_removed.txt") -Encoding ascii
}

# Builds a zip with exact control over the entry names. $entries: ordered list of @{ Name = ...; Bytes = ... }
function New-ZipFile([string]$zipPath, $entries)
{
	if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath -Force }
	New-Item -ItemType Directory -Path (Split-Path -Parent $zipPath) -Force | Out-Null
	$stream = [IO.File]::Open($zipPath, [IO.FileMode]::Create)
	try
	{
		$archive = New-Object IO.Compression.ZipArchive($stream, [IO.Compression.ZipArchiveMode]::Create)
		try
		{
			foreach ($entry in $entries)
			{
				$zipEntry = $archive.CreateEntry($entry.Name, [IO.Compression.CompressionLevel]::Fastest)
				$entryStream = $zipEntry.Open()
				try { if ($entry.Bytes.Length -gt 0) { $entryStream.Write($entry.Bytes, 0, $entry.Bytes.Length) } } finally { $entryStream.Dispose() }
			}
		}
		finally { $archive.Dispose() }
	}
	finally { $stream.Dispose() }
}

function Text-Bytes([string]$text) { return [Text.Encoding]::ASCII.GetBytes($text) }

$script:newExeBytes = $null
function Get-NewExeBytes
{
	if ($null -eq $script:newExeBytes)
	{
		$old = [IO.File]::ReadAllBytes((Join-Path $packageDir "Sonic3AIR.exe"))
		$marker = Text-Bytes "UPDATE-TEST-MARKER-1.1.0"
		$script:newExeBytes = New-Object byte[] ($old.Length + $marker.Length)
		[Array]::Copy($old, $script:newExeBytes, $old.Length)
		[Array]::Copy($marker, 0, $script:newExeBytes, $old.Length, $marker.Length)
	}
	return $script:newExeBytes
}

# A good update package (the layout Compress-Archive makes: one top folder). $extra: more entries, $without: names (after the folder) to leave out
function New-GoodPackageEntries([string]$version, $extra = @(), [string[]]$without = @())
{
	$prefix = "Sonic3AIR_Online/"
	$configText = "// config of the new package`n" + [IO.File]::ReadAllText((Join-Path $packageDir "config.json"))
	$entries = @(
		@{ Name = $prefix + "Sonic3AIR.exe"; Bytes = (Get-NewExeBytes) },
		@{ Name = $prefix + "version.txt"; Bytes = (Text-Bytes $version) },
		@{ Name = $prefix + "update_helper.ps1"; Bytes = [IO.File]::ReadAllBytes($helperSource) },
		@{ Name = $prefix + "config.json"; Bytes = (Text-Bytes $configText) },
		@{ Name = $prefix + "data/"; Bytes = @() },
		@{ Name = $prefix + "data/update_test_new.txt"; Bytes = (Text-Bytes "a new file of version $version") },
		@{ Name = $prefix + "data/update_test_changed.txt"; Bytes = (Text-Bytes "new content $version") }
	)
	$entries = @($entries | Where-Object { $without -notcontains $_.Name.Substring($prefix.Length) })
	return @($entries) + @($extra)
}

# ---------------------------------------------------------------------------------------------------------------------------------
# The test server
# ---------------------------------------------------------------------------------------------------------------------------------
$script:serverRoot = Join-Path $work "server"
$script:serverLog = Join-Path $work "server_requests.log"
$script:serverPort = 0
$script:serverProcess = $null

function Get-FreePort
{
	$listener = New-Object Net.Sockets.TcpListener([Net.IPAddress]::Loopback, 0)
	$listener.Start()
	$port = ([Net.IPEndPoint]$listener.LocalEndpoint).Port
	$listener.Stop()
	return $port
}

function Start-TestServer
{
	New-Item -ItemType Directory -Path (Join-Path $script:serverRoot "files") -Force | Out-Null
	$script:serverPort = Get-FreePort
	Remove-Item -LiteralPath $script:serverLog -Force -ErrorAction SilentlyContinue
	$script:serverProcess = Start-Process -FilePath "powershell.exe" -ArgumentList @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", (Q $serverScript), "-Port", $script:serverPort, "-Root", (Q $script:serverRoot), "-LogFile", (Q $script:serverLog), "-RunSeconds", 3000) -WindowStyle Hidden -PassThru
	[void]$startedProcesses.Add($script:serverProcess)
	$up = Wait-Until { (Read-SharedText $script:serverLog) -match "listening" } 30
	if (-not $up) { throw "the loopback test server did not start" }
}

function Set-ServerBehaviour([hashtable]$behaviour)
{
	$config = @{ apiStatus = 200; rateLimit = $false; partialAfter = 0; slowKBps = 0; redirect = $true }
	foreach ($key in $behaviour.Keys) { $config[$key] = $behaviour[$key] }
	Set-Content -LiteralPath (Join-Path $script:serverRoot "server.json") -Value ($config | ConvertTo-Json) -Encoding ascii
}

function Clear-ServerLog { Remove-Item -LiteralPath $script:serverLog -Force -ErrorAction SilentlyContinue; Add-Content -LiteralPath $script:serverLog -Value "log cleared" }

# Publishes a "release": the zip (given as entries or as a file), the manifest and the API answer. $claimedHash lets a test lie about the zip.
function Publish-Release([string]$tag, $zipEntries, [string]$zipFile = "", [string]$claimedHash = "", [bool]$withManifest = $true, [bool]$withDigest = $true, [string]$manifestHash = "", [string]$notes = "Test release notes: new things.")
{
	$files = Join-Path $script:serverRoot "files"
	Get-ChildItem -LiteralPath $files -Force -ErrorAction SilentlyContinue | Remove-Item -Force -Recurse
	$zipPath = Join-Path $files "Sonic3AIR_Online.zip"
	if ($zipFile -ne "") { Copy-Item -LiteralPath $zipFile -Destination $zipPath } else { New-ZipFile $zipPath $zipEntries }
	$realHash = Get-Sha256 $zipPath
	$size = (Get-Item -LiteralPath $zipPath).Length
	$digestHash = if ($claimedHash -ne "") { $claimedHash } else { $realHash }
	$manifestSha = if ($manifestHash -ne "") { $manifestHash } else { $digestHash }
	$version = $tag.TrimStart('v')
	$base = "http://127.0.0.1:" + $script:serverPort + "/dl/"
	$assets = @(@{ name = "Sonic3AIR_Online.zip"; size = $size; browser_download_url = ($base + "Sonic3AIR_Online.zip") })
	if ($withDigest) { $assets[0]["digest"] = "sha256:" + $digestHash }
	if ($withManifest)
	{
		$manifestText = (@{ version = $version; asset = "Sonic3AIR_Online.zip"; size = $size; sha256 = $manifestSha } | ConvertTo-Json)
		Set-Content -LiteralPath (Join-Path $files "Sonic3AIR_Online.manifest.json") -Value $manifestText -Encoding ascii
		$assets += @{ name = "Sonic3AIR_Online.manifest.json"; size = $manifestText.Length; browser_download_url = ($base + "Sonic3AIR_Online.manifest.json") }
	}
	$api = @{ tag_name = $tag; name = $tag; draft = $false; prerelease = $false; body = $notes; assets = $assets }
	Set-Content -LiteralPath (Join-Path $script:serverRoot "api_latest.json") -Value ($api | ConvertTo-Json -Depth 6) -Encoding ascii
	return @{ Hash = $realHash; Size = $size }
}

# ---------------------------------------------------------------------------------------------------------------------------------
# Running the real game
# ---------------------------------------------------------------------------------------------------------------------------------
$script:updateLog = Join-Path $updateDir "update_log.txt"

function Reset-UpdateState
{
	foreach ($name in @("update_log.txt", "last_result.txt", "helper_log.txt", "helper.ps1", "staging_manifest.txt")) { Remove-Item -LiteralPath (Join-Path $updateDir $name) -Force -ErrorAction SilentlyContinue }
	foreach ($name in @("download", "staging", "backup")) { $path = Join-Path $updateDir $name; if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Recurse -Force } }
}

function Invoke-Game([string]$installDir, [string]$mode, [string]$apiBase, [string]$waitPattern, [int]$timeoutSeconds, [switch]$KeepRunning, [string]$repo = 'test/repo', [string]$exeName = 'Sonic3AIR.exe')
{
	Reset-UpdateState
	# An empty $apiBase / $repo leaves the switch unset: the game then uses the real api.github.com and the repository from its settings
	$variables = @{ 'S3AIR_UPDATE_AUTO' = $mode; 'S3AIR_LOGNAME' = '_updcheck' }
	if ($apiBase -ne '') { $variables['S3AIR_UPDATE_API'] = $apiBase }
	if ($repo -ne '') { $variables['S3AIR_UPDATE_REPO'] = $repo }
	foreach ($key in $variables.Keys) { [Environment]::SetEnvironmentVariable($key, [string]$variables[$key], 'Process') }
	try { $process = Start-Process -FilePath (Join-Path $installDir $exeName) -WorkingDirectory $installDir -PassThru }
	finally { foreach ($key in $variables.Keys) { [Environment]::SetEnvironmentVariable($key, $null, 'Process') } }
	[void]$startedProcesses.Add($process)
	$null = Wait-Until { ((Read-SharedText $script:updateLog) -match $waitPattern) -or $process.HasExited } $timeoutSeconds
	Start-Sleep -Seconds 3		# the failure is logged first, the cleaning up follows
	$text = Read-SharedText $script:updateLog
	if (-not $KeepRunning) { Stop-TestGames; [void]$startedProcesses.Remove($process) }
	return $text
}

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class UpdateCheckWindowCapture
{
	[StructLayout(LayoutKind.Sequential)]
	public struct RECT { public int Left; public int Top; public int Right; public int Bottom; }
	[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
	[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
}
"@

function Save-WindowPicture([System.Diagnostics.Process]$process, [string]$path)
{
	$process.Refresh()
	$handle = $process.MainWindowHandle
	if ($handle -eq [IntPtr]::Zero) { return $false }
	[void][UpdateCheckWindowCapture]::SetForegroundWindow($handle)
	Start-Sleep -Milliseconds 500
	$rect = New-Object UpdateCheckWindowCapture+RECT
	[void][UpdateCheckWindowCapture]::GetWindowRect($handle, [ref]$rect)
	$width = $rect.Right - $rect.Left; $height = $rect.Bottom - $rect.Top
	if ($width -le 0 -or $height -le 0) { return $false }
	$bitmap = New-Object System.Drawing.Bitmap $width, $height
	$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
	$graphics.CopyFromScreen($rect.Left, $rect.Top, 0, 0, $bitmap.Size)
	$bitmap.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
	$graphics.Dispose(); $bitmap.Dispose()
	return $true
}

# Starts the game with the options menu open on the update entry and the updater in "check" mode; takes a picture once the log line appears
function Invoke-MenuPicture([string]$installDir, [string]$apiBase, [string]$waitPattern, [string]$pictureName)
{
	Reset-UpdateState
	$variables = @{ 'S3AIR_UPDATE_API' = $apiBase; 'S3AIR_UPDATE_REPO' = 'test/repo'; 'S3AIR_UPDATE_AUTO' = 'check'; 'S3AIR_OPTIONS_OPEN' = 'update'; 'S3AIR_LOGNAME' = '_updcheck' }
	foreach ($key in $variables.Keys) { [Environment]::SetEnvironmentVariable($key, [string]$variables[$key], 'Process') }
	try { $process = Start-Process -FilePath (Join-Path $installDir "Sonic3AIR.exe") -WorkingDirectory $installDir -PassThru }
	finally { foreach ($key in $variables.Keys) { [Environment]::SetEnvironmentVariable($key, $null, 'Process') } }
	[void]$startedProcesses.Add($process)
	$null = Wait-Until { ((Read-SharedText $script:updateLog) -match $waitPattern) -or $process.HasExited } 90
	Start-Sleep -Seconds 12		# the menu has to be on screen
	$picturePath = Join-Path $lastLogs $pictureName
	$taken = $false
	$alive = -not $process.HasExited
	if ($alive) { $taken = Save-WindowPicture $process $picturePath }
	$stillAlive = -not $process.HasExited
	Stop-TestGames; [void]$startedProcesses.Remove($process)
	return @{ Taken = $taken; Alive = ($alive -and $stillAlive); Path = $picturePath }
}

function Get-ServerRequests { return (Read-SharedText $script:serverLog) }
function Test-NoLeftovers
{
	$download = Join-Path $updateDir "download"
	$staging = Join-Path $updateDir "staging"
	$downloadLeft = (Test-Path -LiteralPath $download) -and @(Get-ChildItem -LiteralPath $download -Recurse -File -Force).Count -gt 0
	$stagingLeft = (Test-Path -LiteralPath $staging) -and @(Get-ChildItem -LiteralPath $staging -Recurse -File -Force).Count -gt 0
	return (-not $downloadLeft) -and (-not $stagingLeft)
}

$volatilePattern = @('^saves/', '(?i)\.log$', '(?i)^logfile', 'update_write_probe')

# ---------------------------------------------------------------------------------------------------------------------------------
# H: the install helper alone
# ---------------------------------------------------------------------------------------------------------------------------------
function New-HelperFixture([string]$name, [bool]$withMods = $true)
{
	$base = Join-Path $work $name
	Remove-CheckedTree $base
	$fixture = @{ Base = $base; Game = (Join-Path $base "game"); Update = (Join-Path $base "appdata\update"); Mods = (Join-Path $base "appdata\mods"); Staging = (Join-Path $base "appdata\update\staging") }
	foreach ($dir in @($fixture.Game, $fixture.Update, $fixture.Mods, (Join-Path $fixture.Game "data"), (Join-Path $fixture.Game "saves"), (Join-Path $fixture.Game "mods\MyMod"), (Join-Path $fixture.Staging "data"))) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
	# The installed (old) game
	Set-Content -LiteralPath (Join-Path $fixture.Game "Sonic3AIR.exe") -Value "old program" -Encoding ascii
	Set-Content -LiteralPath (Join-Path $fixture.Game "version.txt") -Value "1.0.0" -Encoding ascii
	Set-Content -LiteralPath (Join-Path $fixture.Game "config.json") -Value "// the player's own config" -Encoding ascii
	Set-Content -LiteralPath (Join-Path $fixture.Game "data\changed.txt") -Value "old" -Encoding ascii
	Set-Content -LiteralPath (Join-Path $fixture.Game "data\stale.txt") -Value "stale, was installed by the old version" -Encoding ascii
	Set-Content -LiteralPath (Join-Path $fixture.Game "data\locked.txt") -Value "old locked" -Encoding ascii
	Set-Content -LiteralPath (Join-Path $fixture.Game "data\player_made.txt") -Value "made by the player" -Encoding ascii
	Set-Content -LiteralPath (Join-Path $fixture.Game "saves\save1.sav") -Value "save" -Encoding ascii
	Set-Content -LiteralPath (Join-Path $fixture.Game "mods\MyMod\mod.json") -Value "my mod" -Encoding ascii
	Set-Content -LiteralPath (Join-Path $fixture.Game "installed_files.txt") -Value @("Sonic3AIR.exe", "version.txt", "config.json", "data/changed.txt", "data/stale.txt", "data/locked.txt") -Encoding ascii
	# Settings and mods in the data folder
	Set-Content -LiteralPath (Join-Path $fixture.Base "appdata\settings.json") -Value '{"settings":"mine"}' -Encoding ascii
	if ($withMods)
	{
		New-Item -ItemType Directory -Path (Join-Path $fixture.Mods "DropInCoop\scripts") -Force | Out-Null
		New-Item -ItemType Directory -Path (Join-Path $fixture.Mods "UserMod") -Force | Out-Null
		Set-Content -LiteralPath (Join-Path $fixture.Mods "DropInCoop\mod.json") -Value "old coop mod" -Encoding ascii
		Set-Content -LiteralPath (Join-Path $fixture.Mods "DropInCoop\scripts\old_only.lemon") -Value "old only" -Encoding ascii
		Set-Content -LiteralPath (Join-Path $fixture.Mods "UserMod\mod.json") -Value "user mod" -Encoding ascii
	}
	# The staged (new) version
	$files = @{
		"Sonic3AIR.exe" = "new program"; "version.txt" = "1.1.0"; "config.json" = "// new package config"; "data/changed.txt" = "new"; "data/added.txt" = "added"; "data/locked.txt" = "new locked"
		"bundled_mods/DropInCoop/mod.json" = "new coop mod"; "bundled_mods/DropInCoop/scripts/new.lemon" = "new script"
	}
	$fixture.StagedFiles = $files
	Write-HelperStaging $fixture $files
	return $fixture
}

function Write-HelperStaging($fixture, $files)
{
	$lines = New-Object System.Collections.ArrayList
	foreach ($relative in ($files.Keys | Sort-Object))
	{
		$full = Join-Path $fixture.Staging ($relative.Replace('/', '\'))
		New-Item -ItemType Directory -Path (Split-Path -Parent $full) -Force | Out-Null
		[IO.File]::WriteAllBytes($full, (Text-Bytes $files[$relative]))
		[void]$lines.Add((Get-BytesSha256 (Text-Bytes $files[$relative])) + "  " + (Text-Bytes $files[$relative]).Length + "  " + $relative)
	}
	Set-Content -LiteralPath (Join-Path $fixture.Update "staging_manifest.txt") -Value $lines -Encoding ascii
}

function Invoke-Helper($fixture, [hashtable]$extra = @{})
{
	$arguments = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", (Q $helperSource), "-GameDir", (Q $fixture.Game), "-StagingDir", (Q $fixture.Staging), "-UpdateDir", (Q $fixture.Update), "-ModsDir", (Q $fixture.Mods), "-ExeName", "Sonic3AIR.exe", "-FromVersion", "1.0.0", "-ToVersion", "1.1.0", "-NoRestart", "-LockWaitSeconds", 3, "-WaitSeconds", 20)
	foreach ($key in $extra.Keys) { $arguments += "-$key"; if ($null -ne $extra[$key] -and $extra[$key] -ne "") { $arguments += [string]$extra[$key] } }
	$process = Start-Process -FilePath "powershell.exe" -ArgumentList $arguments -WindowStyle Hidden -PassThru -Wait
	return $process.ExitCode
}

function Get-FixtureSignature($fixture) { return (Get-TreeSignature $fixture.Base @('^appdata/update/')) }
function Get-Result($fixture) { return (Read-SharedText (Join-Path $fixture.Update "last_result.txt")).Trim() }
function Get-Text([string]$path) { return (Read-SharedText $path).Trim() }

function Invoke-HelperChecks
{
	Write-Step "H. The install helper (update_helper.ps1) alone, on throw-away folders"

	# H1: happy path
	$f = New-HelperFixture "h1"
	$userBefore = @{ Save = (Get-Sha256 (Join-Path $f.Game "saves\save1.sav")); Mod = (Get-Sha256 (Join-Path $f.Game "mods\MyMod\mod.json")); Player = (Get-Sha256 (Join-Path $f.Game "data\player_made.txt")); Config = (Get-Sha256 (Join-Path $f.Game "config.json")); Settings = (Get-Sha256 (Join-Path $f.Base "appdata\settings.json")); UserMod = (Get-Sha256 (Join-Path $f.Mods "UserMod\mod.json")) }
	$exitCode = Invoke-Helper $f
	$g = $f.Game
	Add-Result "H1a" "Helper installs the update: new and changed files in, program replaced, version.txt new" (($exitCode -eq 0) -and (Get-Text (Join-Path $g "Sonic3AIR.exe")) -eq "new program" -and (Get-Text (Join-Path $g "version.txt")) -eq "1.1.0" -and (Get-Text (Join-Path $g "data\added.txt")) -eq "added" -and (Get-Text (Join-Path $g "data\changed.txt")) -eq "new") ("exit " + $exitCode + ", result: " + (Get-Result $f))
	Add-Result "H1b" "Helper deletes a file that the old version installed and the new one dropped, and nothing else" (-not (Test-Path -LiteralPath (Join-Path $g "data\stale.txt"))) ""
	Add-Result "H1c" "Saves, settings.json, the player's mods (game folder and data folder) and the player's own files are untouched" ((Get-Sha256 (Join-Path $g "saves\save1.sav")) -eq $userBefore.Save -and (Get-Sha256 (Join-Path $g "mods\MyMod\mod.json")) -eq $userBefore.Mod -and (Get-Sha256 (Join-Path $g "data\player_made.txt")) -eq $userBefore.Player -and (Get-Sha256 (Join-Path $f.Base "appdata\settings.json")) -eq $userBefore.Settings -and (Get-Sha256 (Join-Path $f.Mods "UserMod\mod.json")) -eq $userBefore.UserMod) ""
	Add-Result "H1d" "config.json of the player is kept (the package's copy is not installed over it)" ((Get-Sha256 (Join-Path $g "config.json")) -eq $userBefore.Config) ""
	$installedList = @(Get-Content -LiteralPath (Join-Path $g "installed_files.txt"))
	Add-Result "H1e" "A backup of every replaced or deleted file is kept, and the list of installed files is renewed" ((Get-Text (Join-Path $f.Update "backup\from_1.0.0_to_1.1.0\game\Sonic3AIR.exe")) -eq "old program" -and (Get-Text (Join-Path $f.Update "backup\from_1.0.0_to_1.1.0\game\data\stale.txt")) -like "stale*" -and ($installedList -contains "data/added.txt") -and ($installedList -notcontains "data/stale.txt")) ""
	Add-Result "H1f" "The co-op mod is installed into the mods folder (replacing only DropInCoop), the staging folder is removed, the result is written" ((Get-Text (Join-Path $f.Mods "DropInCoop\mod.json")) -eq "new coop mod" -and (Test-Path -LiteralPath (Join-Path $f.Mods "DropInCoop\scripts\new.lemon")) -and -not (Test-Path -LiteralPath (Join-Path $f.Mods "DropInCoop\scripts\old_only.lemon")) -and -not (Test-Path -LiteralPath $f.Staging) -and (Get-Result $f) -eq "Updated from v1.0.0 to v1.1.0.") ""

	# H2: rollback, a file that cannot be replaced (really locked, not simulated)
	$f = New-HelperFixture "h2"
	$before = Get-FixtureSignature $f
	$lock = [IO.File]::Open((Join-Path $f.Game "data\locked.txt"), [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)		# readable (so the backup works) but not writable, like a file another program has open
	try { $exitCode = Invoke-Helper $f } finally { $lock.Dispose() }
	$after = Get-FixtureSignature $f
	Add-Result "H2a" "A file that is locked halfway through the install makes the helper fail, and it says it rolled back" (($exitCode -eq 1) -and ((Get-Result $f) -match "rolled back")) ("exit " + $exitCode + ", result: " + (Get-Result $f))
	Add-Result "H2b" "After that rollback the game folder, the mods folder and the settings are byte-for-byte as before (the staging folder is still there)" ($before -eq $after) $(if ($before -ne $after) { "differs" } else { "" })

	# H2c: a file that cannot even be read for the backup: nothing has been changed yet
	$f = New-HelperFixture "h2c"
	$before = Get-FixtureSignature $f
	$lock = [IO.File]::Open((Join-Path $f.Game "data\locked.txt"), [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::None)
	try { $exitCode = Invoke-Helper $f } finally { $lock.Dispose() }
	Add-Result "H2c" "A file that cannot be read for the backup stops the helper before any change (exit 4, old game restarted)" (($exitCode -eq 4) -and ($before -eq (Get-FixtureSignature $f)) -and ((Get-Result $f) -match "nothing was changed")) ("exit " + $exitCode)

	# H3: rollback, failure after the game files (everything copied, program replaced)
	$f = New-HelperFixture "h3"
	$before = Get-FixtureSignature $f
	$exitCode = Invoke-Helper $f @{ TestFailAt = "afterGameFiles" }
	Add-Result "H3" "A failure after all game files were replaced is rolled back completely (program, files, list of installed files)" (($exitCode -eq 1) -and ($before -eq (Get-FixtureSignature $f))) ("exit " + $exitCode)

	# H4: rollback including the co-op mod
	$f = New-HelperFixture "h4"
	$before = Get-FixtureSignature $f
	$exitCode = Invoke-Helper $f @{ TestFailAt = "afterMods" }
	Add-Result "H4" "A failure after the co-op mod was replaced is rolled back, the old DropInCoop folder is back" (($exitCode -eq 1) -and ($before -eq (Get-FixtureSignature $f)) -and (Get-Text (Join-Path $f.Mods "DropInCoop\mod.json")) -eq "old coop mod") ("exit " + $exitCode)

	# H5: program file locked (another copy of the game still running)
	$f = New-HelperFixture "h5"
	$before = Get-FixtureSignature $f
	$lock = [IO.File]::Open((Join-Path $f.Game "Sonic3AIR.exe"), [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
	try { $exitCode = Invoke-Helper $f } finally { $lock.Dispose() }
	Add-Result "H5" "A locked program file: the helper waits, gives up with exit 3 and changes nothing" (($exitCode -eq 3) -and ($before -eq (Get-FixtureSignature $f)) -and ((Get-Result $f) -match "still in use")) ("exit " + $exitCode + ", result: " + (Get-Result $f))

	# H6: a staged file was changed after the game wrote the list
	$f = New-HelperFixture "h6"
	$before = Get-FixtureSignature $f
	Set-Content -LiteralPath (Join-Path $f.Staging "data\changed.txt") -Value "tampered" -Encoding ascii
	$exitCode = Invoke-Helper $f
	Add-Result "H6" "A staged file that no longer matches its SHA-256 stops the helper before anything is changed" (($exitCode -eq 4) -and ((Get-Result $f) -match "SHA-256|size") -and ($before -eq (Get-FixtureSignature $f))) ("exit " + $exitCode + ", result: " + (Get-Result $f))

	# H7: an unsafe path in the list
	$f = New-HelperFixture "h7"
	$before = Get-FixtureSignature $f
	$evilHash = Get-BytesSha256 (Text-Bytes "evil")
	Add-Content -LiteralPath (Join-Path $f.Update "staging_manifest.txt") -Value ($evilHash + "  4  ../outside.txt") -Encoding ascii
	$exitCode = Invoke-Helper $f
	Add-Result "H7" "A path that climbs out of the folder in the staging list is refused, nothing is changed or written outside" (($exitCode -eq 4) -and -not (Test-Path -LiteralPath (Join-Path $f.Base "game\..\outside.txt")) -and ($before -eq (Get-FixtureSignature $f))) ("exit " + $exitCode)

	# H8: protected files in the staging folder are never installed
	$f = New-HelperFixture "h8"
	$files = $f.StagedFiles.Clone()
	$files["saves/evil.sav"] = "evil save"; $files["settings.json"] = "evil settings"; $files["mods/EvilMod/mod.json"] = "evil mod"; $files["rom.gen"] = "evil rom"
	Write-HelperStaging $f $files
	$exitCode = Invoke-Helper $f
	Add-Result "H8" "Saves, settings, user mods and ROM files that somehow got into the staging folder are skipped (the rest installs)" (($exitCode -eq 0) -and -not (Test-Path -LiteralPath (Join-Path $f.Game "saves\evil.sav")) -and -not (Test-Path -LiteralPath (Join-Path $f.Game "settings.json")) -and -not (Test-Path -LiteralPath (Join-Path $f.Game "mods\EvilMod")) -and -not (Test-Path -LiteralPath (Join-Path $f.Game "rom.gen")) -and (Get-Text (Join-Path $f.Game "version.txt")) -eq "1.1.0") ("exit " + $exitCode)

	# H9: the helper waits for the game process to close
	$f = New-HelperFixture "h9"
	$sleeper = Start-Process -FilePath "powershell.exe" -ArgumentList @("-NoProfile", "-Command", "Start-Sleep -Seconds 4") -WindowStyle Hidden -PassThru
	$clock = [Diagnostics.Stopwatch]::StartNew()
	$exitCode = Invoke-Helper $f @{ ProcessId = $sleeper.Id }
	$waited = $clock.Elapsed.TotalSeconds
	Add-Result "H9" "The helper waits until the game process has exited before it touches any file" (($exitCode -eq 0) -and $waited -ge 3.5 -and (Get-Text (Join-Path $f.Game "version.txt")) -eq "1.1.0") ("waited " + [math]::Round($waited, 1) + " s")

	# H10: the first update from a version that has no list of installed files deletes nothing
	$f = New-HelperFixture "h10"
	Remove-Item -LiteralPath (Join-Path $f.Game "installed_files.txt") -Force
	$exitCode = Invoke-Helper $f
	Add-Result "H10" "Without a list of installed files from the old version nothing is deleted" (($exitCode -eq 0) -and (Test-Path -LiteralPath (Join-Path $f.Game "data\stale.txt")) -and (Test-Path -LiteralPath (Join-Path $f.Game "installed_files.txt"))) ("exit " + $exitCode)

	Add-Result "H11" "The helper in the package folder is the same file as the source" (-not (Test-Path -LiteralPath (Join-Path $packageDir "update_helper.ps1")) -or ((Get-Sha256 (Join-Path $packageDir "update_helper.ps1")) -eq (Get-Sha256 $helperSource))) ""
}

# ---------------------------------------------------------------------------------------------------------------------------------
# T: the real game against the loopback server
# ---------------------------------------------------------------------------------------------------------------------------------
function Invoke-GameChecks
{
	Write-Step "T. The real game against a loopback test server that plays GitHub"
	$install = Join-Path $work "install"
	New-OldInstall $install
	$apiBase = "http://127.0.0.1:" + $script:serverPort
	$rom = Join-Path $appdata "Sonic_Knuckles_wSonic3.bin"
	$romBefore = if (Test-Path -LiteralPath $rom) { Get-Sha256 $rom } else { "" }
	$appdataSavesBefore = if (Test-Path -LiteralPath (Join-Path $appdata "saves")) { (Get-ChildItem -LiteralPath (Join-Path $appdata "saves") -Recurse -File | ForEach-Object { $_.FullName.Substring($appdata.Length) } | Sort-Object) -join "|" } else { "" }
	$beforeInstall = Get-TreeSignature $install $volatilePattern
	$preciousBefore = Get-Sha256 (Join-Path $install "saves\precious.sav")

	$happyOnly = ($OnlyCase -eq "T1")
	if (-not $happyOnly) {
	# T4 / T5: same version, older version
	Set-ServerBehaviour @{}
	foreach ($case in @(@("T4", "v1.0.0", "up to date", "The same version is reported as up to date and nothing is downloaded"), @("T5", "v0.9.0", "no downgrade", "An older release is not offered: no downgrade, nothing is downloaded")))
	{
		[void](Publish-Release $case[1] (New-GoodPackageEntries $case[1].TrimStart('v')))
		Clear-ServerLog
		$log = Invoke-Game $install "check" $apiBase $case[2] 90
		$requests = Get-ServerRequests
		Add-Result $case[0] $case[3] (($log -match $case[2]) -and ($requests -match "releases/latest") -and -not ($requests -match "/dl/|/cdn/") -and ($beforeInstall -eq (Get-TreeSignature $install $volatilePattern))) (($log -split "`n" | Select-Object -Last 2) -join " | ")
	}

	# T6: no network (nothing listens on that port)
	$deadPort = Get-FreePort
	$log = Invoke-Game $install "check" ("http://127.0.0.1:" + $deadPort) "FAILED:" 90
	Add-Result "T6" "No network: the check fails with a plain message, the game folder is untouched" (($log -match "FAILED: No connection") -and ($beforeInstall -eq (Get-TreeSignature $install $volatilePattern))) (($log -split "`n" | Select-Object -Last 2) -join " | ")

	# T7: rate limit, 404, garbage
	Publish-Release "v1.1.0" (New-GoodPackageEntries "1.1.0") | Out-Null
	Set-ServerBehaviour @{ apiStatus = 403; rateLimit = $true }
	$log = Invoke-Game $install "check" $apiBase "FAILED:" 90
	Add-Result "T7a" "GitHub's rate limit (HTTP 403 with no requests left) is reported in plain words" ($log -match "FAILED: GitHub says too many requests") (($log -split "`n" | Select-Object -Last 2) -join " | ")
	Set-ServerBehaviour @{ apiStatus = 404 }
	$log = Invoke-Game $install "check" $apiBase "FAILED:" 90
	Add-Result "T7b" "No release found (404, also what a private repository answers) is explained, including the private case" (($log -match "FAILED: No release was found") -and ($log -match "private")) (($log -split "`n" | Select-Object -Last 2) -join " | ")
	Set-ServerBehaviour @{ apiStatus = 502 }
	$log = Invoke-Game $install "check" $apiBase "FAILED:" 90
	Add-Result "T7c" "A server error (HTTP 502) is reported as GitHub having problems" ($log -match "FAILED: GitHub is having problems") (($log -split "`n" | Select-Object -Last 2) -join " | ")
	Set-ServerBehaviour @{}
	Set-Content -LiteralPath (Join-Path $script:serverRoot "api_latest.json") -Value "this is not json at all" -Encoding ascii
	$log = Invoke-Game $install "check" $apiBase "FAILED:" 90
	Add-Result "T7d" "An answer that is not JSON is refused without crashing the game" (($log -match "FAILED:") -and ($beforeInstall -eq (Get-TreeSignature $install $volatilePattern))) (($log -split "`n" | Select-Object -Last 2) -join " | ")

	# T2: hash problems
	$good = New-GoodPackageEntries "1.1.0"
	Publish-Release "v1.1.0" $good -claimedHash (Get-BytesSha256 (Text-Bytes "some other bytes")) | Out-Null
	Clear-ServerLog
	$log = Invoke-Game $install "apply" $apiBase "FAILED:" 120
	Add-Result "T2a" "A download whose SHA-256 differs from the manifest is thrown away and never unpacked or installed" (($log -match "does not match the SHA-256") -and (Test-NoLeftovers) -and -not (Test-Path -LiteralPath (Join-Path $updateDir "helper.ps1")) -and ($beforeInstall -eq (Get-TreeSignature $install $volatilePattern))) (($log -split "`n" | Select-Object -Last 2) -join " | ")
	Publish-Release "v1.1.0" $good -manifestHash (Get-BytesSha256 (Text-Bytes "manifest lies")) | Out-Null
	Clear-ServerLog
	$log = Invoke-Game $install "apply" $apiBase "FAILED:" 120
	Add-Result "T2b" "A manifest and a GitHub digest that disagree stop the update before the big download starts" (($log -match "disagree") -and -not ((Get-ServerRequests) -match "/cdn/Sonic3AIR_Online.zip")) (($log -split "`n" | Select-Object -Last 2) -join " | ")
	Publish-Release "v1.1.0" $good -withManifest $false -withDigest $false | Out-Null
	Clear-ServerLog
	$log = Invoke-Game $install "apply" $apiBase "FAILED:" 120
	Add-Result "T2c" "A release without manifest and without digest cannot be verified and is not installed" (($log -match "cannot be verified") -and -not ((Get-ServerRequests) -match "/cdn/Sonic3AIR_Online.zip")) (($log -split "`n" | Select-Object -Last 2) -join " | ")

	# T3: zip-slip and other content the update must refuse (every zip has the right hash, so only the content rules stop it)
	$evilCases = @(
		@("T3a", "path with .. climbing out of the folder", "evil_dotdot.txt", @(@{ Name = "Sonic3AIR_Online/../../evil_dotdot.txt"; Bytes = (Text-Bytes "evil") }), "unsafe file name"),
		@("T3b", "absolute path", "evil_abs.txt", @(@{ Name = "/evil_abs.txt"; Bytes = (Text-Bytes "evil") }), "unsafe file name"),
		@("T3c", "drive letter", "evil_drive.txt", @(@{ Name = "C:/evil_drive.txt"; Bytes = (Text-Bytes "evil") }), "unsafe file name"),
		@("T3d", "backslash traversal", "evil_backslash.txt", @(@{ Name = "Sonic3AIR_Online\data\..\..\..\evil_backslash.txt"; Bytes = (Text-Bytes "evil") }), "unsafe file name"),
		@("T3e", "a save game and settings", "evil.sav", @(@{ Name = "Sonic3AIR_Online/saves/evil.sav"; Bytes = (Text-Bytes "evil") }), "refused: it contains"),
		@("T3f", "a user settings file", "settings_evil.json", @(@{ Name = "Sonic3AIR_Online/settings_evil.json"; Bytes = (Text-Bytes "{}") }), "refused: it contains")
	)
	foreach ($case in $evilCases)
	{
		Publish-Release "v1.1.0" (New-GoodPackageEntries "1.1.0" $case[3]) | Out-Null
		$log = Invoke-Game $install "apply" $apiBase "FAILED:" 180
		$escaped = @(
			(Join-Path $work $case[2]), (Join-Path (Split-Path -Parent $work) $case[2]), (Join-Path $install $case[2]), (Join-Path $appdata $case[2]), (Join-Path $updateDir $case[2]), (Join-Path (Join-Path $updateDir "staging") $case[2]),
			(Join-Path $install ("saves\" + $case[2])), (Join-Path "C:\" $case[2]), (Join-Path $lastLogs $case[2])
		) | Where-Object { Test-Path -LiteralPath $_ }
		Add-Result $case[0] ("Update package with " + $case[1] + " is refused, nothing is written outside the staging folder") (($log -match $case[4]) -and ($escaped.Count -eq 0) -and (Test-NoLeftovers) -and ($beforeInstall -eq (Get-TreeSignature $install $volatilePattern))) (($log -split "`n" | Select-Object -Last 2) -join " | ")
	}
	Publish-Release "v1.1.0" (New-GoodPackageEntries "1.1.0" @() @("Sonic3AIR.exe")) | Out-Null
	$log = Invoke-Game $install "apply" $apiBase "FAILED:" 180
	Add-Result "T3g" "A package without Sonic3AIR.exe is not a game package and is refused" (($log -match "not a game package") -and (Test-NoLeftovers)) (($log -split "`n" | Select-Object -Last 2) -join " | ")
	Publish-Release "v1.1.0" (New-GoodPackageEntries "1.0.5") | Out-Null
	$log = Invoke-Game $install "apply" $apiBase "FAILED:" 180
	Add-Result "T3h" "A package whose own version.txt differs from the release is refused" (($log -match "does not match the release") -and (Test-NoLeftovers) -and ($beforeInstall -eq (Get-TreeSignature $install $volatilePattern))) (($log -split "`n" | Select-Object -Last 2) -join " | ")

	# T8: partial download (the server closes the connection halfway)
	$published = Publish-Release "v1.1.0" (New-GoodPackageEntries "1.1.0")
	Set-ServerBehaviour @{ partialAfter = [int]($published.Size / 2) }
	$log = Invoke-Game $install "apply" $apiBase "FAILED:" 120
	Add-Result "T8" "A download that is cut off halfway is detected, the leftovers are deleted, nothing is installed" (($log -match "FAILED: (The download was cut off|The download is incomplete)") -and (Test-NoLeftovers) -and ($beforeInstall -eq (Get-TreeSignature $install $volatilePattern))) (($log -split "`n" | Select-Object -Last 2) -join " | ")

	# T9: the player cancels during the download
	Set-ServerBehaviour @{ slowKBps = 300 }
	$log = Invoke-Game $install "cancel" $apiBase "cancelled during the download" 120
	Add-Result "T9" "Cancelling during the download stops it, deletes the partial file and changes nothing" (($log -match "cancelled during the download") -and (Test-NoLeftovers) -and -not (Test-Path -LiteralPath (Join-Path $updateDir "helper.ps1")) -and ($beforeInstall -eq (Get-TreeSignature $install $volatilePattern))) (($log -split "`n" | Select-Object -Last 2) -join " | ")
	Set-ServerBehaviour @{}

	# T12: an install without version.txt (development build) has updates switched off
	Rename-Item -LiteralPath (Join-Path $install "version.txt") -NewName "version.txt.off"
	Clear-ServerLog
	$log = Invoke-Game $install "apply" $apiBase "switched off" 90
	Rename-Item -LiteralPath (Join-Path $install "version.txt.off") -NewName "version.txt"
	Add-Result "T12" "A development build without version.txt has the updater switched off and asks the server nothing" (($log -match "updates are switched off") -and -not ((Get-ServerRequests) -match "releases/latest")) (($log -split "`n" | Select-Object -Last 2) -join " | ")

	# T12b: a renamed program would fail after the whole download (the package and the install helper use Sonic3AIR.exe), so the updater says so at once and asks the server nothing
	Rename-Item -LiteralPath (Join-Path $install "Sonic3AIR.exe") -NewName "AirNet.exe"
	try
	{
		Clear-ServerLog
		$log = Invoke-Game $install "apply" $apiBase "switched off" 90 -exeName "AirNet.exe"
	}
	finally { Rename-Item -LiteralPath (Join-Path $install "AirNet.exe") -NewName "Sonic3AIR.exe" }
	Add-Result "T12b" "A game program renamed away from Sonic3AIR.exe has the updater switched off with an explanation and asks the server nothing (instead of failing after the download)" (($log -match "switched off: Updates only work when the game program is named Sonic3AIR.exe") -and -not ((Get-ServerRequests) -match "releases/latest")) (($log -split "`n" | Select-Object -Last 2) -join " | ")

	# T13: the Options menu with the update entry, in two states, with pictures (looked at by eye) and proof that the game keeps running
	Publish-Release "v1.1.0" (New-GoodPackageEntries "1.1.0") -notes "Test release notes: new things.`nSecond line of the notes with a longer text that has to wrap around to the next line of the menu.`n- a list entry`n- another entry" | Out-Null
	$picture = Invoke-MenuPicture $install $apiBase "update available" "T13-update-available.png"
	Add-Result "T13a" "The options menu draws the update entry with an available update and release notes without crashing (picture: checks\last_logs\T13-update-available.png)" ($picture.Taken -and $picture.Alive) ""
	Set-ServerBehaviour @{ apiStatus = 404 }
	$picture = Invoke-MenuPicture $install $apiBase "FAILED:" "T13-update-failed.png"
	Add-Result "T13b" "The options menu draws the entry with a long failure message without crashing (picture: checks\last_logs\T13-update-failed.png)" ($picture.Taken -and $picture.Alive) ""
	Set-ServerBehaviour @{}

	# T14: the real HTTPS API of GitHub (read only, anonymous, check only), once for the public repository of the stock game and once for the default repository of this fork
	if (-not $Offline)
	{
		$definite = "FAILED: (No release was found|GitHub says too many requests|The latest release has no)|update available|up to date"
		$log = Invoke-Game $install "check" "" $definite 120 -repo "Eukaryot/sonic3air"
		Add-Result "T14a" "The real api.github.com over HTTPS answers the updater (the stock game's public repository: JSON read, no Sonic3AIR_Online.zip there, so no update)" (($log -match "FAILED: The latest release has no") -or ($log -match "too many requests")) (($log -split "`n" | Select-Object -Last 2) -join " | ")
		$log = Invoke-Game $install "check" "" $definite 120 -repo ""
		Add-Result "T14b" "The default repository of the fork asked the real way (private today: GitHub answers 404 and the game explains it; once public with a release it finds it)" ($log -match $definite) (($log -split "`n" | Select-Object -Last 2) -join " | ")
	}

	Add-Result "T-unchanged" "After all the refused updates the game folder is byte-for-byte what it was, and the player's save is the same" (($beforeInstall -eq (Get-TreeSignature $install $volatilePattern)) -and ((Get-Sha256 (Join-Path $install "saves\precious.sav")) -eq $preciousBefore)) ""

	}
	# T1: the happy path, last because it changes the install
	$oldExeHash = Get-Sha256 (Join-Path $install "Sonic3AIR.exe")
	$configBefore = Get-Sha256 (Join-Path $install "config.json")
	$userFilesBefore = @{ Player = (Get-Sha256 (Join-Path $install "data\my_user_file.txt")); Mod = (Get-Sha256 (Join-Path $install "mods\MyUserMod\mod.json")) }
	$published = Publish-Release "v1.1.0" (New-GoodPackageEntries "1.1.0") -notes "Test release notes: new things."
	Clear-ServerLog
	$log = Invoke-Game $install "apply" $apiBase "install helper started" 180 -KeepRunning
	$installedOk = Wait-Until { (Read-SharedText (Join-Path $updateDir "last_result.txt")) -match "Updated from" } 120
	$restartedOk = Wait-Until { (Read-SharedText $script:updateLog) -match "up to date \(1\.1\.0\)" } 90
	$running = @(Get-Process -Name "Sonic3AIR" -ErrorAction SilentlyContinue | Where-Object { try { $_.Path -like ($install + "\*") } catch { $false } })
	$logAfter = Read-SharedText $script:updateLog
	Stop-TestGames
	$requests = Get-ServerRequests
	Add-Result "T1a" "Happy path: update found, downloaded through the redirect, SHA-256 matched, unpacked, helper started, game closed" (($log -match "update available: 1\.1\.0") -and ($log -match "download verified") -and ($log -match "ready to install") -and ($log -match "install helper started") -and ($requests -match "GET /dl/Sonic3AIR_Online.zip 302") -and ($requests -match "GET /cdn/Sonic3AIR_Online.zip 200")) (($log -split "`n" | Select-Object -Last 3) -join " | ")
	Add-Result "T1b" "Happy path: the helper installed the new version (program file, version.txt, new and changed files, stale file gone)" ($installedOk -and ((Get-Sha256 (Join-Path $install "Sonic3AIR.exe")) -ne $oldExeHash) -and ((Get-Text (Join-Path $install "version.txt")) -eq "1.1.0") -and ((Get-Text (Join-Path $install "data\update_test_new.txt")) -like "a new file*") -and ((Get-Text (Join-Path $install "data\update_test_changed.txt")) -like "new content*") -and -not (Test-Path -LiteralPath (Join-Path $install "data\update_test_removed.txt"))) ("result: " + (Get-Text (Join-Path $updateDir "last_result.txt")))
	Add-Result "T1c" "Happy path: the player's save, config.json, mod and own file in the game folder were not touched; the ROM and the saves of the data folder neither" (((Get-Sha256 (Join-Path $install "saves\precious.sav")) -eq $preciousBefore) -and ((Get-Sha256 (Join-Path $install "config.json")) -eq $configBefore) -and ((Get-Sha256 (Join-Path $install "data\my_user_file.txt")) -eq $userFilesBefore.Player) -and ((Get-Sha256 (Join-Path $install "mods\MyUserMod\mod.json")) -eq $userFilesBefore.Mod) -and ($romBefore -eq "" -or (Get-Sha256 $rom) -eq $romBefore) -and ($appdataSavesBefore -eq $(if (Test-Path -LiteralPath (Join-Path $appdata "saves")) { (Get-ChildItem -LiteralPath (Join-Path $appdata "saves") -Recurse -File | ForEach-Object { $_.FullName.Substring($appdata.Length) } | Sort-Object) -join "|" } else { "" }))) ""
	Add-Result "T1d" "Happy path: a backup of the old program was kept, and the new game started by itself and reports it is up to date" ((Test-Path -LiteralPath (Join-Path $updateDir "backup\from_1.0.0_to_1.1.0\game\Sonic3AIR.exe")) -and ((Get-Sha256 (Join-Path $updateDir "backup\from_1.0.0_to_1.1.0\game\Sonic3AIR.exe")) -eq $oldExeHash) -and $restartedOk -and $running.Count -ge 1) ("restarted: " + $restartedOk + ", processes: " + $running.Count)
	Add-Result "T1e" "Happy path: asking again right after the update says up to date, and nothing is downloaded a second time" ($restartedOk -and (([regex]::Matches($requests, "GET /cdn/Sonic3AIR_Online.zip 200")).Count -eq 1)) ""
	if (-not $KeepWork) { Reset-UpdateState }
}

# ---------------------------------------------------------------------------------------------------------------------------------
# R: the release script and the real release zip
# ---------------------------------------------------------------------------------------------------------------------------------
function Invoke-ReleaseChecks
{
	Write-Step "R. release.ps1 -DryRun and the real release package"
	$release = Join-Path $checksDir "release.ps1"
	function Run-Release([string[]]$arguments)
	{
		$output = & powershell -NoProfile -ExecutionPolicy Bypass -File $release @arguments 2>&1
		return @{ Code = $LASTEXITCODE; Text = ($output | Out-String) }
	}

	$dirtyRun = Run-Release @("-Version", "99.0.0", "-DryRun")
	$dirtyNow = @(& git -C $root status --porcelain).Count -gt 0
	Add-Result "R1a" "release.ps1 refuses a working tree with uncommitted changes (also in a dry run)" ((-not $dirtyNow) -or (($dirtyRun.Code -ne 0) -and ($dirtyRun.Text -match "uncommitted"))) $(if (-not $dirtyNow) { "tree is clean, so this could not be shown" } else { "" })
	$badRun = Run-Release @("-Version", "1.2", "-DryRun", "-AllowDirty")
	Add-Result "R1b" "release.ps1 refuses a version that is not x.y.z" (($badRun.Code -ne 0) -and ($badRun.Text -match "Version must look like")) ""
	$publishDirty = Run-Release @("-Version", "99.0.0", "-AllowDirty")
	Add-Result "R1c" "-AllowDirty cannot be combined with a real publish" (($publishDirty.Code -ne 0) -and ($publishDirty.Text -match "only allowed together with -DryRun")) ""

	$tagsBefore = @(& git -C $root tag --list).Count
	$dry = Run-Release @("-Version", "99.0.0", "-Notes", "Dry run notes", "-DryRun", "-AllowDirty")
	$releaseFolder = Join-Path $root "release\v99.0.0"
	$tagsAfter = @(& git -C $root tag --list).Count
	$zipPath = Join-Path $releaseFolder "Sonic3AIR_Online.zip"
	$manifestPath = Join-Path $releaseFolder "Sonic3AIR_Online.manifest.json"
	$shaPath = Join-Path $releaseFolder "Sonic3AIR_Online.zip.sha256"
	Add-Result "R1d" "release.ps1 -DryRun builds the package and ends with RELEASE DRY RUN OK, without creating a tag" (($dry.Code -eq 0) -and ($dry.Text -match "RELEASE DRY RUN OK") -and ($tagsBefore -eq $tagsAfter)) ("exit " + $dry.Code)
	$manifestOk = $false
	if ((Test-Path -LiteralPath $manifestPath) -and (Test-Path -LiteralPath $zipPath))
	{
		$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
		$manifestOk = ($manifest.version -eq "99.0.0") -and ($manifest.asset -eq "Sonic3AIR_Online.zip") -and ($manifest.size -eq (Get-Item -LiteralPath $zipPath).Length) -and ($manifest.sha256 -eq (Get-Sha256 $zipPath)) -and ((Get-Content -LiteralPath $shaPath -Raw).Trim() -eq ($manifest.sha256 + "  Sonic3AIR_Online.zip"))
	}
	Add-Result "R1e" "The manifest (version, asset, size, sha256) and the .sha256 file match the zip" $manifestOk ""
	$versionInZip = ""
	$zipEntriesBad = @()
	$hasHelper = $false
	if (Test-Path -LiteralPath $zipPath)
	{
		$archive = [IO.Compression.ZipFile]::OpenRead($zipPath)
		try
		{
			$entry = $archive.Entries | Where-Object { $_.FullName.Replace('\', '/') -match '(^|/)version\.txt$' } | Select-Object -First 1
			if ($entry) { $versionInZip = (New-Object IO.StreamReader($entry.Open())).ReadToEnd().Trim() }
			$hasHelper = @($archive.Entries | Where-Object { $_.FullName.Replace('\', '/') -match '(^|/)update_helper\.ps1$' }).Count -eq 1
			$zipEntriesBad = @($archive.Entries | Where-Object { $_.FullName.Replace('\', '/') -match '(?i)(^|/)(saves|states|savedata|mods)(/|$)|(^|/)settings[^/]*\.json$|testhooks\.enabled|\.(gen|smd|md|sav|srm)$|sonic_knuckles' })
		}
		finally { $archive.Dispose() }
	}
	Add-Result "R1f" "The release zip has version.txt = 99.0.0, update_helper.ps1, and no save, settings, ROM or test marker" (($versionInZip -eq "99.0.0") -and $hasHelper -and ($zipEntriesBad.Count -eq 0)) ("version.txt: '" + $versionInZip + "'")

	# R1g: without -Build a dry run says so when Sonic3AIR.exe is older than the source (a real release always builds, so a stale program cannot be shipped under a new tag)
	$staleExe = Get-Item -LiteralPath (Join-Path $root "sonic3air_online_stable_source\Oxygen\sonic3air\bin\Release_x64\Sonic3AIR.exe")
	$staleExeOriginalTime = $staleExe.LastWriteTime
	try
	{
		$staleExe.LastWriteTime = [datetime]"2001-01-01"
		$staleRun = Run-Release @("-Version", "99.0.0", "-Notes", "Dry run notes", "-DryRun", "-AllowDirty")
	}
	finally { $staleExe.LastWriteTime = $staleExeOriginalTime }
	Add-Result "R1g" "release.ps1 -DryRun without -Build notices that Sonic3AIR.exe is older than the source" ($staleRun.Text -match "Sonic3AIR\.exe is older than the source file") ""

	if ($SkipReal -or -not (Test-Path -LiteralPath $zipPath)) { return }

	# R2: the real release zip, installed over an older copy through the real updater
	$install = Join-Path $work "install_real"
	New-OldInstall $install
	$apiBase = "http://127.0.0.1:" + $script:serverPort
	Set-ServerBehaviour @{}
	$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
	[void](Publish-Release "v99.0.0" @() -zipFile $zipPath -notes "The real release package.")
	$modInstalled = Join-Path $appdata "mods\DropInCoop"
	if (Test-Path -LiteralPath $modInstalled) { if (Test-Path -LiteralPath $modsBackup) { Remove-Item -LiteralPath $modsBackup -Recurse -Force }; New-Item -ItemType Directory -Path $modsBackup -Force | Out-Null; Copy-Item -LiteralPath $modInstalled -Destination $modsBackup -Recurse -Force }
	$beforeUser = Get-Sha256 (Join-Path $install "saves\precious.sav")
	$log = Invoke-Game $install "apply" $apiBase "install helper started|FAILED:" 400 -KeepRunning
	$done = Wait-Until { (Read-SharedText (Join-Path $updateDir "last_result.txt")) -match "Updated from|FAILED" } 400
	$restarted = Wait-Until { (Read-SharedText $script:updateLog) -match "up to date \(99\.0\.0\)" } 120
	Stop-TestGames
	$resultText = Get-Text (Join-Path $updateDir "last_result.txt")
	Add-Result "R2a" "The real release zip passes every rule of the updater (names, forbidden content, version) and installs over an older copy" ($done -and ($resultText -match "Updated from v1\.0\.0 to v99\.0\.0")) ($resultText + " | " + (($log -split "`n" | Select-Object -Last 2) -join " | "))
	# Every file of the zip is now in the install, byte for byte
	$equal = $true
	$checked = 0
	$archive = [IO.Compression.ZipFile]::OpenRead($zipPath)
	try
	{
		foreach ($entry in $archive.Entries)
		{
			$relative = ($entry.FullName.Replace('\', '/') -replace '^Sonic3AIR_Online/?', '')
			if ($relative -eq '' -or $relative.EndsWith('/') -or $relative -ieq 'config.json') { continue }
			$target = Join-Path $install ($relative.Replace('/', '\'))
			if (-not (Test-Path -LiteralPath $target)) { $equal = $false; break }
			$stream = $entry.Open()
			try { $sha = [Security.Cryptography.SHA256]::Create(); $entryHash = [BitConverter]::ToString($sha.ComputeHash($stream)).Replace("-", "").ToLowerInvariant() } finally { $stream.Dispose() }
			if ($entryHash -ne (Get-Sha256 $target)) { $equal = $false; break }
			++$checked
		}
	}
	finally { $archive.Dispose() }
	Add-Result "R2b" "Every file of the release zip is in the game folder byte for byte, and the player's save is untouched" ($equal -and $checked -gt 500 -and ((Get-Sha256 (Join-Path $install "saves\precious.sav")) -eq $beforeUser)) ("files compared: " + $checked)
	Add-Result "R2c" "The updated game starts and reports 99.0.0 as its version" ($restarted -and ((Get-Text (Join-Path $install "version.txt")) -eq "99.0.0")) ""
	Restore-DeveloperMod
	Remove-CheckedTree $install
}

# ---------------------------------------------------------------------------------------------------------------------------------
$exitCode = 0
$rebuildPackage = $false
try
{
	if (Get-Process -Name "Sonic3AIR" -ErrorAction SilentlyContinue) { throw "Sonic 3 A.I.R. is running. Close it first: the checks start and stop their own game windows, and only one thing runs at a time." }
	if (-not (Test-Path -LiteralPath (Join-Path $packageDir "Sonic3AIR.exe"))) { throw "No package folder at $packageDir. Run checks\build_online_stable.ps1 and checks\make_package.ps1 first." }
	New-Item -ItemType Directory -Path $lastLogs -Force | Out-Null
	if (Test-Path -LiteralPath $work) { Remove-CheckedTree $work }
	New-Item -ItemType Directory -Path $work -Force | Out-Null
	New-Item -ItemType Directory -Path $updateDir -Force | Out-Null

	if (Wants "H") { Invoke-HelperChecks }
	if ((Wants "T") -or (Wants "R"))
	{
		Start-TestServer
		if (Wants "T") { Invoke-GameChecks }
		if (Wants "R") { $rebuildPackage = $true; Invoke-ReleaseChecks }
	}
}
catch
{
	Add-Result "T0" "The update checks ran to the end" $false $_.Exception.Message
}
finally
{
	Stop-StartedProcesses
	try { Restore-DeveloperMod } catch { Write-Host ("could not put back the developer's co-op mod: " + $_.Exception.Message) }
	if (-not $KeepWork) { try { Reset-UpdateState } catch { } }
	if ((Test-Path -LiteralPath $work) -and -not $KeepWork) { try { Remove-CheckedTree $work } catch { Write-Host ("could not remove the work folder: " + $_.Exception.Message) } }
	# The release dry run builds a package with the test version 99.0.0: put the normal (unversioned) package back, and remove the test release
	if ($rebuildPackage)
	{
		$testRelease = Join-Path $root "release\v99.0.0"
		if (Test-Path -LiteralPath $testRelease) { Remove-Item -LiteralPath $testRelease -Recurse -Force }
		try { & (Join-Path $checksDir "make_package.ps1") | Out-Null; Write-Host "normal package restored (no test version in it)" } catch { Add-Result "R-restore" "The normal package was rebuilt after the release dry run" $false $_.Exception.Message }
	}
}

$lines = @($results | ForEach-Object { ($(if ($_.Passed) { "PASS" } else { "FAIL" })) + " " + $_.Id + "  " + $_.Title + $(if ($_.Detail) { "  -- " + $_.Detail } else { "" }) })
Set-Content -LiteralPath $reportPath -Value $lines -Encoding UTF8
$failed = @($results | Where-Object { -not $_.Passed }).Count
Write-Host ""
Write-Host ("Update checks: " + ($results.Count - $failed) + " passed, " + $failed + " failed") -ForegroundColor $(if ($failed -eq 0) { "Green" } else { "Red" })
if ($failed -ne 0) { exit 1 }
Write-Output "UPDATE CHECKS PASS"
exit 0
