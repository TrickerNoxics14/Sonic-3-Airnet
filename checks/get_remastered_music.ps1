# Gets the remastered soundtrack of Sonic 3 A.I.R. (audioremaster.bin, 126 MB) that the web build and the Windows package include:
# from the cache folder <workspace>\music_cache (not in git), else from the official download, checked against the size and SHA-256 of the version
# this fork is made for. Prints the path of the checked file.
#   $music = & checks\get_remastered_music.ps1
param()
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$cacheDir = Join-Path $root "music_cache"
$target = Join-Path $cacheDir "audioremaster.bin"
$url = "https://sonic3air.org/download/audioremaster.bin"
$expectedSize = 131677405
$expectedSha256 = "55F460DD9A3516494E56ABCD39E687FFF4A75011D66EAFCD83C1490A8E254FA4"

function Test-MusicPackage([string]$path)
{
	if (-not (Test-Path -LiteralPath $path)) { return $false }
	if ((Get-Item -LiteralPath $path).Length -ne $expectedSize) { return $false }
	return ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -eq $expectedSha256)
}

if (-not (Test-MusicPackage $target))
{
	New-Item -ItemType Directory -Force -Path $cacheDir | Out-Null
	$partial = $target + ".download"
	Write-Host "Downloading the remastered soundtrack from $url (126 MB, once; kept in music_cache) ..."
	$previousProgress = $ProgressPreference
	$ProgressPreference = 'SilentlyContinue'
	try { Invoke-WebRequest -Uri $url -OutFile $partial -UseBasicParsing }
	finally { $ProgressPreference = $previousProgress }
	if (-not (Test-MusicPackage $partial))
	{
		Remove-Item -LiteralPath $partial -Force -ErrorAction SilentlyContinue
		throw "The downloaded audioremaster.bin is not the expected version (size $expectedSize, SHA-256 $expectedSha256). The official file may have changed: check it and update the numbers in get_remastered_music.ps1."
	}
	Move-Item -LiteralPath $partial -Destination $target -Force
}
Write-Output $target
