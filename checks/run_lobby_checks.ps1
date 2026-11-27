# Stage P: the public lobby list, quick join and numeric lobby codes, against the real lobby server program (checks\relay_ws, node) on 127.0.0.1.
#
#   checks\run_lobby_checks.ps1             (needs the built game: checks\build_online_stable.ps1; node 18 or newer)
#
# P1  a host opens a PUBLIC lobby, the lobby server lists it, a friend presses QUICK JOIN, they play and stay in sync (no repair, same state hash)
# P2  a PRIVATE lobby is not listed; a friend joins it with its 6 digit code; the listing disappears when the host goes away (expiry)
# P3  a wrong code and a code with a typing mistake: plain message, no hang
# P4  no lobby server set: the menu says so kindly (screenshots), quick join does not hang
# P5  lobby server down: the list and quick join say so within seconds
# P6  a list with an incompatible lobby and a lobby that already plays: greyed out, quick join takes the right one
# P7  screens of the menu (screenshots in checks\last_logs, to be looked at)
# Only one thing runs at a time (the same mutex as run_checks.ps1); every game and the lobby server are stopped at the end; the player's settings.json is put back
# and the hashes of the save files are compared before and after.
param(
	[string[]]$Only = @()
)

$ErrorActionPreference = "Stop"
$checkRunMutex = New-Object System.Threading.Mutex($false, "Local\S3AIRDropInChecks")
if (-not $checkRunMutex.WaitOne(0)) { $checkRunMutex.Dispose(); throw "Another check run owns the game and logs. Wait for it to finish." }

$Only = @($Only | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim().ToUpper() } | Where-Object { $_ })
$checksDir = $PSScriptRoot
$root = Split-Path -Parent $checksDir
$game = Join-Path $root "sonic3air_online_stable_source\Oxygen\sonic3air"
$bin = Join-Path $game "bin\Release_x64"
$exe = Join-Path $bin "Sonic3AIR.exe"
$appdata = Join-Path $env:APPDATA "Sonic3AIR"
$lastLogs = Join-Path $checksDir "last_logs"
$relayDir = Join-Path $checksDir "relay_ws"
$reportPath = Join-Path $checksDir "lobby_checks_report.txt"
$testHooksMarker = Join-Path $bin 'testhooks.enabled'
$settingsPath = Join-Path $appdata "settings.json"
$settingsBackup = Join-Path $lastLogs "lobby_check_settings_backup.json"
$results = New-Object System.Collections.ArrayList
$started = New-Object System.Collections.ArrayList
$relayPort = 29100
$lobbyServer = "127.0.0.1:$relayPort"

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class LobbyWindowCapture
{
	[StructLayout(LayoutKind.Sequential)]
	public struct RECT { public int Left; public int Top; public int Right; public int Bottom; }
	[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
	[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
}
"@

function Wants([string]$id) { return ($Only.Count -eq 0) -or ($Only -contains $id) }
function Write-Step([string]$text) { Write-Host ""; Write-Host $text -ForegroundColor Cyan }
function Add-Result([string]$id, [string]$title, [bool]$passed, [string]$detail)
{
	[void]$results.Add([pscustomobject]@{ Id = $id; Title = $title; Passed = $passed; Detail = $detail })
	$mark = if ($passed) { "PASS" } else { "FAIL" }
	Write-Host ("  [" + $mark + "] " + $id + "  " + $title + $(if ($detail) { "  -- " + $detail } else { "" })) -ForegroundColor $(if ($passed) { "Green" } else { "Red" })
}
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
function Get-NetLog([string]$suffix) { return Read-SharedText (Join-Path $appdata ("logfile" + $suffix + ".txt")) }
function Get-OnlineLines([string]$suffix, [string]$role)
{
	return @((Read-SharedText (Join-Path $appdata ("online_log" + $suffix + ".txt"))) -split "`r?`n" | Where-Object { $_ -match ('^\[' + $role + '\]') })
}
function Get-FileHashHex([string]$path) { if (Test-Path -LiteralPath $path) { return (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash } else { return "(missing)" } }

function Start-GameProcess([hashtable]$variables)
{
	$variables['S3AIR_CHECKS'] = '1'
	$shotDir = Join-Path $lastLogs ("shots\" + $variables['S3AIR_LOGNAME'])
	New-Item -ItemType Directory -Path $shotDir -Force | Out-Null
	Remove-Item -LiteralPath (Join-Path $shotDir "shot.request") -ErrorAction SilentlyContinue
	$variables['S3AIR_SHOT_DIR'] = $shotDir
	foreach ($key in $variables.Keys) { [Environment]::SetEnvironmentVariable($key, [string]$variables[$key], "Process") }
	try { $process = Start-Process -FilePath $exe -WorkingDirectory $game -PassThru }
	finally { foreach ($key in $variables.Keys) { [Environment]::SetEnvironmentVariable($key, $null, "Process") } }
	[void]$started.Add($process)
	$script:shotDirs[$process.Id] = $shotDir
	return $process
}
function Stop-StartedProcesses
{
	foreach ($process in $started) { if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue } }
	$started.Clear()
	Start-Sleep -Milliseconds 800
}
function Remove-GameLogs([string[]]$suffixes)
{
	foreach ($suffix in $suffixes) { Remove-Item -LiteralPath (Join-Path $appdata ("logfile" + $suffix + ".txt")), (Join-Path $appdata ("online_log" + $suffix + ".txt")) -ErrorAction SilentlyContinue }
}
function Save-Logs([string]$tag, [string[]]$suffixes)
{
	foreach ($suffix in $suffixes)
	{
		Copy-Item -LiteralPath (Join-Path $appdata ("logfile" + $suffix + ".txt")) -Destination (Join-Path $lastLogs ("P-" + $tag + "-logfile" + $suffix + ".txt")) -Force -ErrorAction SilentlyContinue
		Copy-Item -LiteralPath (Join-Path $appdata ("online_log" + $suffix + ".txt")) -Destination (Join-Path $lastLogs ("P-" + $tag + "-online_log" + $suffix + ".txt")) -Force -ErrorAction SilentlyContinue
	}
}
function Save-Shot([System.Diagnostics.Process]$process, [string]$name)
{
	# The game saves a picture of its own screen when it finds "shot.request" (see S3AIR_SHOT_DIR in Application.cpp): never a capture of the desktop
	$dir = $script:shotDirs[$process.Id]
	if ($null -eq $dir -or $process.HasExited) { return $false }
	$target = Join-Path $dir ($name + ".bmp")
	Remove-Item -LiteralPath $target -ErrorAction SilentlyContinue
	Set-Content -LiteralPath (Join-Path $dir "shot.request") -Value $name -Encoding ascii -NoNewline
	$ok = Wait-Until { Test-Path -LiteralPath $target } 10
	if (-not $ok) { return $false }
	Start-Sleep -Milliseconds 300
	$bitmap = [System.Drawing.Bitmap]::FromFile($target)
	try
	{
		# the game's picture, scaled up (nearest neighbour) so that the small text can be read
		$scale = [Math]::Max(1, [int][Math]::Floor(1200 / $bitmap.Width))
		$big = New-Object System.Drawing.Bitmap ($bitmap.Width * $scale), ($bitmap.Height * $scale)
		$g = [System.Drawing.Graphics]::FromImage($big)
		$g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
		$g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::Half
		$g.DrawImage($bitmap, 0, 0, ($bitmap.Width * $scale), ($bitmap.Height * $scale))
		$g.Dispose()
		$big.Save((Join-Path $lastLogs ("P-" + $name + ".png")), [System.Drawing.Imaging.ImageFormat]::Png)
		$big.Dispose()
	}
	finally { $bitmap.Dispose() }
	Remove-Item -LiteralPath $target -ErrorAction SilentlyContinue
	return $true
}

function Start-LobbyServer
{
	Stop-LobbyServer
	foreach ($name in @('PORT', 'UDP_PORT', 'BIND', 'STATS_SECONDS')) { }
	$env:PORT = [string]$relayPort; $env:UDP_PORT = [string]$relayPort; $env:BIND = '127.0.0.1'; $env:STATS_SECONDS = '0'; $env:MAX_LOBBIES_PER_IP = '12'; $env:MAX_LISTINGS_PER_IP = '12'
	try
	{
		$script:serverProcess = Start-Process -FilePath 'node' -ArgumentList @('server.mjs') -WorkingDirectory $relayDir -PassThru -WindowStyle Hidden -RedirectStandardOutput (Join-Path $lastLogs 'P-lobby-server-output.txt') -RedirectStandardError (Join-Path $lastLogs 'P-lobby-server-error.txt')
	}
	finally { foreach ($name in @('PORT', 'UDP_PORT', 'BIND', 'STATS_SECONDS', 'MAX_LOBBIES_PER_IP', 'MAX_LISTINGS_PER_IP')) { Remove-Item "Env:$name" -ErrorAction SilentlyContinue } }
	$ok = Wait-Until { try { (Invoke-RestMethod -Uri "http://127.0.0.1:$relayPort/health" -TimeoutSec 2).ok } catch { $false } } 20
	if (-not $ok) { throw "the lobby server (node server.mjs) did not start" }
}
function Stop-LobbyServer
{
	if ($null -ne $script:serverProcess -and -not $script:serverProcess.HasExited) { Stop-Process -Id $script:serverProcess.Id -Force -ErrorAction SilentlyContinue }
	$script:serverProcess = $null
	Start-Sleep -Milliseconds 300
}
$script:serverProcess = $null
$script:shotDirs = @{}
$script:fakeProcesses = New-Object System.Collections.ArrayList

function Invoke-Probe([string[]]$arguments)
{
	$text = & node (Join-Path $relayDir 'probe.mjs') $lobbyServer @arguments 2>&1
	return ($text | Out-String).Trim()
}
function Get-ListedLobbies([string]$compat = '')
{
	$json = Invoke-Probe @('list', $compat)
	try { return ($json | ConvertFrom-Json) } catch { return $null }
}
function Start-FakeLobby([string]$name, [int]$seconds, [string]$compat, [int]$players, [int]$max, [int]$inGame, [string]$hostName)
{
	$process = Start-Process -FilePath 'node' -ArgumentList @('probe.mjs', $lobbyServer, 'fake', ('"' + $name + '"'), [string]$seconds, $compat, [string]$players, [string]$max, [string]$inGame, $hostName) -WorkingDirectory $relayDir -PassThru -WindowStyle Hidden -RedirectStandardOutput (Join-Path $lastLogs ('P-fake-' + ($name -replace '\W', '_') + '.txt'))
	[void]$script:fakeProcesses.Add($process)
	return $process
}
function Stop-FakeLobbies
{
	foreach ($process in $script:fakeProcesses) { if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue } }
	$script:fakeProcesses.Clear()
}
function Get-LobbyCodeFromLog([string]$suffix)
{
	$m = [regex]::Match((Get-NetLog $suffix), 'lobby code (\d{6})')
	if ($m.Success) { return $m.Groups[1].Value } else { return $null }
}

# -------------------------------------------------------------------------------------------------------------------

Write-Host ""
Write-Host "Sonic 3 AirNet: public lobby checks (stage P)" -ForegroundColor White
New-Item -ItemType Directory -Path $lastLogs -Force | Out-Null
$saveFiles = @(Get-ChildItem -LiteralPath (Join-Path $appdata "storage") -File -ErrorAction SilentlyContinue)
$saveBefore = @{}
foreach ($file in $saveFiles) { $saveBefore[$file.Name] = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash + "|" + $file.LastWriteTimeUtc.Ticks }
$inputSettingsBefore = Get-FileHashHex (Join-Path $appdata "settings_input.json")

try
{
	if (Get-Process -Name "Sonic3AIR" -ErrorAction SilentlyContinue) { throw "Sonic 3 A.I.R. is running (maybe the player's own game). It is not touched; close it and run this again." }
	if (-not (Test-Path -LiteralPath $exe)) { throw "The game is not built: $exe" }
	if ($null -eq (Get-Command node -ErrorAction SilentlyContinue)) { throw "node (version 18 or newer) is needed for the lobby server program" }
	# The settings file is changed by the games of the checks (the name, the lobby server...): saved here, put back at the end
	if (Test-Path -LiteralPath $settingsPath) { Copy-Item -LiteralPath $settingsPath -Destination $settingsBackup -Force }
	Set-Content -LiteralPath $testHooksMarker -Value 'Developer test hooks are enabled while a check run is active. Created and removed by checks\run_lobby_checks.ps1.' -Encoding ascii

	Start-LobbyServer
	$health = Invoke-RestMethod -Uri "http://127.0.0.1:$relayPort/health"
	Add-Result 'P0' 'The lobby server program starts and answers on 127.0.0.1' ($health.ok -eq $true) ("listed lobbies: " + $health.listed)

	$common = @{ 'S3AIR_LOBBY_SERVER' = $lobbyServer }

	# ---------------------------------------------------------------------------------------------------------------
	if (Wants 'P1')
	{
		Write-Step "P1. A public lobby is listed, a friend uses QUICK JOIN, they play in sync"
		Stop-StartedProcesses
		Remove-GameLogs @('_phost', '_pfriend')
		$hostProcess = Start-GameProcess ($common + @{ 'S3AIR_LOBBY_OPEN' = 'hostpublic'; 'S3AIR_LOGNAME' = '_phost'; 'S3AIR_PLAYER_NAME' = 'Hosty'; 'S3AIR_LOBBY_NAME' = 'Check Lobby'; 'S3AIR_LOBBY_AUTOSTART' = '10'; 'S3AIR_LOBBY_AUTOGAME' = '0x0100' })
		$code = $null
		[void](Wait-Until { $script:code = Get-LobbyCodeFromLog '_phost'; [bool]$script:code } 120)
		$code = $script:code
		Add-Result 'P1a' 'The host opens a public lobby and the lobby server gives it a 6 digit code' ([bool]$code) ("code: " + $(if ($code) { 'received' } else { 'none' }))
		$list = Get-ListedLobbies
		$entry = if ($null -ne $list -and $list.lobbies.Count -gt 0) { $list.lobbies[0] } else { $null }
		Add-Result 'P1b' 'The list shows the lobby with its name, host name and player count' ($null -ne $entry -and $entry.name -eq 'Check Lobby' -and $entry.host -eq 'Hosty' -and $entry.players -eq 1 -and $entry.max -eq 4 -and $entry.code -eq $code) ($(if ($entry) { "name=" + $entry.name + " host=" + $entry.host + " players=" + $entry.players + "/" + $entry.max } else { 'nothing listed' }))
		[void](Save-Shot $hostProcess 'P1-host-lobby')

		$friendProcess = Start-GameProcess ($common + @{ 'S3AIR_LOBBY_OPEN' = 'quick'; 'S3AIR_LOGNAME' = '_pfriend'; 'S3AIR_PLAYER_NAME' = 'Friendy'; 'S3AIR_LOBBY_AUTOREADY' = '1'; 'S3AIR_NETPLAY_TEST_INPUT' = '0x1'; 'S3AIR_NETPLAY_TEST_TOGGLE' = '1' })
		$picked = Wait-Until { (Get-NetLog '_pfriend') -match 'quick join picks the lobby "Check Lobby"' } 120
		Add-Result 'P1c' 'QUICK JOIN picks the lobby from the list and the lobby server looks it up' ($picked -and ((Get-NetLog '_pfriend') -match 'the lobby server found the lobby, joining it')) ""
		$inLobby = Wait-Until { (Get-NetLog '_phost') -match 'a friend proved the code' } 90
		Add-Result 'P1d' 'The friend gets into the host lobby through the lobby server and proves the code' $inLobby ""
		if ($inLobby)
		{
			Start-Sleep -Seconds 3
			[void](Save-Shot $hostProcess 'P1-host-with-friend')
			[void](Save-Shot $friendProcess 'P1-friend-in-lobby')
			$list2 = Get-ListedLobbies
			Add-Result 'P1e' 'The list shows 2 of 4 players after the friend joined' ($null -ne $list2 -and $list2.lobbies.Count -gt 0 -and $list2.lobbies[0].players -eq 2) ($(if ($list2 -and $list2.lobbies.Count) { "players=" + $list2.lobbies[0].players } else { 'nothing listed' }))
		}
		$reached = Wait-Until { (@(Get-OnlineLines '_pfriend' 'friend' | Where-Object { $_ -match 'state=5 ' })).Count -gt 0 } 120
		if ($reached) { Start-Sleep -Seconds 35 }
		$friendLines = Get-OnlineLines '_pfriend' 'friend'
		$hostLines = Get-OnlineLines '_phost' 'host'
		$playing = @($friendLines | Where-Object { $_ -match 'state=5 ' })
		$maxRepairs = 0
		foreach ($line in $friendLines) { $m = [regex]::Match($line, 'repairs=(\d+)'); if ($m.Success -and [int]$m.Groups[1].Value -gt $maxRepairs) { $maxRepairs = [int]$m.Groups[1].Value } }
		$lastSync = if ($playing.Count -gt 0) { [regex]::Match($playing[-1], 'last sync ok at frame (\d+)').Groups[1].Value } else { '' }
		$lastFrame = if ($playing.Count -gt 0) { [regex]::Match($playing[-1], 'frame=(\d+)').Groups[1].Value } else { '' }
		$viaRelay = ($playing.Count -gt 0) -and ($playing[-1] -match 'relay=1')
		Add-Result 'P1f' 'Both play the level together through the lobby server relay, with no repair needed (hash checks stay in sync)' ($reached -and $playing.Count -ge 5 -and $maxRepairs -eq 0 -and $viaRelay) ("friend lines in game: " + $playing.Count + ", repairs: " + $maxRepairs + ", last frame " + $lastFrame + ", last sync ok at frame " + $lastSync + ", relay: " + $viaRelay)
		$list3 = Get-ListedLobbies
		Add-Result 'P1g' 'While they play, the list shows the lobby as already playing' ($null -ne $list3 -and $list3.lobbies.Count -gt 0 -and ($list3.lobbies[0].flags -band 1) -eq 1) ($(if ($list3 -and $list3.lobbies.Count) { "flags=" + $list3.lobbies[0].flags } else { 'nothing listed' }))
		Save-Logs 'P1' @('_phost', '_pfriend')
		# P2 below measures the expiry: the host is killed without a word
		Stop-StartedProcesses
		$killedAt = Get-Date
		$gone = Wait-Until { $l = Get-ListedLobbies; ($null -ne $l) -and ($l.lobbies.Count -eq 0) } 60
		$seconds = [int]((Get-Date) - $killedAt).TotalSeconds
		Add-Result 'P1h' 'A lobby whose host vanished without a word disappears from the list by itself' $gone ("gone after about " + $seconds + " s (limit 20 s of silence)")
	}

	# ---------------------------------------------------------------------------------------------------------------
	if (Wants 'P2')
	{
		Write-Step "P2. A private lobby is joined with its 6 digit code, and is not in the list"
		Stop-StartedProcesses
		Remove-GameLogs @('_phost', '_pfriend')
		$hostProcess = Start-GameProcess ($common + @{ 'S3AIR_LOBBY_OPEN' = 'hostprivate'; 'S3AIR_LOGNAME' = '_phost'; 'S3AIR_PLAYER_NAME' = 'Hosty'; 'S3AIR_LOBBY_NAME' = 'Secret Room' })
		[void](Wait-Until { $script:code = Get-LobbyCodeFromLog '_phost'; [bool]$script:code } 120)
		$code = $script:code
		$list = Get-ListedLobbies
		Add-Result 'P2a' 'The private lobby has a code but is not in the public list' ([bool]$code -and $null -ne $list -and $list.lobbies.Count -eq 0) ("code: " + $(if ($code) { 'received' } else { 'none' }) + ", listed: " + $(if ($list) { $list.lobbies.Count } else { '?' }))
		if ($code)
		{
			$resolved = Invoke-Probe @('resolve', $code) | ConvertFrom-Json
			Add-Result 'P2b' 'The lobby server resolves the code, and answers with nothing about the host address' ($resolved.status -eq 0 -and $resolved.players -eq 1) (($resolved | ConvertTo-Json -Compress))
			$friendProcess = Start-GameProcess ($common + @{ 'S3AIR_LOBBY_OPEN' = 'join'; 'S3AIR_LOBBY_CODE' = $code; 'S3AIR_LOGNAME' = '_pfriend'; 'S3AIR_PLAYER_NAME' = 'Friendy' })
			$joined = Wait-Until { (Get-NetLog '_phost') -match 'a friend proved the code' } 120
			Add-Result 'P2c' 'A friend types the 6 digit code and joins the private lobby' $joined ""
			if ($joined) { Start-Sleep -Seconds 3; [void](Save-Shot $friendProcess 'P2-friend-joined'); [void](Save-Shot $hostProcess 'P2-host-private') }
		}
		Save-Logs 'P2' @('_phost', '_pfriend')
		Stop-StartedProcesses
		[void](Wait-Until { $l = Get-ListedLobbies; ($null -ne $l) -and ($l.lobbies.Count -eq 0) } 40)
	}

	# ---------------------------------------------------------------------------------------------------------------
	if (Wants 'P3')
	{
		Write-Step "P3. A wrong code and a mistyped long code: a plain message, no hang"
		foreach ($case in @(
			@{ Id = 'P3a'; Code = '999999'; Pattern = 'on screen: message There is no lobby with this code'; Title = 'A lobby code that does not exist ends in "no lobby with this code" within seconds'; Shot = 'P3-unknown-code' },
			@{ Id = 'P3b'; Code = '1234-5678-90124'; Pattern = 'on screen: message This long code has a typing mistake'; Title = 'A long code with a wrong check digit is refused at once, nothing is sent'; Shot = 'P3-typo-code' },
			@{ Id = 'P3c'; Code = '12345'; Pattern = 'on screen: message A lobby code has 6 digits'; Title = 'A code of the wrong length is refused at once with the rule'; Shot = 'P3-short-code' }))
		{
			Stop-StartedProcesses
			Remove-GameLogs @('_pfriend')
			$friendProcess = Start-GameProcess ($common + @{ 'S3AIR_LOBBY_OPEN' = 'join'; 'S3AIR_LOBBY_CODE' = $case.Code; 'S3AIR_LOGNAME' = '_pfriend'; 'S3AIR_PLAYER_NAME' = 'Friendy' })
			$told = Wait-Until { (Get-NetLog '_pfriend') -match [regex]::Escape($case.Pattern) } 90
			if ($told) { Start-Sleep -Seconds 1; [void](Save-Shot $friendProcess $case.Shot) }
			Add-Result $case.Id $case.Title $told ""
			Save-Logs $case.Id @('_pfriend')
		}
		Stop-StartedProcesses
	}

	# ---------------------------------------------------------------------------------------------------------------
	if (Wants 'P4')
	{
		Write-Step "P4. No lobby server set: the menu says so kindly, and nothing hangs"
		Stop-StartedProcesses
		Remove-GameLogs @('_pfriend')
		$friendProcess = Start-GameProcess @{ 'S3AIR_LOBBY_SERVER' = 'none'; 'S3AIR_LOBBY_OPEN' = 'screen'; 'S3AIR_LOBBY_SCREEN' = 'home,list,quick,create'; 'S3AIR_LOGNAME' = '_pfriend' }
		[void](Wait-Until { (Get-NetLog '_pfriend') -match 'showing the screen home' } 120)
		Start-Sleep -Seconds 2
		[void](Save-Shot $friendProcess 'P4-home-no-server')
		[void](Wait-Until { (Get-NetLog '_pfriend') -match 'showing the screen list' } 30)
		Start-Sleep -Seconds 2
		[void](Save-Shot $friendProcess 'P4-list-no-server')
		[void](Wait-Until { (Get-NetLog '_pfriend') -match 'showing the screen quick' } 30)
		$quickTold = Wait-Until { (Get-NetLog '_pfriend') -match 'on screen: message No lobby server is set yet' } 20
		Start-Sleep -Seconds 1
		[void](Save-Shot $friendProcess 'P4-quick-no-server')
		[void](Wait-Until { (Get-NetLog '_pfriend') -match 'showing the screen create' } 30)
		Start-Sleep -Seconds 2
		[void](Save-Shot $friendProcess 'P4-create-no-server')
		Add-Result 'P4a' 'Quick join without a lobby server says that none is set (no hang, back on the first screen)' $quickTold ""
		$logText = Get-NetLog '_pfriend'
		Add-Result 'P4b' 'The game stayed alive and responsive through all of it' (-not $friendProcess.HasExited -and $logText -match 'showing the screen create') ""
		Save-Logs 'P4' @('_pfriend')
		Stop-StartedProcesses
	}

	# ---------------------------------------------------------------------------------------------------------------
	if (Wants 'P5')
	{
		Write-Step "P5. Lobby server down (nothing listens there): the list and quick join say so within seconds"
		Stop-StartedProcesses
		Remove-GameLogs @('_pfriend')
		$friendProcess = Start-GameProcess @{ 'S3AIR_LOBBY_SERVER' = '127.0.0.1:29199'; 'S3AIR_LOBBY_OPEN' = 'screen'; 'S3AIR_LOBBY_SCREEN' = 'home,list,quick,home'; 'S3AIR_LOGNAME' = '_pfriend' }
		[void](Wait-Until { (Get-NetLog '_pfriend') -match 'showing the screen list' } 120)
		Start-Sleep -Seconds 4
		[void](Save-Shot $friendProcess 'P5-list-server-down')
		[void](Wait-Until { (Get-NetLog '_pfriend') -match 'showing the screen quick' } 30)
		$quickTold = Wait-Until { (Get-NetLog '_pfriend') -match 'the lookup ended: The lobby server does not answer' } 30
		Start-Sleep -Seconds 1
		[void](Save-Shot $friendProcess 'P5-quick-server-down')
		$logText = Get-NetLog '_pfriend'
		Add-Result 'P5a' 'With the lobby server down the game reports it as not reachable' ($logText -match 'the lobby server is now not reachable') ""
		Add-Result 'P5b' 'Quick join tells the player that the server does not answer, within 30 seconds' $quickTold ""
		Add-Result 'P5c' 'The game keeps running (no hang, no crash)' (-not $friendProcess.HasExited) ""
		Save-Logs 'P5' @('_pfriend')
		Stop-StartedProcesses
	}

	# ---------------------------------------------------------------------------------------------------------------
	if (Wants 'P6')
	{
		Write-Step "P6. Another version and a lobby that already plays are greyed out; quick join takes the open one"
		Stop-StartedProcesses
		Remove-GameLogs @('_phost', '_pfriend')
		Stop-FakeLobbies
		[void](Start-FakeLobby 'Other Version' 300 'zzzz9999' 2 4 0 'Eve')
		[void](Start-FakeLobby 'Already Playing' 300 'fake0000' 3 4 1 'Max')
		$hostProcess = Start-GameProcess ($common + @{ 'S3AIR_LOBBY_OPEN' = 'hostpublic'; 'S3AIR_LOGNAME' = '_phost'; 'S3AIR_PLAYER_NAME' = 'Hosty'; 'S3AIR_LOBBY_NAME' = 'Open Room'; 'S3AIR_LOBBY_MAX' = '3' })
		[void](Wait-Until { (Get-NetLog '_phost') -match 'lobby code \d{6}' } 120)
		$friendProcess = Start-GameProcess ($common + @{ 'S3AIR_LOBBY_OPEN' = 'screen'; 'S3AIR_LOBBY_SCREEN' = 'list'; 'S3AIR_LOGNAME' = '_pfriend'; 'S3AIR_PLAYER_NAME' = 'Friendy' })
		[void](Wait-Until { (Get-NetLog '_pfriend') -match 'showing the screen list' } 120)
		Start-Sleep -Seconds 5
		[void](Save-Shot $friendProcess 'P6-list-with-incompatible')
		$list = Get-ListedLobbies
		Add-Result 'P6a' 'The list holds three lobbies: the real one and the two fakes' ($null -ne $list -and $list.lobbies.Count -eq 3) ($(if ($list) { ($list.lobbies | ForEach-Object { $_.name + "(" + $_.players + "/" + $_.max + ")" }) -join ", " } else { '?' }))
		Stop-Process -Id $friendProcess.Id -Force -ErrorAction SilentlyContinue		# (only the browsing friend: the host stays)
		Start-Sleep -Seconds 1
		Remove-GameLogs @('_pfriend')
		$friendProcess = Start-GameProcess ($common + @{ 'S3AIR_LOBBY_OPEN' = 'quick'; 'S3AIR_LOGNAME' = '_pfriend'; 'S3AIR_PLAYER_NAME' = 'Friendy' })
		$picked = Wait-Until { (Get-NetLog '_pfriend') -match 'quick join picks the lobby "Open Room" \(1/3\)' } 120
		Add-Result 'P6b' 'Quick join skips the other version and the running game and picks "Open Room"' $picked ""
		$joined = Wait-Until { (Get-NetLog '_phost') -match 'a friend proved the code' } 90
		Add-Result 'P6c' 'It gets into that lobby' $joined ""
		Save-Logs 'P6' @('_phost', '_pfriend')
		Stop-StartedProcesses
		Stop-FakeLobbies
	}

	# ---------------------------------------------------------------------------------------------------------------
	if (Wants 'P7')
	{
		Write-Step "P7. The screens of the menu (screenshots in checks\last_logs, to be looked at)"
		Stop-StartedProcesses
		Remove-GameLogs @('_pfriend')
		Stop-FakeLobbies
		[void](Start-FakeLobby 'Speed Runners' 300 'fake0000' 2 4 0 'Eve')
		[void](Start-FakeLobby 'Chill Zone' 300 'zzzz9999' 1 4 0 'Max')
		[void](Start-FakeLobby 'Blue Sphere Club' 300 'fake0000' 3 4 1 'Tails Fan')
		[void](Start-FakeLobby 'Full House' 300 'fake0000' 4 4 0 'Amy')
		Start-Sleep -Seconds 3
		$friendProcess = Start-GameProcess ($common + @{ 'S3AIR_LOBBY_OPEN' = 'screen'; 'S3AIR_LOBBY_SCREEN' = 'home,list,create,lan,settings,join,quick'; 'S3AIR_LOBBY_TYPE' = '48a2b916'; 'S3AIR_LOGNAME' = '_pfriend'; 'S3AIR_PLAYER_NAME' = 'Friendy' })
		foreach ($name in @('home', 'list', 'create', 'lan', 'settings', 'join', 'quick'))
		{
			[void](Wait-Until { (Get-NetLog '_pfriend') -match ('showing the screen ' + $name) } 120)
			Start-Sleep -Seconds $(if (($name -eq 'list') -or ($name -eq 'join')) { 4 } else { 2 })
			[void](Save-Shot $friendProcess ('P7-' + $name))
		}
		Add-Result 'P7a' 'All screens of the menu were opened and photographed' (Test-Path (Join-Path $lastLogs 'P-P7-quick.png')) "P-P7-*.png in checks\last_logs"
		Add-Result 'P7b' 'Typing on the code screen keeps the digits and ignores letters (48a2b916 gives 6 digits)' ((Get-NetLog '_pfriend') -match 'typed on the code screen, the field now holds 6 digits') ""
		# The screens say where their entries end and where the text below them starts and ends (the Windows game has one entry more than the browser: LAN GAME)
		$layouts = @([regex]::Matches((Get-NetLog '_pfriend'), 'Lobby: layout (\w+) entries end at (\d+), the text below starts at (\d+) and ends at (\d+)') | ForEach-Object { [pscustomobject]@{ Screen = $_.Groups[1].Value; Rows = [int]$_.Groups[2].Value; Start = [int]$_.Groups[3].Value; End = [int]$_.Groups[4].Value } })
		$badLayouts = @($layouts | Where-Object { ($_.Start -lt $_.Rows) -or ($_.End -gt 213) })
		$layoutText = (@($layouts | ForEach-Object { "$($_.Screen) $($_.Rows)/$($_.Start)/$($_.End)" }) | Select-Object -Unique) -join ', '
		Add-Result 'P7c' 'On the screens of the menu the text below the entries starts after them and ends above the button bar (no overlap)' ((@($layouts | Where-Object { $_.Screen -eq 'START' }).Count -gt 0) -and ($badLayouts.Count -eq 0)) $layoutText
		Save-Logs 'P7' @('_pfriend')
		Stop-StartedProcesses
		Stop-FakeLobbies
	}
}
catch { Add-Result 'P?' 'The lobby check run completed' $false $_.Exception.Message }
finally
{
	Stop-StartedProcesses
	Stop-FakeLobbies
	Stop-LobbyServer
	if (Test-Path -LiteralPath $testHooksMarker) { Remove-Item -LiteralPath $testHooksMarker -Force }
	# The player's settings and save files: put back / compared
	if (Test-Path -LiteralPath $settingsBackup) { Copy-Item -LiteralPath $settingsBackup -Destination $settingsPath -Force; Remove-Item -LiteralPath $settingsBackup -Force }
	$saveChanged = @()
	foreach ($file in $saveFiles) { $now = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash + "|" + (Get-Item -LiteralPath $file.FullName).LastWriteTimeUtc.Ticks; if ($now -ne $saveBefore[$file.Name]) { $saveChanged += $file.Name } }
	$saveNow = @(Get-ChildItem -LiteralPath (Join-Path $appdata "storage") -File -ErrorAction SilentlyContinue)
	if ($saveNow.Count -ne $saveFiles.Count) { $saveChanged += "(number of files changed)" }
	Add-Result 'P8' 'The player save files (storage folder) are untouched: same content and same time stamps as before the run' ($saveChanged.Count -eq 0) (($saveChanged -join ", ") + " " + $saveFiles.Count + " files checked")
}

$passed = @($results | Where-Object { $_.Passed }).Count
$lines = @("Sonic 3 AirNet lobby checks (stage P) - " + (Get-Date -Format "yyyy-MM-dd HH:mm"), "")
foreach ($r in $results) { $lines += (($(if ($r.Passed) { "PASS" } else { "FAIL" })) + "  " + $r.Id.PadRight(4) + " " + $r.Title + $(if ($r.Detail) { "  -- " + $r.Detail } else { "" })) }
$lines += ""
$lines += ("Result: " + $passed + " of " + $results.Count + " passed.")
Set-Content -LiteralPath $reportPath -Value $lines -Encoding utf8
Write-Host ""
Write-Host ("Result: " + $passed + " of " + $results.Count + " lobby checks passed. Report: " + $reportPath) -ForegroundColor $(if ($passed -eq $results.Count) { "Green" } else { "Red" })
$checkRunMutex.ReleaseMutex()
$checkRunMutex.Dispose()
if ($passed -ne $results.Count) { exit 1 }
