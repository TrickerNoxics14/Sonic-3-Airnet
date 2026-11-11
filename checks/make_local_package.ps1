$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem

$root = Split-Path -Parent $PSScriptRoot
$sourceMod = Join-Path $root 'DropInCoop'
$stageRoot = Join-Path $PSScriptRoot '.local-package-stage'
$stageMod = Join-Path $stageRoot 'DropInCoop'
$outputZip = Join-Path $root 'DropInCoop_Local.zip'
$modJsonPath = Join-Path $stageMod 'mod.json'

if (-not (Test-Path -LiteralPath (Join-Path $sourceMod 'scripts\party.lemon'))) {
	throw "The co-op source mod is missing its party script: $sourceMod"
}

if (Test-Path -LiteralPath $stageRoot) {
	$resolvedStage = (Resolve-Path -LiteralPath $stageRoot).Path
	$resolvedChecks = (Resolve-Path -LiteralPath $PSScriptRoot).Path
	if (-not $resolvedStage.StartsWith($resolvedChecks, [StringComparison]::OrdinalIgnoreCase)) {
		throw "Refusing to clean staging path outside checks/: $resolvedStage"
	}
	Remove-Item -LiteralPath $resolvedStage -Recurse -Force
}
New-Item -ItemType Directory -Path $stageRoot | Out-Null
Copy-Item -LiteralPath $sourceMod -Destination $stageMod -Recurse

$metadata = Get-Content -LiteralPath $modJsonPath -Raw | ConvertFrom-Json
$metadata.Metadata.GameVersion = '26.03.28.0'
$metadata | ConvertTo-Json -Depth 32 | Set-Content -LiteralPath $modJsonPath -Encoding utf8

if (-not $metadata.UsesFeatures.FourPlayers) {
	throw 'The local mod package must declare the stock engine FourPlayers feature.'
}
if ($metadata.Metadata.GameVersion -ne '26.03.28.0') {
	throw 'The local mod package minimum game version was not set correctly.'
}

$files = @(Get-ChildItem -LiteralPath $stageRoot -File -Recurse)
$badFiles = @($files | Where-Object {
	$_.Extension -match '^\.(exe|dll|md|gen|smd)$' -or ($_.Extension -eq '.bin' -and $_.Length -gt 512KB)
})
if ($badFiles.Count -gt 0) {
	throw "Unexpected engine binary, ROM, or oversized binary in local mod package: $($badFiles[0].FullName)"
}

if (Test-Path -LiteralPath $outputZip) {
	Remove-Item -LiteralPath $outputZip -Force
}
[IO.Compression.ZipFile]::CreateFromDirectory($stageRoot, $outputZip, [IO.Compression.CompressionLevel]::Optimal, $false)

$archive = [IO.Compression.ZipFile]::OpenRead($outputZip)
try {
	$archiveModJson = $archive.Entries | Where-Object { ($_.FullName -replace '\\', '/') -eq 'DropInCoop/mod.json' } | Select-Object -First 1
	if ($null -eq $archiveModJson) { throw 'The ZIP is missing DropInCoop/mod.json.' }
	$reader = [IO.StreamReader]::new($archiveModJson.Open())
	try { $packedMetadata = $reader.ReadToEnd() | ConvertFrom-Json }
	finally { $reader.Dispose() }
	if ($packedMetadata.Metadata.GameVersion -ne '26.03.28.0' -or -not $packedMetadata.UsesFeatures.FourPlayers) {
		throw 'The zipped mod does not advertise the stock four-player capability and minimum version.'
	}
	$unexpectedEngine = @($archive.Entries | Where-Object { $_.FullName -match '\.(exe|dll|md|gen|smd)$' })
	if ($unexpectedEngine.Count -gt 0) { throw "The local mod ZIP contains an engine or ROM: $($unexpectedEngine[0].FullName)" }
	Write-Host "Created local-only mod: $outputZip"
	Write-Host "Verified $($archive.Entries.Count) mod files; minimum stock game version 26.03.28.0; no engine or ROM included."
}
finally {
	$archive.Dispose()
	$resolvedStage = (Resolve-Path -LiteralPath $stageRoot).Path
	$resolvedChecks = (Resolve-Path -LiteralPath $PSScriptRoot).Path
	if ($resolvedStage.StartsWith($resolvedChecks, [StringComparison]::OrdinalIgnoreCase)) {
		Remove-Item -LiteralPath $resolvedStage -Recurse -Force
	}
}
