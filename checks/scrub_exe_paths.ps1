# Overwrites the builder's workspace path inside a built exe with same-length filler (ASCII and UTF-16), in place.
# Needed for prebuilt third-party static libraries (SDL, Dear ImGui) whose assert strings carry the path of the machine
# that compiled them and are not rebuilt by the normal project build. The strings are only used in assertion text.
param([Parameter(Mandatory)][string]$ExePath, [Parameter(Mandatory)][string]$Prefix)
$ErrorActionPreference = "Stop"
Add-Type -TypeDefinition @'
using System;
public static class PathScrub {
	public static int Replace(byte[] data, byte[] needle) {
		int count = 0;
		for (int i = 0; i <= data.Length - needle.Length; i++) {
			if (data[i] != needle[0]) continue;
			int k = 1; while (k < needle.Length && data[i + k] == needle[k]) k++;
			if (k == needle.Length) {
				// keep a path-like shape: "C:\" then filler characters
				for (int j = 3; j < needle.Length; j++) data[i + j] = needle[0] == 0 ? (byte)0 : (byte)'_';
				count++; i += needle.Length - 1;
			}
		}
		return count;
	}
}
'@
$bytes = [IO.File]::ReadAllBytes($ExePath)
$ascii = [Text.Encoding]::ASCII.GetBytes($Prefix)
$wide = [Text.Encoding]::Unicode.GetBytes($Prefix)
# UTF-16 filler must keep the zero high bytes, so scrub wide strings by character instead
$a = [PathScrub]::Replace($bytes, $ascii)
$w = 0
for ($i = 0; $i -le $bytes.Length - $wide.Length; $i++) {
	if ($bytes[$i] -ne $wide[0]) { continue }
	$k = 1; while ($k -lt $wide.Length -and $bytes[$i + $k] -eq $wide[$k]) { $k++ }
	if ($k -eq $wide.Length) { for ($j = 6; $j -lt $wide.Length; $j += 2) { $bytes[$i + $j] = [byte][char]'_' }; $w++; $i += $wide.Length - 1 }
}
[IO.File]::WriteAllBytes($ExePath, $bytes)
Write-Output "Scrubbed $a ASCII and $w UTF-16 occurrence(s) of the build path in $ExePath"
