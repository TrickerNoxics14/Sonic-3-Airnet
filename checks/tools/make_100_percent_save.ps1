<#
.SYNOPSIS
  Builds a "100%" save for Sonic 3 A.I.R. / Sonic 3 AirNet from the game's own persistent data format.

.DESCRIPTION
  Format (oxygenengine/source/oxygen/simulation/PersistentData.cpp): "OXY.PDATA", u16 version 0x0100, u32 entry count,
  then per entry: u32 key length, key bytes, u32 data size, data bytes. All numbers little endian.
  Files: s3air_unlocks.bin (Secrets, FinishedZoneActs), s3air_achievements.bin (Achievements),
         s3air_saveslots.bin (SRAM_Saveslots 0x50 bytes, SRAM_SaveslotsExt 0x100 bytes).

  Modes
    -Mode RoundTrip : parse the files in -StorageDir, write them again byte for byte, compare (writes nothing to StorageDir)
    -Mode Generate  : write the 100% files into -OutDir (nothing in StorageDir is touched)
    -Mode Install   : back up StorageDir (+ settings*.json) to -BackupRoot\<timestamp>, then write the 100% files into
                      StorageDir and a reusable copy into -OutDir

  Existing used data-select slots are kept (they stay in their slot); the 100% slots fill the empty ones.
#>
param(
	[ValidateSet('RoundTrip','Generate','Install')][string]$Mode = 'Generate',
	[string]$AppDataDir = (Join-Path $env:APPDATA 'Sonic3AIR'),
	[string]$StorageDir = (Join-Path $AppDataDir 'storage'),
	[string]$OutDir = (Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'Sonic3AIR-100-percent-save'),
	[string]$BackupRoot = (Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'Sonic3AIR-save-backups')
)
$ErrorActionPreference = 'Stop'

function Read-PData([string]$path)
{
	$b = [IO.File]::ReadAllBytes($path)
	if ([Text.Encoding]::ASCII.GetString($b, 0, 9) -ne 'OXY.PDATA') { throw "Not a persistent data file: $path" }
	$ver = [BitConverter]::ToUInt16($b, 9)
	if ($ver -ne 0x0100) { throw "Unsupported version $ver" }
	$count = [BitConverter]::ToUInt32($b, 11); $p = 15
	$entries = New-Object System.Collections.ArrayList
	for ($i = 0; $i -lt $count; $i++)
	{
		$kl = [BitConverter]::ToUInt32($b, $p); $p += 4
		$key = [Text.Encoding]::ASCII.GetString($b, $p, $kl); $p += $kl
		$dl = [BitConverter]::ToUInt32($b, $p); $p += 4
		$data = New-Object byte[] $dl; [Array]::Copy($b, $p, $data, 0, $dl); $p += $dl
		[void]$entries.Add([pscustomobject]@{ Key = $key; Data = $data })
	}
	if ($p -ne $b.Length) { throw "Trailing bytes in $path" }
	return ,$entries
}

function Write-PData([string]$path, $entries)
{
	$ms = New-Object IO.MemoryStream
	$w = New-Object IO.BinaryWriter($ms)
	$w.Write([Text.Encoding]::ASCII.GetBytes('OXY.PDATA'))
	$w.Write([uint16]0x0100)
	$w.Write([uint32]$entries.Count)
	foreach ($e in $entries)
	{
		$k = [Text.Encoding]::ASCII.GetBytes($e.Key)
		$w.Write([uint32]$k.Length); $w.Write($k)
		$w.Write([uint32]$e.Data.Length); $w.Write([byte[]]$e.Data)
	}
	$w.Flush()
	[IO.File]::WriteAllBytes($path, $ms.ToArray())
}

function U32([uint32[]]$values)
{
	$out = New-Object System.Collections.Generic.List[byte]
	foreach ($v in $values) { $out.AddRange([BitConverter]::GetBytes([uint32]$v)) }
	return ,$out.ToArray()
}

function Get-Entry($entries, [string]$key) { return ($entries | Where-Object { $_.Key -eq $key } | Select-Object -First 1) }

if ($Mode -eq 'RoundTrip')
{
	$ok = $true
	foreach ($f in Get-ChildItem -LiteralPath $StorageDir -Filter *.bin)
	{
		$tmp = Join-Path ([IO.Path]::GetTempPath()) ("rt_" + $f.Name)
		Write-PData $tmp (Read-PData $f.FullName)
		$same = ((Get-FileHash $tmp -Algorithm SHA256).Hash -eq (Get-FileHash $f.FullName -Algorithm SHA256).Hash)
		Remove-Item $tmp
		Write-Host ("{0}: round trip {1}" -f $f.Name, $(if ($same) { 'IDENTICAL' } else { 'DIFFERENT' }))
		if (-not $same) { $ok = $false }
	}
	if (-not $ok) { exit 1 }
	return
}

# ------------------------------------------------------------------ the 100% data
# Secrets (defines_secrets.lemon / SharedDatabase::Secret::Type): 0x00 K&T, 0x01 drop dash, 0x02 debug, 0x03 super peel-out,
# 0x04 Blue Sphere, 0x10 level select, 0x11 S&K title, 0x12 game speed, 0x18 Doomsday. (ids >= 0x20 count as unlocked anyway)
$secretIds = 0x00,0x01,0x02,0x03,0x04,0x10,0x11,0x12,0x18
$secretMask = [uint32]0
foreach ($id in $secretIds) { $secretMask = $secretMask -bor ([uint32]1 -shl $id) }

# Achievements (defines_achievements.lemon, 18 of them); state 1 = complete
$achievementIds = 0x0001,0x0002,0x0010,0x0011,0x0012,0x0013,0x0020,0x0101,0x0301,0x0501,0x0502,0x0601,0x0701,0x0801,0x0c01,0x1401,0x1501,0x1601
$ach = New-Object System.Collections.Generic.List[uint32]
$ach.Add([uint32]$achievementIds.Count)
foreach ($id in ($achievementIds | Sort-Object)) { $ach.Add([uint32]$id); $ach.Add([uint32]1) }

# FinishedZoneActs as PlayerUnlocksData::serializeFinishedZoneActs reads it: u32 all characters, u32 x3 per character (Sonic, Tails, Knuckles)
$finished = U32 @([uint32]::MaxValue, [uint32]::MaxValue, [uint32]::MaxValue, [uint32]::MaxValue)

# Data select slots, 10 bytes each (scripts/general/saveslots.lemon, scripts/menus/dataselect.lemon):
#  0 completion (0x80 empty, 0 running, 1 beaten, 2 all chaos emeralds, 3 all super emeralds), 1 random base (non zero),
#  2 (characters << 4) | next blue sphere stage, 3 zone (end of the game), 4-5 giant rings u16, 6-7 emeralds u16 (2 bits each, 3 = super),
#  8 lives, 9 continues
$chars = @(
	@{ Name = 'Sonic & Tails';     Chars = 0; Zone = 0x0e },
	@{ Name = 'Sonic';             Chars = 1; Zone = 0x0e },
	@{ Name = 'Tails';             Chars = 2; Zone = 0x0d },
	@{ Name = 'Knuckles';          Chars = 3; Zone = 0x0c },
	@{ Name = 'Knuckles & Tails';  Chars = 4; Zone = 0x0c }
)

$slotsPath = Join-Path $StorageDir 's3air_saveslots.bin'
$slots = New-Object byte[] 0x50
$ext = New-Object byte[] 0x100
for ($i = 0; $i -lt 8; $i++) { $slots[$i * 10] = 0x80 }
if (Test-Path $slotsPath)
{
	$old = Read-PData $slotsPath
	$o = Get-Entry $old 'SRAM_Saveslots'; if ($o -and $o.Data.Length -eq 0x50) { $slots = [byte[]]$o.Data.Clone() }
	$o = Get-Entry $old 'SRAM_SaveslotsExt'; if ($o -and $o.Data.Length -eq 0x100) { $ext = [byte[]]$o.Data.Clone() }
}
$next = 0
foreach ($c in $chars)
{
	while ($next -lt 8 -and ($slots[$next * 10] -band 0x80) -eq 0) { $next++ }	# keep slots that are in use
	if ($next -ge 8) { throw 'No free data select slot left' }
	$s = New-Object byte[] 10
	$s[0] = 3; $s[1] = 0x31 + $next; $s[2] = [byte]($c.Chars -shl 4); $s[3] = $c.Zone
	$s[4] = 0; $s[5] = 0; $s[6] = 0xff; $s[7] = 0xff; $s[8] = 5; $s[9] = 1
	[Array]::Copy($s, 0, $slots, $next * 10, 10)
	Write-Host ("slot {0}: {1}, completed, 7 chaos + 7 super emeralds, zone 0x{2:x2}" -f ($next + 1), $c.Name, $c.Zone)
	$next++
}

$result = @{
	's3air_unlocks.bin'      = @( [pscustomobject]@{ Key = 'Secrets'; Data = (U32 @($secretMask)) }, [pscustomobject]@{ Key = 'FinishedZoneActs'; Data = $finished } )
	's3air_achievements.bin' = @( [pscustomobject]@{ Key = 'Achievements'; Data = (U32 $ach.ToArray()) } )
	's3air_saveslots.bin'    = @( [pscustomobject]@{ Key = 'SRAM_Saveslots'; Data = $slots }, [pscustomobject]@{ Key = 'SRAM_SaveslotsExt'; Data = $ext } )
}
Write-Host ("Secrets mask 0x{0:x8}, achievements {1}" -f $secretMask, $achievementIds.Count)

if ($Mode -eq 'Install')
{
	$stamp = Get-Date -Format 'yyyy-MM-dd_HH-mm-ss'
	$bk = Join-Path $BackupRoot $stamp
	New-Item -ItemType Directory -Force -Path $bk | Out-Null
	Copy-Item -LiteralPath $StorageDir -Destination (Join-Path $bk 'storage') -Recurse
	foreach ($n in 'settings.json','settings_input.json') { $p = Join-Path $AppDataDir $n; if (Test-Path $p) { Copy-Item -LiteralPath $p -Destination $bk } }
	Write-Host "Backup: $bk"
}

$dirs = @($OutDir); if ($Mode -eq 'Install') { $dirs += $StorageDir }
foreach ($d in $dirs)
{
	New-Item -ItemType Directory -Force -Path $d | Out-Null
	foreach ($name in $result.Keys) { Write-PData (Join-Path $d $name) $result[$name] }
	Write-Host "Written to $d"
}
