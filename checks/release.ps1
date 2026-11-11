# Builds a release of the online fork and, unless -DryRun, publishes it on GitHub.
#
#   checks\release.ps1 -Version 1.2.3 -Notes "What is new" -DryRun        (everything except tag, push and upload)
#   checks\release.ps1 -Version 1.2.3 -Notes "What is new" [-Build]       (the real thing: needs a committed tree, git push rights and the gh CLI logged in)
#
# What it does:
#   1. fetches the tags from origin, checks the version (x.y.z, newer than every existing tag vX.Y.Z), that the working tree is committed, and that the tag does not exist
#   2. builds the game (always for a real release; with -Build for a dry run, which otherwise only warns when Sonic3AIR.exe is older than the source), then runs make_package.ps1 -Version (which writes version.txt into the package and refuses saves, ROMs, test fixtures)
#   3. refuses to go on if the build embeds a personal Windows path (the warning of make_package.ps1)
#   4. inspects the zip once more with the rules the in-game updater applies (it would refuse a package with unsafe names or user data)
#   5. writes release\vX.Y.Z\  Sonic3Airnet.zip, Sonic3Airnet.zip.sha256, Sonic3Airnet.manifest.json (version, asset, size, sha256)
#   6. unless -DryRun: git tag vX.Y.Z, git push the tag, gh release create with the three files
# The in-game updater (Options > Check for updates) reads releases/latest of the repository in settings.json (Online / UpdateRepo) and verifies the zip against
# the manifest. Private repositories: nobody without a login can download release files from them, so friends see "No release was found" until the releases are public.
param(
	[Parameter(Mandatory = $true)][string]$Version,
	[string]$Notes = "",
	[switch]$DryRun,
	[switch]$Build,
	[switch]$AllowDirty		# only together with -DryRun: look at the result of a work in progress; never for a real release
)

$ErrorActionPreference = "Stop"
$checksDir = $PSScriptRoot
$root = Split-Path -Parent $checksDir
$assetName = "Sonic3Airnet.zip"
$manifestName = "Sonic3Airnet.manifest.json"
$problems = New-Object System.Collections.ArrayList

function Fail([string]$message)
{
	Write-Host ("RELEASE REFUSED: " + $message) -ForegroundColor Red
	exit 1
}

function Run-Git
{
	# git reports progress (for example while pushing) on stderr; that must not count as an error under ErrorActionPreference Stop
	$previousPreference = $ErrorActionPreference
	$ErrorActionPreference = 'Continue'
	try { $output = & git -C $root @args 2>&1 } finally { $ErrorActionPreference = $previousPreference }
	if ($LASTEXITCODE -ne 0) { throw ("git " + ($args -join " ") + " failed: " + ($output -join "`n")) }
	return $output
}

# ---- 1. Version, tree, tag ---------------------------------------------------------------------------------------------------------
if ($Version -notmatch '^(\d{1,5})\.(\d{1,5})\.(\d{1,5})$') { Fail "Version must look like 1.2.3 (digits and dots only), got '$Version'" }
$tag = "v" + $Version
if ($AllowDirty -and -not $DryRun) { Fail "-AllowDirty is only allowed together with -DryRun" }

$dirty = @(Run-Git status --porcelain)
if ($dirty.Count -gt 0)
{
	if (-not $AllowDirty) { Fail ("the working tree has " + $dirty.Count + " uncommitted change(s). Commit them first (git status shows them). First one: " + $dirty[0]) }
	Write-Host ("note: dry run with " + $dirty.Count + " uncommitted change(s) (-AllowDirty). A real release would be refused.") -ForegroundColor Yellow
}

# Tags made from another clone must count too: refresh them first (the "newer than every tag" rule below reads the local tag list).
# A real release needs this to work; a dry run only warns.
$env:GIT_TERMINAL_PROMPT = "0"		# never wait for a password prompt
$fetchOk = $false
$fetchOutput = @()
try
{
	$fetchOutput = & git -C $root fetch --tags --quiet origin 2>&1
	$fetchOk = ($LASTEXITCODE -eq 0)
}
catch { $fetchOutput = @($_.Exception.Message) }
if (-not $fetchOk)
{
	if (-not $DryRun) { Fail ("could not fetch the tags from origin, so a newer release made elsewhere cannot be ruled out: " + ($fetchOutput -join " ")) }
	Write-Host ("note: could not fetch the tags from origin (" + ($fetchOutput -join " ") + "); the version check only knows the local tags.") -ForegroundColor Yellow
}
$existingTags = @(Run-Git tag --list "v*")
if ($existingTags -contains $tag) { Fail "the tag $tag already exists" }
function ConvertTo-VersionKey([string]$text)
{
	if ($text -match '^v?(\d+)\.(\d+)\.(\d+)$') { return ([int64]$Matches[1] * 10000000000 + [int64]$Matches[2] * 100000 + [int64]$Matches[3]) }
	return -1
}
$newKey = ConvertTo-VersionKey $Version
foreach ($existing in $existingTags)
{
	if ((ConvertTo-VersionKey $existing) -ge $newKey) { Fail "version $Version is not newer than the existing tag $existing (the updater never goes back to an older version)" }
}

# ---- 2. Build and package ----------------------------------------------------------------------------------------------------------
# A real release is always built here from the committed tree (-Build is implied): the package must be the program of the tagged source, not
# whatever was left in the build folder. A dry run may reuse an existing build, but says so when it is older than the source.
$mustBuild = $Build -or (-not $DryRun)
if ($mustBuild)
{
	& (Join-Path $checksDir "build_online_stable.ps1")
	if ($LASTEXITCODE -ne 0) { Fail "the build failed" }
}
$exeForCheck = Join-Path $root "sonic3air_online_stable_source\Oxygen\sonic3air\bin\Release_x64\Sonic3AIR.exe"
if (-not (Test-Path -LiteralPath $exeForCheck)) { Fail "Sonic3AIR.exe was not found at $exeForCheck (run with -Build)" }
$sourceFiles = @(Run-Git ls-files -- sonic3air_online_stable_source) | ForEach-Object { Join-Path $root $_ } | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf }
$newestSource = $sourceFiles | ForEach-Object { Get-Item -LiteralPath $_ } | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
if ($null -ne $newestSource -and (Get-Item -LiteralPath $exeForCheck).LastWriteTimeUtc -lt $newestSource.LastWriteTimeUtc)
{
	$staleText = "Sonic3AIR.exe is older than the source file " + $newestSource.FullName + ": the package would not contain this source's program (build with -Build)"
	if (-not $DryRun) { Fail $staleText }
	Write-Host ("note: " + $staleText) -ForegroundColor Yellow
}
$helperPath = Join-Path $checksDir "package_files\update_helper.ps1"
$parseErrors = $null
[void][System.Management.Automation.Language.Parser]::ParseFile($helperPath, [ref]$null, [ref]$parseErrors)
if ($parseErrors -and $parseErrors.Count -gt 0) { Fail ("update_helper.ps1 has a syntax error: " + $parseErrors[0].Message) }

$packageWarnings = New-Object System.Collections.ArrayList
$packageOutput = & (Join-Path $checksDir "make_package.ps1") -Version $Version 3>&1
foreach ($item in $packageOutput)
{
	if ($item -is [System.Management.Automation.WarningRecord]) { [void]$packageWarnings.Add($item.Message); Write-Host ("WARNING: " + $item.Message) -ForegroundColor Yellow }
	else { Write-Host $item }
}

# ---- 3. Personal path in the executable ----------------------------------------------------------------------------------------------
foreach ($warning in $packageWarnings)
{
	if ($warning -match 'personal path') { Fail ("make_package.ps1 warns: " + $warning) }
}

# ---- 4. Inspect the zip with the updater's rules -----------------------------------------------------------------------------------------
$zipPath = Join-Path $root $assetName
if (-not (Test-Path -LiteralPath $zipPath)) { Fail "$assetName was not created" }
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::OpenRead($zipPath)
try
{
	$names = @($archive.Entries | ForEach-Object { $_.FullName.Replace('\', '/') })
	$hasTop = ($names.Count -gt 0) -and (@($names | Where-Object { $_ -notmatch '^Sonic3Airnet(/|$)' }).Count -eq 0)
	$unsafe = New-Object System.Collections.ArrayList
	$forbidden = New-Object System.Collections.ArrayList
	$versionEntry = $null
	foreach ($entry in $archive.Entries)
	{
		$name = $entry.FullName.Replace('\', '/')
		$relative = if ($hasTop) { $name -replace '^Sonic3Airnet/?', '' } else { $name }
		if ($relative -eq '') { continue }
		if ($name.EndsWith('/')) { continue }
		# Unsafe names (the updater refuses the whole package for one of them)
		if ($relative -match '(^/|:|(^|/)\.\.?(/|$)|//|[<>"|?*\x00-\x1f]|[. ](/|$))') { [void]$unsafe.Add($name); continue }
		# Things an update must never carry
		if ($relative -match '(?i)(^|/)(saves|states|savedata|mods)(/|$)' -or
			$relative -match '(?i)(^|/)settings[^/]*\.json$' -or $relative -match '(?i)(^|/)active-mods\.json$' -or $relative -match '(?i)(^|/)testhooks\.enabled$' -or
			$relative -match '(?i)(^|/)(ChecksHarness|ActHarness|BossHarness|NetPartyHarness|PartyHarness|TeleportHarness)(/|$)' -or
			$relative -match '(?i)sonic_knuckles|sonic_3_and|_harness' -or $relative -match '(?i)\.(gen|smd|md|sav|srm)$' -or
			($relative -match '(?i)\.bin$' -and $entry.Length -gt 512KB)) { [void]$forbidden.Add($name) }
		if ($relative -eq 'version.txt') { $versionEntry = $entry }
	}
	if ($unsafe.Count -gt 0) { Fail ("the package has unsafe file names, the updater would refuse it: " + $unsafe[0]) }
	if ($forbidden.Count -gt 0) { Fail ("the package contains a save, ROM, settings file or test fixture: " + $forbidden[0]) }
	if (-not ($names | Where-Object { $_ -match '(^|/)Sonic3AIR\.exe$' })) { Fail "the package has no Sonic3AIR.exe" }
	if (-not ($names | Where-Object { $_ -match '(^|/)update_helper\.ps1$' })) { Fail "the package has no update_helper.ps1 (the updater of this version could not install later updates)" }
	if ($null -eq $versionEntry) { Fail "the package has no version.txt" }
	$reader = New-Object IO.StreamReader($versionEntry.Open())
	try { $packagedVersion = $reader.ReadToEnd().Trim() } finally { $reader.Dispose() }
	if ($packagedVersion -ne $Version) { Fail "version.txt in the package says '$packagedVersion', not '$Version'" }
}
finally { $archive.Dispose() }

# ---- 5. Release files ---------------------------------------------------------------------------------------------------------------------
$releaseDir = Join-Path $root ("release\" + $tag)
if (Test-Path -LiteralPath $releaseDir) { Remove-Item -LiteralPath $releaseDir -Recurse -Force }
New-Item -ItemType Directory -Path $releaseDir -Force | Out-Null
$releaseZip = Join-Path $releaseDir $assetName
Copy-Item -LiteralPath $zipPath -Destination $releaseZip
$hash = (Get-FileHash -LiteralPath $releaseZip -Algorithm SHA256).Hash.ToLowerInvariant()
$size = (Get-Item -LiteralPath $releaseZip).Length
Set-Content -LiteralPath (Join-Path $releaseDir ($assetName + ".sha256")) -Value ($hash + "  " + $assetName) -Encoding ascii
$manifest = [ordered]@{ version = $Version; asset = $assetName; size = [int64]$size; sha256 = $hash }
Set-Content -LiteralPath (Join-Path $releaseDir $manifestName) -Value ($manifest | ConvertTo-Json) -Encoding ascii

Write-Host ""
Write-Host ("Release " + $tag + " prepared in " + $releaseDir) -ForegroundColor Green
Write-Host ("  " + $assetName + "  " + [math]::Round($size / 1MB, 1) + " MB")
Write-Host ("  SHA-256 " + $hash)
if ($Notes -eq "") { $Notes = "Sonic 3 AirNet $Version" }

$files = @($releaseZip, (Join-Path $releaseDir ($assetName + ".sha256")), (Join-Path $releaseDir $manifestName))
if ($DryRun)
{
	Write-Host ""
	Write-Host "DRY RUN: nothing was tagged, pushed or uploaded. A real run would do:" -ForegroundColor Cyan
	Write-Host ("  git tag -a " + $tag + " -m `"Release " + $Version + "`"")
	Write-Host ("  git push origin " + $tag)
	Write-Host ("  gh release create " + $tag + " <3 files> --title `"" + $tag + "`" --notes `"" + $Notes + "`"")
	Write-Host "RELEASE DRY RUN OK"
	exit 0
}

# ---- 6. Publish -----------------------------------------------------------------------------------------------------------------------------
& gh auth status 2>&1 | Out-Null
if ($LASTEXITCODE -ne 0) { Fail "the gh CLI is not logged in (gh auth login). Nothing was tagged or pushed." }
Run-Git tag -a $tag -m ("Release " + $Version) | Out-Null
Run-Git push origin $tag | Out-Null
& gh release create $tag @files --title $tag --notes $Notes
if ($LASTEXITCODE -ne 0)
{
	Write-Host ("The tag " + $tag + " was pushed but gh release create failed. Fix the problem and run: gh release create " + $tag + " <files in " + $releaseDir + ">") -ForegroundColor Yellow
	exit 1
}
Write-Host ("Published " + $tag) -ForegroundColor Green
Write-Host "Note: if the repository is private, friends cannot download the update from it (see README_ONLINE.txt)."
exit 0
