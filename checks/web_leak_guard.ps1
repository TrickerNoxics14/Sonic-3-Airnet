# Leak guard for the web build: the output folder of build_web.ps1 (and the exact tree that publish_pages.ps1 is about to publish) goes to a PUBLIC page, so it must not
# carry anything of the machine it was built on. Every file of the folder is read in full, text AND binary (.wasm, .data, pictures), and searched for:
#   R1  the Windows user name and the computer name of the builder (as text, UTF-16 and UTF-32: a wide string in a binary looks different from a text string)
#   R2  the builder's folders: the workspace, the user profile, AppData, Temp (also the short 8.3 names), the folders passed with -ExtraFolder (the emsdk), the workspace folder's own name;
#       each in the forms backslash, slash, doubled backslash (JSON) and %20 (URL)
#   R3  any "<drive>:\Users\" or "<drive>:/Users/" path, whoever it belongs to ("Documents and Settings" too)
#   R4  an e-mail address: the one(s) the builder's git is set up with (full address, local part, domain; the public GitHub noreply address is not a secret and is skipped),
#       and any other address-shaped text (example.com/.org/.net, localhost and the GitHub noreply host are accepted)
#   R5  access tokens and private keys (GitHub tokens, AWS key ids, Anthropic and Slack keys, PEM private keys)
#   R6  a ROM: a file of exactly 4 MiB or 2 MiB (a Mega Drive cartridge, S3 and S&K alone are 2 MiB) or a file with a Mega Drive header ("SEGA ..." at 0x100, or at 0x300 with a
#       512 byte copier header), whatever its name
#   R7  names: a file or folder called savedata / saves / states / storage / persistent / idbfs / roms / userdata / .git, settings.json / config.json, a ROM or save file extension
#       (.bin .gen .smd .md .32x .sms .rom .srm .sav .sram .state*), a name that says Knuckles / S3K / Sonic 3
#   R8  nothing to scan (a missing or empty folder passes no test, so it fails)
# The values it looks for are NEVER written into this file: they are read from the environment of the machine that runs it ($env:USERNAME, $env:COMPUTERNAME, the workspace
# path, the profile folders, `git config user.email`). On a failure it prints the file, the rule and where in the file, never the value that was found.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File checks\web_leak_guard.ps1 -Dir web_build [-ExtraFolder <folder>,...]   scan, exit 1 on any finding
#   powershell -NoProfile -ExecutionPolicy Bypass -File checks\web_leak_guard.ps1 -SelfTest                                    leaking fixtures (made-up names) must be caught by their rule, a clean one must pass
param([string]$Dir, [string[]]$ExtraFolder = @(), [switch]$SelfTest)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$latin1 = [Text.Encoding]::GetEncoding(28591)
$ruleText = @{
	R1 = "Windows user name or computer name"; R2 = "a folder of the builder's machine"; R3 = "a Windows user folder path (<drive>:\Users\...)"; R4 = "an e-mail address"
	R5 = "an access token or private key"; R6 = "a ROM (size or Mega Drive header)"; R7 = "a name that belongs to save data, storage, a ROM or source control"; R8 = "nothing to scan"
}
$romSizes = @(4194304, 2097152)
$mailAllowed = '(^|\.)(users\.noreply\.github\.com|example\.(com|org|net)|localhost)$'
$mailPublicHosts = '(^|\.)(users\.noreply\.github\.com|github\.com|example\.(com|org|net)|localhost)$'
# Addresses of the original authors in the licence headers of the third-party upscaler shaders (upscaler_hqx and upscaler_xbrz-freescale in the oxygenengine data, packed into sonic3air.data):
# they sit in public source files of this repository and must ship with the code. Anything else that looks like an address is a finding; add an address here only after looking at where it sits.
$mailThirdParty = @('maxst@hiend3d.com', 'grom@zeminvaders.net', 'jules@aerix.nl', 'sergiogdb@gmail.com')

function New-Needle([string]$rule, [string]$label, [string]$value) { [pscustomobject]@{ Rule = $rule; Label = $label; Value = $value } }

# One value as it can sit in a file: UTF-8 (= text), UTF-16LE (wchar_t on Windows), UTF-32LE (wchar_t in WebAssembly); each as the Latin-1 string of its bytes
function Get-Variants($needles)
{
	$list = New-Object System.Collections.ArrayList
	foreach ($n in $needles)
	{
		if (-not $n.Value) { continue }
		[void]$list.Add([pscustomobject]@{ Rule = $n.Rule; Label = $n.Label; Encoding = "text"; Text = $latin1.GetString([Text.Encoding]::UTF8.GetBytes($n.Value)) })
		[void]$list.Add([pscustomobject]@{ Rule = $n.Rule; Label = $n.Label; Encoding = "UTF-16"; Text = $latin1.GetString([Text.Encoding]::Unicode.GetBytes($n.Value)) })
		[void]$list.Add([pscustomobject]@{ Rule = $n.Rule; Label = $n.Label; Encoding = "UTF-32"; Text = $latin1.GetString([Text.Encoding]::UTF32.GetBytes($n.Value)) })
	}
	return ,$list
}

# A folder as it is written in different places: C:\a\b  C:/a/b  C:\\a\\b (JSON)  C:/a%20b (URL)
function Add-PathNeedles($list, [string]$rule, [string]$label, [string]$path)
{
	if (-not $path) { return }
	$p = $path.TrimEnd('\', '/')
	if ($p.Length -lt 8) { return }		# a drive or a one-word folder would match everywhere
	$back = $p.Replace('/', '\'); $fwd = $p.Replace('\', '/')
	foreach ($form in @($back, $fwd, $back.Replace('\', '\\'), $fwd.Replace(' ', '%20')) | Select-Object -Unique) { [void]$list.Add((New-Needle $rule $label $form)) }
}

# The patterns for given names and folders (the real machine's values in a real run, made-up ones in the self test)
function Get-NeedleSet([string[]]$UserNames, [string[]]$ComputerNames, [string[]]$Folders, [string[]]$Emails)
{
	$list = New-Object System.Collections.ArrayList
	foreach ($u in ($UserNames | Where-Object { $_ } | Select-Object -Unique)) { if ($u.Length -ge 4) { [void]$list.Add((New-Needle "R1" "the Windows user name" $u)) } }
	foreach ($c in ($ComputerNames | Where-Object { $_ } | Select-Object -Unique)) { if ($c.Length -ge 4) { [void]$list.Add((New-Needle "R1" "the computer name" $c)) } }
	foreach ($f in ($Folders | Where-Object { $_ } | Select-Object -Unique)) { Add-PathNeedles $list "R2" "a folder of the builder's machine" $f }
	foreach ($e in ($Emails | Where-Object { $_ -and $_.Contains('@') } | Select-Object -Unique))
	{
		$at = $e.LastIndexOf('@'); $local = $e.Substring(0, $at); $domain = $e.Substring($at + 1)
		if ($domain -match $mailPublicHosts) { continue }		# the public GitHub noreply address is meant to be public
		[void]$list.Add((New-Needle "R4" "the builder's e-mail address" $e))
		if ($local.Length -ge 4) { [void]$list.Add((New-Needle "R4" "the local part of the builder's e-mail address" $local)) }
		if ($domain.Length -ge 6) { [void]$list.Add((New-Needle "R4" "the domain of the builder's e-mail address" $domain)) }
	}
	# whoever's they are: a path into a Windows user folder
	foreach ($g in @(':\Users\', ':/Users/', ':\\Users\\', ':\Documents and Settings\', ':/Documents and Settings/')) { [void]$list.Add((New-Needle "R3" "a Windows user folder path" $g)) }
	return ,$list
}

function Get-ShortPath([string]$path)
{
	try { if (Test-Path -LiteralPath $path) { return (New-Object -ComObject Scripting.FileSystemObject).GetFolder($path).ShortPath } } catch { }
	return $null
}

# The real values of this machine, read when the script runs
function Get-RealNeedles([string[]]$ExtraFolder)
{
	$users = @($env:USERNAME, [Environment]::UserName)
	$computers = @($env:COMPUTERNAME, [Environment]::MachineName)
	$folders = @($root, $PSScriptRoot, (Split-Path -Leaf $root), $env:USERPROFILE, $env:HOME, $env:APPDATA, $env:LOCALAPPDATA, $env:TEMP, $env:TMP, [IO.Path]::GetTempPath()) + @($ExtraFolder)
	$leaf = Split-Path -Leaf $root
	$folders = @($folders | Where-Object { $_ -and (($_ -ne $leaf) -or $leaf.Length -ge 6) })
	foreach ($f in @($folders)) { $short = Get-ShortPath $f; if ($short) { $folders += $short } }
	$emails = @($env:GIT_AUTHOR_EMAIL, $env:GIT_COMMITTER_EMAIL)
	try
	{
		$git = Get-Command git -ErrorAction SilentlyContinue
		if ($git)
		{
			foreach ($args2 in @(@("config", "--global", "--get-all", "user.email"), @("-C", $root, "config", "--get-all", "user.email"))) { $emails += @(& git @args2 2>$null | ForEach-Object { "$_".Trim() }) }
		}
	} catch { }
	# a plain folder name without a path (the workspace folder's own name) is looked for as it is
	$set = Get-NeedleSet $users $computers @($folders | Where-Object { $_ -match '[\\/]' }) $emails
	foreach ($f in @($folders | Where-Object { $_ -notmatch '[\\/]' })) { [void]$set.Add((New-Needle "R2" "the name of the workspace folder" $f)) }
	return $set
}

function Find-Emails([string]$text, $found)
{
	$localEnd = New-Object Text.RegularExpressions.Regex('[A-Za-z0-9._%+\-]+$')
	$domainStart = New-Object Text.RegularExpressions.Regex('^[A-Za-z0-9\-]+(?:\.[A-Za-z0-9\-]+)*\.[A-Za-z]{2,24}(?![A-Za-z0-9\-])')
	$i = $text.IndexOf('@')
	while ($i -ge 0)
	{
		if ($i -gt 0 -and $i + 1 -lt $text.Length)
		{
			$a = [Math]::Max(0, $i - 64)
			$left = $text.Substring($a, $i - $a)
			if ($localEnd.IsMatch($left))
			{
				$right = $text.Substring($i + 1, [Math]::Min(255, $text.Length - $i - 1))
				$m = $domainStart.Match($right)
				$local = $localEnd.Match($left).Value
				if ($m.Success -and ($m.Value -notmatch $mailAllowed) -and ($mailThirdParty -notcontains ($local + '@' + $m.Value).ToLower())) { [void]$found.Add($i - $local.Length) }
			}
		}
		$i = $text.IndexOf('@', $i + 1)
	}
}

function Find-Tokens([string]$text, $found)
{
	# prefix, then the shape of the whole token (a window after the prefix, so a long file is not searched with a slow expression)
	$kinds = @(
		@('ghp_', '^gh[pousr]_[A-Za-z0-9]{30,}'), @('gho_', '^gh[pousr]_[A-Za-z0-9]{30,}'), @('ghu_', '^gh[pousr]_[A-Za-z0-9]{30,}'), @('ghs_', '^gh[pousr]_[A-Za-z0-9]{30,}'), @('ghr_', '^gh[pousr]_[A-Za-z0-9]{30,}'),
		@('github_pat_', '^github_pat_[A-Za-z0-9_]{20,}'), @('AKIA', '^AKIA[0-9A-Z]{16}'), @('sk-ant-', '^sk-ant-[A-Za-z0-9_\-]{20,}'), @('xox', '^xox[abprs]-[A-Za-z0-9\-]{10,}'),
		@('-----BEGIN ', '^-----BEGIN [A-Z ]*PRIVATE KEY-----'))
	foreach ($k in $kinds)
	{
		$i = $text.IndexOf($k[0], [StringComparison]::Ordinal)
		while ($i -ge 0)
		{
			$window = $text.Substring($i, [Math]::Min(300, $text.Length - $i))
			if ($window -cmatch $k[1]) { [void]$found.Add($i) }
			$i = $text.IndexOf($k[0], $i + 1, [StringComparison]::Ordinal)
		}
	}
}

function Test-RomHeader([string]$text)
{
	foreach ($offset in @(0x100, 0x300)) { if ($text.Length -ge $offset + 12 -and $text.Substring($offset, 4) -ceq 'SEGA') { return $offset } }
	return -1
}

# Scans a folder; returns the findings (file, rule, what) and what was read
function Invoke-LeakScan([string]$Folder, $Needles)
{
	$findings = New-Object System.Collections.ArrayList
	$result = [pscustomobject]@{ Findings = $findings; Files = 0; Bytes = [int64]0 }
	if (-not (Test-Path -LiteralPath $Folder -PathType Container)) { [void]$findings.Add([pscustomobject]@{ File = $Folder; Rule = "R8"; What = "the folder does not exist" }); return $result }
	# every file and folder below $Folder (hidden ones too), with its path relative to $Folder built from the names on the way (not by cutting a prefix: a path may be given in its 8.3 short form)
	$items = New-Object System.Collections.ArrayList
	$todo = New-Object System.Collections.Stack
	$todo.Push(@((Resolve-Path -LiteralPath $Folder).ProviderPath, ""))
	while ($todo.Count -gt 0)
	{
		$entry = $todo.Pop()
		foreach ($child in @(Get-ChildItem -LiteralPath $entry[0] -Force))
		{
			$rel = $(if ($entry[1]) { $entry[1] + "/" + $child.Name } else { $child.Name })
			[void]$items.Add([pscustomobject]@{ Rel = $rel; Path = $child.FullName; IsDir = [bool]$child.PSIsContainer })
			if ($child.PSIsContainer) { $todo.Push(@($child.FullName, $rel)) }
		}
	}
	$files = @($items | Where-Object { -not $_.IsDir })
	if ($files.Count -eq 0) { [void]$findings.Add([pscustomobject]@{ File = $Folder; Rule = "R8"; What = "the folder holds no file (a scan of nothing proves nothing)" }); return $result }
	$variants = Get-Variants $Needles
	foreach ($item in $items)
	{
		$rel = $item.Rel
		$segments = $rel.Split('/')
		# R7: names (every folder on the way, and the file itself)
		for ($s = 0; $s -lt $segments.Count; $s++)
		{
			$seg = $segments[$s]
			if ($seg -match '(?i)^(savedata|saves?|states|storage|persistent|persistentdata|idbfs|userdata|roms?|\.git|\.svn|\.hg|settings\.json|config\.json)$') { [void]$findings.Add([pscustomobject]@{ File = $rel; Rule = "R7"; What = "name '$seg' (save data, settings, storage, ROM or source control)" }); break }
		}
		$leaf = $segments[$segments.Count - 1]
		if (-not $item.IsDir)
		{
			if ($leaf -match '(?i)\.(bin|gen|smd|md|32x|sms|rom|srm|sav|sram|ssm|state\d*|st\d)$') { [void]$findings.Add([pscustomobject]@{ File = $rel; Rule = "R7"; What = "ROM or save file extension" }) }
			elseif ($leaf -match '(?i)knuckles|wsonic3|(^|[^a-z0-9])s3k([^a-z0-9]|$)|sonic[ _-]*3[ _.-]') { [void]$findings.Add([pscustomobject]@{ File = $rel; Rule = "R7"; What = "a name that says Sonic 3 / Knuckles / S3K" }) }
		}
	}
	foreach ($f in $files)
	{
		$rel = $f.Rel
		$bytes = [IO.File]::ReadAllBytes($f.Path)
		$result.Files++; $result.Bytes += $bytes.Length
		if ($romSizes -contains $bytes.Length) { [void]$findings.Add([pscustomobject]@{ File = $rel; Rule = "R6"; What = "the file is exactly $($bytes.Length) bytes (the size of a ROM)" }) }
		$text = $latin1.GetString($bytes)
		$header = Test-RomHeader $text
		if ($header -ge 0) { [void]$findings.Add([pscustomobject]@{ File = $rel; Rule = "R6"; What = "a Mega Drive ROM header at byte offset $header" }) }
		foreach ($v in $variants)
		{
			$at = $text.IndexOf($v.Text, [StringComparison]::OrdinalIgnoreCase)
			if ($at -ge 0) { [void]$findings.Add([pscustomobject]@{ File = $rel; Rule = $v.Rule; What = "$($v.Label) ($($v.Encoding)) at byte offset $at" }) }
		}
		$mails = New-Object System.Collections.ArrayList
		Find-Emails $text $mails
		if ($mails.Count -gt 0) { [void]$findings.Add([pscustomobject]@{ File = $rel; Rule = "R4"; What = "an e-mail address ($($mails.Count) time(s), first at byte offset $($mails[0]))" }) }
		$tokens = New-Object System.Collections.ArrayList
		Find-Tokens $text $tokens
		if ($tokens.Count -gt 0) { [void]$findings.Add([pscustomobject]@{ File = $rel; Rule = "R5"; What = "an access token or private key ($($tokens.Count) time(s), first at byte offset $($tokens[0]))" }) }
	}
	return $result
}

# ---------------------------------------------------------------------------------------------------------------------------------------------------------------
if ($SelfTest)
{
	# Everything below uses MADE-UP names; the one test with real values writes them into a temporary file only
	$fakeUser = "zzfakeuser_leak"; $fakePc = "ZZFAKEPC01"; $fakeRoot = "Q:\FakeBuilds\zzproject root"; $fakeMail = "zz.fake.person@zzmailhost.net"
	$fakeNeedles = Get-NeedleSet @($fakeUser) @($fakePc) @($fakeRoot, "Q:\FakeBuilds\emsdk-zz") @($fakeMail, "someone@users.noreply.github.com")
	$tmp = Join-Path ([IO.Path]::GetTempPath()) ("leakguard-selftest-" + [Guid]::NewGuid().ToString("N"))
	New-Item -ItemType Directory -Force $tmp | Out-Null
	$rng = New-Object Random 7
	function New-Random([int]$n) { $b = New-Object byte[] $n; $rng.NextBytes($b); return ,$b }
	function Join-Bytes([byte[]]$a, [byte[]]$mid, [byte[]]$z) { $r = New-Object byte[] ($a.Length + $mid.Length + $z.Length); [Array]::Copy($a, 0, $r, 0, $a.Length); [Array]::Copy($mid, 0, $r, $a.Length, $mid.Length); [Array]::Copy($z, 0, $r, $a.Length + $mid.Length, $z.Length); return ,$r }
	$wasm = New-Random 40000; $data = New-Random 60000
	# a clean tree that looks like the real output: a page, a script with ordinary at signs and URLs, a binary, a data package, a picture in a folder
	function New-CleanTree([string]$path)
	{
		New-Item -ItemType Directory -Force (Join-Path $path "img") | Out-Null
		[IO.File]::WriteAllText((Join-Path $path "index.html"), "<html><style>@keyframes r{from{opacity:0}}</style><a href='https://example.org/x'>mail someone@users.noreply.github.com or a@example.com, C:\Program Files is just text</a></html>")
		[IO.File]::WriteAllText((Join-Path $path "licence.txt"), "Copyright (C) 2003 Maxim Stepin ( maxst@hiend3d.com ) and Hyllian - SERGIOGDB@gmail.com")
		[IO.File]::WriteAllText((Join-Path $path "app.js"), "/** @type {IDBDatabase} */ var u='https://github.com/ocornut/imgui'; var v='pkg@1.2.3';")
		[IO.File]::WriteAllBytes((Join-Path $path "app.wasm"), $wasm)
		[IO.File]::WriteAllBytes((Join-Path $path "app.data"), $data)
		[IO.File]::WriteAllBytes((Join-Path $path "img\icon.png"), (New-Random 3000))
	}
	function Get-Bytes([string]$s, [string]$enc) { if ($enc -eq "utf16") { [Text.Encoding]::Unicode.GetBytes($s) } elseif ($enc -eq "utf32") { [Text.Encoding]::UTF32.GetBytes($s) } else { [Text.Encoding]::UTF8.GetBytes($s) } }
	$ok = $true
	function Check([string]$name, [bool]$good, [string]$detail) { if (-not $good) { Write-Host "FAIL web_leak_guard self test: $name $detail" -ForegroundColor Red; $script:ok = $false } }
	try
	{
		# clean tree passes, with no finding at all
		$cleanDir = Join-Path $tmp "clean"; New-CleanTree $cleanDir
		$clean = Invoke-LeakScan $cleanDir $fakeNeedles
		Check "clean tree" ($clean.Findings.Count -eq 0 -and $clean.Files -eq 6) ("(findings: " + (($clean.Findings | ForEach-Object { $_.File + ":" + $_.Rule + " " + $_.What }) -join "; ") + ", files read: " + $clean.Files + ")")
		# the same tree must also be clean under the real patterns of this machine (the real output is built here)
		$cleanReal = Invoke-LeakScan $cleanDir (Get-RealNeedles @())
		Check "clean tree, real patterns" ($cleanReal.Findings.Count -eq 0) ("(findings: " + (($cleanReal.Findings | ForEach-Object { $_.File + ":" + $_.Rule }) -join "; ") + ")")
		# one leak per fixture; the expected rule must be among the findings of that fixture
		$cases = @(
			@("R1", "user name in a script (text)", { param($d) [IO.File]::AppendAllText((Join-Path $d "app.js"), "// built by $fakeUser") }),
			@("R1", "user name in different capitals", { param($d) [IO.File]::AppendAllText((Join-Path $d "app.js"), "// built by " + $fakeUser.ToUpper()) }),
			@("R1", "user name inside the wasm", { param($d) [IO.File]::WriteAllBytes((Join-Path $d "app.wasm"), (Join-Bytes $wasm[0..9999] (Get-Bytes $fakeUser "utf8") $wasm[10000..39999])) }),
			@("R1", "user name as UTF-16 in the data package", { param($d) [IO.File]::WriteAllBytes((Join-Path $d "app.data"), (Join-Bytes $data[0..999] (Get-Bytes $fakeUser "utf16") $data[1000..59999])) }),
			@("R1", "user name as UTF-32 in the wasm", { param($d) [IO.File]::WriteAllBytes((Join-Path $d "app.wasm"), (Join-Bytes $wasm[0..99] (Get-Bytes $fakeUser "utf32") $wasm[100..39999])) }),
			@("R1", "computer name in a picture", { param($d) [IO.File]::WriteAllBytes((Join-Path $d "img\icon.png"), (Join-Bytes (New-Random 100) (Get-Bytes $fakePc "utf8") (New-Random 100))) }),
			@("R2", "workspace folder with backslashes", { param($d) [IO.File]::AppendAllText((Join-Path $d "app.js"), "var p='$fakeRoot\src\main.cpp';") }),
			@("R2", "workspace folder with slashes", { param($d) [IO.File]::AppendAllText((Join-Path $d "app.js"), "var p='" + $fakeRoot.Replace('\', '/') + "/src/main.cpp';") }),
			@("R2", "workspace folder as JSON (doubled backslashes)", { param($d) [IO.File]::AppendAllText((Join-Path $d "app.js"), "{""p"":""" + $fakeRoot.Replace('\', '\\') + "\\x""}") }),
			@("R2", "workspace folder as an URL (%20)", { param($d) [IO.File]::AppendAllText((Join-Path $d "app.js"), "file:///" + $fakeRoot.Replace('\', '/').Replace(' ', '%20') + "/x") }),
			@("R2", "extra folder (the emsdk) in the loader", { param($d) [IO.File]::AppendAllText((Join-Path $d "app.js"), "var e='Q:/FakeBuilds/emsdk-zz/upstream';") }),
			@("R2", "workspace folder as UTF-16 in the wasm", { param($d) [IO.File]::WriteAllBytes((Join-Path $d "app.wasm"), (Join-Bytes $wasm[0..99] (Get-Bytes $fakeRoot "utf16") $wasm[100..39999])) }),
			@("R3", "a user folder of somebody else (backslash)", { param($d) [IO.File]::AppendAllText((Join-Path $d "app.js"), "var p='E:\Users\someone_else\Desktop\x';") }),
			@("R3", "a user folder of somebody else (slash)", { param($d) [IO.File]::AppendAllText((Join-Path $d "app.js"), "var p='d:/users/someone_else/Desktop/x';") }),
			@("R3", "a user folder of somebody else (JSON)", { param($d) [IO.File]::AppendAllText((Join-Path $d "app.js"), "{""p"":""C:\\Users\\someone_else""}") }),
			@("R3", "a user folder inside the wasm", { param($d) [IO.File]::WriteAllBytes((Join-Path $d "app.wasm"), (Join-Bytes $wasm[0..99] (Get-Bytes "C:/Users/x/src.cpp" "utf8") $wasm[100..39999])) }),
			@("R4", "the builder's e-mail address", { param($d) [IO.File]::AppendAllText((Join-Path $d "index.html"), "<p>$fakeMail</p>") }),
			@("R4", "the local part of the builder's e-mail address alone", { param($d) [IO.File]::AppendAllText((Join-Path $d "app.js"), "// zz.fake.person") }),
			@("R4", "the domain of the builder's e-mail address alone", { param($d) [IO.File]::AppendAllText((Join-Path $d "app.js"), "// see zzmailhost.net") }),
			@("R4", "an e-mail address of somebody else", { param($d) [IO.File]::AppendAllText((Join-Path $d "index.html"), "<p>jane.doe@mailhost.org</p>") }),
			@("R4", "an e-mail address inside the data package", { param($d) [IO.File]::WriteAllBytes((Join-Path $d "app.data"), (Join-Bytes $data[0..999] (Get-Bytes " john_smith@some-mail.co.uk " "utf8") $data[1000..59999])) }),
			@("R5", "a GitHub token", { param($d) [IO.File]::AppendAllText((Join-Path $d "app.js"), "var t='ghp_" + ("a1B2c3D4e5" * 4) + "';") }),
			@("R5", "a fine-grained GitHub token", { param($d) [IO.File]::AppendAllText((Join-Path $d "app.js"), "var t='github_pat_" + ("a1B2c3D4e5_" * 3) + "';") }),
			@("R5", "an AWS key id", { param($d) [IO.File]::AppendAllText((Join-Path $d "app.js"), "var t='AKIAABCDEFGHIJKLMNOP';") }),
			@("R5", "a private key", { param($d) [IO.File]::AppendAllText((Join-Path $d "app.js"), "-----BEGIN RSA PRIVATE KEY-----`nMIIE") }),
			@("R6", "a file of 4 MiB under a harmless name", { param($d) [IO.File]::WriteAllBytes((Join-Path $d "img\blob.dat"), (New-Object byte[] 4194304)) }),
			@("R6", "a file of 2 MiB under a harmless name", { param($d) [IO.File]::WriteAllBytes((Join-Path $d "img\half.dat"), (New-Object byte[] 2097152)) }),
			@("R6", "a Mega Drive header in a file of another size", { param($d) $b = New-Object byte[] 5000; [Array]::Copy([Text.Encoding]::ASCII.GetBytes("SEGA MEGA DRIVE "), 0, $b, 0x100, 16); [IO.File]::WriteAllBytes((Join-Path $d "img\x.dat"), $b) }),
			@("R6", "a Mega Drive header behind a copier header", { param($d) $b = New-Object byte[] 5000; [Array]::Copy([Text.Encoding]::ASCII.GetBytes("SEGA GENESIS    "), 0, $b, 0x300, 16); [IO.File]::WriteAllBytes((Join-Path $d "img\y.dat"), $b) }),
			@("R7", "a ROM by its name", { param($d) [IO.File]::WriteAllText((Join-Path $d "Sonic_Knuckles_wSonic3.bin"), "x") }),
			@("R7", "a save folder", { param($d) New-Item -ItemType Directory -Force (Join-Path $d "savedata") | Out-Null; [IO.File]::WriteAllText((Join-Path $d "savedata\x.txt"), "x") }),
			@("R7", "an empty storage folder", { param($d) New-Item -ItemType Directory -Force (Join-Path $d "img\Storage") | Out-Null }),
			@("R7", "a settings file", { param($d) [IO.File]::WriteAllText((Join-Path $d "settings.json"), "{}") }),
			@("R7", "a save file", { param($d) [IO.File]::WriteAllText((Join-Path $d "game.srm"), "x") }),
			@("R7", "a ROM extension that is also a Markdown file", { param($d) [IO.File]::WriteAllText((Join-Path $d "README.md"), "x") }),
			@("R7", "a git folder", { param($d) New-Item -ItemType Directory -Force (Join-Path $d ".git") | Out-Null; [IO.File]::WriteAllText((Join-Path $d ".git\config"), "x") })
		)
		$caught = 0
		foreach ($c in $cases)
		{
			$d = Join-Path $tmp ("case" + $caught + "_" + $c[0]); New-CleanTree $d
			& $c[2] $d
			$r = Invoke-LeakScan $d $fakeNeedles
			$rules = @($r.Findings | ForEach-Object { $_.Rule } | Select-Object -Unique)
			if ($rules -contains $c[0]) { $caught++ } else { Check ("leak '" + $c[1] + "'") $false ("was not caught by rule " + $c[0] + " (findings: " + ($rules -join ",") + ")") }
			Remove-Item -Recurse -Force $d
		}
		# the file is named relative to the scanned folder, whatever way the folder is written (slashes, 8.3 short name of the profile)
		$d = Join-Path $tmp "names"; New-CleanTree $d; [IO.File]::WriteAllBytes((Join-Path $d "img\icon.png"), (Join-Bytes (New-Random 50) (Get-Bytes $fakeUser "utf8") (New-Random 50)))
		$named = @((Invoke-LeakScan ($d.Replace('\', '/')) $fakeNeedles).Findings | ForEach-Object { $_.File })
		Check "findings name the file relative to the folder" ($named.Count -eq 1 -and $named[0] -ceq "img/icon.png") ("(got: " + ($named -join ", ") + ")")
		# nothing to scan
		$emptyDir = Join-Path $tmp "empty"; New-Item -ItemType Directory -Force $emptyDir | Out-Null
		Check "an empty folder fails" (@((Invoke-LeakScan $emptyDir $fakeNeedles).Findings | Where-Object { $_.Rule -eq "R8" }).Count -eq 1) ""
		Check "a missing folder fails" (@((Invoke-LeakScan (Join-Path $tmp "nope") $fakeNeedles).Findings | Where-Object { $_.Rule -eq "R8" }).Count -eq 1) ""
		# a value that is too short must not make the guard useless or noisy: two letters are not looked for as text (the C:\Users rule still covers the path)
		$shortSet = Get-NeedleSet @("ab") @() @() @()
		Check "a user name of two letters is not a pattern" (@($shortSet | Where-Object { $_.Rule -eq "R1" }).Count -eq 0) ""
		Check "the public GitHub noreply address is not a pattern" (@((Get-NeedleSet @() @() @() @("123+someone@users.noreply.github.com")) | Where-Object { $_.Rule -eq "R4" }).Count -eq 0) ""
		# the real patterns come from the environment of the machine, and they catch the real user name and the real workspace path
		$real = Get-RealNeedles @()
		$realUser = [Environment]::UserName
		if ($realUser.Length -ge 4)
		{
			Check "real patterns contain the user name of this machine" (@($real | Where-Object { $_.Rule -eq "R1" -and $_.Value -eq $realUser }).Count -ge 1) ""
			$d = Join-Path $tmp "realuser"; New-CleanTree $d; [IO.File]::AppendAllText((Join-Path $d "app.js"), "// x " + $realUser + " y")
			Check "the real user name is caught" (@((Invoke-LeakScan $d $real).Findings | Where-Object { $_.Rule -eq "R1" }).Count -ge 1) ""
		}
		Check "real patterns contain the workspace folder" (@($real | Where-Object { $_.Rule -eq "R2" -and $_.Value -eq $root }).Count -ge 1) ""
		$d = Join-Path $tmp "realroot"; New-CleanTree $d; [IO.File]::AppendAllText((Join-Path $d "app.js"), "var p='" + $root.Replace('\', '/') + "/x';")
		Check "the real workspace folder is caught" (@((Invoke-LeakScan $d $real).Findings | Where-Object { $_.Rule -eq "R2" }).Count -ge 1) ""
		Check "every one of the leak fixtures was caught" ($caught -eq $cases.Count) ("($caught of " + $cases.Count + ")")
	}
	finally { Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue }
	if ($ok) { Write-Host ("PASS web_leak_guard self test (" + $cases.Count + " leaking fixtures caught by the rule they belong to, the clean tree passes, an empty or missing folder fails, the patterns come from the environment)"); exit 0 }
	Write-Host "FAIL web_leak_guard self test" -ForegroundColor Red; exit 1
}

if (-not $Dir) { Write-Host "usage: web_leak_guard.ps1 -Dir <folder> [-ExtraFolder <folder>,...] | -SelfTest" -ForegroundColor Red; exit 2 }
$needles = Get-RealNeedles $ExtraFolder
$res = Invoke-LeakScan $Dir $needles
if ($res.Findings.Count -gt 0)
{
	foreach ($f in $res.Findings) { Write-Host ("guard FAILED: " + $f.File + ": " + $f.Rule + " " + $ruleText[$f.Rule] + ": " + $f.What) -ForegroundColor Red }
	Write-Host ("guard FAILED: " + $res.Findings.Count + " finding(s) in " + $Dir + " (" + $res.Files + " files read); nothing may be published from this folder") -ForegroundColor Red
	exit 1
}
Write-Host ("guard passed: " + $Dir + ": " + $res.Files + " files, " + $res.Bytes + " bytes read in full (text and binary, .wasm and .data included); no Windows user name or computer name, no builder folder, no <drive>:\Users path, no e-mail address (but the four licence-header addresses of the upscaler shaders), no token or private key, no ROM-sized or ROM-looking file, no save data / storage / ROM names (" + @($needles).Count + " patterns from this machine's environment, each also as UTF-16 and UTF-32)")
exit 0
