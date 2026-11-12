# Static guard for Lemon scripts: the compiler of this engine refuses an "if (...) statement" whose statement sits on the SAME line
# ("Expected single statement after 'if' keyword"). The statement has to be on the next line, or in braces.
# Every other script of the game and of the mods follows this; the test harnesses are written by hand and are the usual place where it slips in
# (and the game then stops on a "Script compilation error" dialog in the middle of a check run).
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File checks\lint_lemon_oneline_if.ps1            scan the repo, exit 1 if the form is found
#   powershell -NoProfile -ExecutionPolicy Bypass -File checks\lint_lemon_oneline_if.ps1 -SelfTest  prove the scanner catches the bad form
param(
	[switch]$SelfTest,
	[string[]]$Paths
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

function Find-OneLineIf([string[]]$lines)
{
	$found = @()
	$n = 0
	foreach ($line in $lines)
	{
		$n++
		$t = $line.TrimStart()
		if ($t -notmatch '^(else\s+)?if\s*\(') { continue }
		$start = $t.IndexOf('(')
		$depth = 0
		$end = -1
		for ($i = $start; $i -lt $t.Length; $i++)
		{
			$c = $t[$i]
			if ($c -eq '"') { $i++; while ($i -lt $t.Length -and $t[$i] -ne '"') { if ($t[$i] -eq '\') { $i++ }; $i++ }; continue }
			if ($c -eq '(') { $depth++ }
			elseif ($c -eq ')') { $depth--; if ($depth -eq 0) { $end = $i; break } }
		}
		if ($end -lt 0) { continue }	# condition continues on the next line
		$rest = ($t.Substring($end + 1) -replace '//.*$', '').Trim()
		if ($rest -ne '' -and $rest -ne '{' -and $rest -notmatch '^(&&|\|\|)') { $found += [pscustomobject]@{ Line = $n; Text = $line.Trim() } }
	}
	return $found
}

if ($SelfTest)
{
	$bad = @('function void f()', '{', "`tif (a % 2 == 0) Checks.report(`"x`", 1, `"y (z)`")", "`tif (b)", "`t{", "`t}", '}')
	$good = @('function void f()', '{', "`tif (a % 2 == 0)", "`t{", "`t`tChecks.report(`"x`", 1, `"y`")", "`t}", "`telse if (b)", "`t`treturn", "`tif (c &&", "`t    d)", "`t`treturn", '}')
	$badHits = @(Find-OneLineIf $bad)
	$goodHits = @(Find-OneLineIf $good)
	$ok = ($badHits.Count -eq 1 -and $badHits[0].Line -eq 3 -and $goodHits.Count -eq 0)
	Write-Host ("self test: bad form found " + $badHits.Count + " time(s) (expected 1), good forms flagged " + $goodHits.Count + " (expected 0)")
	if ($ok) { Write-Host "PASS lint_lemon_oneline_if self test"; exit 0 }
	Write-Host "FAIL lint_lemon_oneline_if self test" -ForegroundColor Red
	exit 1
}

if (-not $Paths -or $Paths.Count -eq 0)
{
	$Paths = @(Get-ChildItem -LiteralPath $root -Recurse -Filter *.lemon -File -ErrorAction SilentlyContinue |
		Where-Object { $_.FullName -notmatch '[\\/](bin|obj|\.git|last_logs|release|parked)[\\/]' } | ForEach-Object { $_.FullName })
}
$total = 0
foreach ($file in $Paths)
{
	foreach ($hit in (Find-OneLineIf (Get-Content -LiteralPath $file)))
	{
		$total++
		Write-Host ("FAIL " + $file.Replace($root + '\', '') + ":" + $hit.Line + "  statement on the same line as 'if': " + $hit.Text) -ForegroundColor Red
	}
}
if ($total -gt 0) { Write-Host ("lint_lemon_oneline_if: " + $total + " problem(s)") -ForegroundColor Red; exit 1 }
Write-Host ("PASS lint_lemon_oneline_if: " + $Paths.Count + " Lemon files, no 'if (...) statement' on one line")
exit 0
