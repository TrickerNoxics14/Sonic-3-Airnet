# Prepares the GitHub Pages branch from the finished web build, and publishes it ONLY with -Push.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File checks\publish_pages.ps1            DRY RUN (the default): builds the branch in a temporary repository, checks it, pushes NOTHING
#   powershell -NoProfile -ExecutionPolicy Bypass -File checks\publish_pages.ps1 -Push      the real thing: a FORCE-push of gh-pages (the old history is replaced). Needs the owner's explicit yes.
#   powershell -NoProfile -ExecutionPolicy Bypass -File checks\publish_pages.ps1 -SelfTest  tests of the refusal rules and of the dry run with made-up fixtures (never pushes)
#
# What it does:
#   1. reads web_build (the output of checks\build_web.ps1) and runs checks\web_leak_guard.ps1 on it (user name, folders, e-mail, token, ROM, save data in ANY file, text or binary)
#   2. makes a temporary repository with ONE orphan commit on the branch gh-pages: nothing of the old gh-pages history is carried over. The commit is made by the GitHub noreply identity
#      of the owner with a Co-Authored-By line; line endings are never converted (the files are committed byte for byte)
#   3. checks that every committed file is byte-identical to web_build (and that nothing else is in the commit but .nojekyll), exports the committed tree and runs the guard AGAIN on exactly that tree
#   4. prints the file list with sizes and what a real push would do
#   -Push refuses (exit 1, nothing pushed) when: the guard fails, the committed tree differs from web_build, the current branch is not browser-multiplayer or main, the working tree is not clean,
#   web_build was not made from the current source tree (the note web_build.stamp that build_web.ps1 writes next to the folder is missing, dirty, or from another source tree), the commit is not
#   a single commit by the noreply identity, or origin is not a plain https://github.com/<owner>/<repo> address. The dry run reports the same reasons ("a real push would be refused: ...").
param([switch]$Push, [string]$SourceDir, [switch]$KeepTemp, [switch]$SelfTest)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$allowedBranches = @("browser-multiplayer", "main")
$identityName = "TrickerNoxics14"
$identityMail = "96994462+TrickerNoxics14@users.noreply.github.com"		# the public GitHub noreply address of the owner (it is in every commit of the repository)
$coAuthor = "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
$remoteRegex = '^https://github\.com/[A-Za-z0-9._-]+/[A-Za-z0-9._-]+?(\.git)?$'
$guardScript = Join-Path $PSScriptRoot "web_leak_guard.ps1"

# Runs git, returns the output lines, throws when git fails (unless -AllowFail). Git writes progress to stderr: that is not an error here.
function Invoke-Git([string]$Repo, [string[]]$GitArgs, [switch]$AllowFail)
{
	$old = $ErrorActionPreference; $ErrorActionPreference = "Continue"
	try { $out = @(& git -C $Repo @GitArgs 2>&1 | ForEach-Object { "$_" }); $code = $LASTEXITCODE } finally { $ErrorActionPreference = $old }
	if ($code -ne 0 -and -not $AllowFail) { throw ("git " + ($GitArgs -join " ") + " failed (" + $code + "): " + ($out -join " | ")) }
	return $out
}

# The reasons a real push has to be refused; empty = allowed. A pure function of the state (the self test calls it with made-up states).
function Get-PushBlockers($State)
{
	$b = New-Object System.Collections.ArrayList
	if ($allowedBranches -notcontains $State.Branch) { [void]$b.Add("the current branch is '" + $State.Branch + "', not " + ($allowedBranches -join " or ")) }
	if ($State.PorcelainCount -gt 0) { [void]$b.Add("the working tree is not clean (" + $State.PorcelainCount + " uncommitted or untracked change(s))") }
	if (-not $State.DefaultSource) { [void]$b.Add("-SourceDir is not the default web_build folder (another folder is for tests only)") }
	if ($State.StampProblem) { [void]$b.Add("web_build is not known to be built from this source tree: " + $State.StampProblem) }
	if (-not $State.GuardOk) { [void]$b.Add("the leak guard failed") }
	if (-not $State.TreeMatches) { [void]$b.Add("the committed tree differs from web_build") }
	if (-not $State.SingleCommitByOwner) { [void]$b.Add("the prepared branch is not one commit by the noreply identity with the Co-Authored-By line") }
	if (-not $State.RemoteOk) { [void]$b.Add("origin is missing or is not a plain https://github.com/<owner>/<repo> address") }
	return $b
}

# web_build.stamp (next to the folder, written by build_web.ps1): the source tree the page was built from. Returns a problem text, or $null when it fits the current source tree.
function Get-StampProblem([string]$Stamp)
{
	if (-not (Test-Path -LiteralPath $Stamp)) { return "there is no note '" + (Split-Path -Leaf $Stamp) + "' next to it (build it again with checks\build_web.ps1)" }
	$kv = @{}
	foreach ($line in (Get-Content -LiteralPath $Stamp)) { if ($line -match '^([A-Za-z]+)=(.*)$') { $kv[$Matches[1]] = $Matches[2].Trim() } }
	if ($kv["guard"] -ne "passed") { return "the note says the leak guard did not pass" }
	if ($kv["dirty"] -ne "0") { return "it was built while the source tree had uncommitted changes" }
	$now = @(Invoke-Git $root @("rev-parse", "HEAD:sonic3air_online_stable_source") -AllowFail)
	if ($now.Count -ne 1 -or $now[0] -notmatch '^[0-9a-f]{40}$') { return "the current source tree cannot be read" }
	if ($kv["sourceTree"] -ne $now[0]) { return "the source tree changed since it was built (build it again with checks\build_web.ps1)" }
	return $null
}

# Runs the guard on a folder; returns @{ Ok; Lines }
function Invoke-Guard([string]$Folder)
{
	$extra = @(); if ($env:EMSDK -and ($env:EMSDK -notmatch '^/')) { $extra = @("-ExtraFolder", $env:EMSDK) }
	$old = $ErrorActionPreference; $ErrorActionPreference = "Continue"
	try { $lines = @(& powershell -NoProfile -ExecutionPolicy Bypass -File $guardScript -Dir $Folder @extra 2>&1 | ForEach-Object { "$_" }); $code = $LASTEXITCODE } finally { $ErrorActionPreference = $old }
	return [pscustomobject]@{ Ok = ($code -eq 0 -and ($lines | Where-Object { $_ -like "guard passed:*" }).Count -eq 1); Lines = $lines }
}

# Every file of a folder: relative path (with slashes) -> full path
function Get-FileMap([string]$Folder)
{
	$map = @{}
	$stack = New-Object System.Collections.Stack
	$stack.Push(@((Resolve-Path -LiteralPath $Folder).ProviderPath, ""))
	while ($stack.Count -gt 0)
	{
		$e = $stack.Pop()
		foreach ($c in @(Get-ChildItem -LiteralPath $e[0] -Force))
		{
			$rel = $(if ($e[1]) { $e[1] + "/" + $c.Name } else { $c.Name })
			if ($c.PSIsContainer) { $stack.Push(@($c.FullName, $rel)) } else { $map[$rel] = $c.FullName }
		}
	}
	return $map
}

# The whole preparation. Returns @{ Exit; Blockers; ... }; with -Push it pushes at the very end, and only when nothing blocks.
function Invoke-Publish([string]$Source, [bool]$DefaultSource, [bool]$DoPush, [bool]$Keep)
{
	$state = @{ Branch = ""; PorcelainCount = 0; DefaultSource = $DefaultSource; StampProblem = $null; GuardOk = $false; TreeMatches = $false; SingleCommitByOwner = $false; RemoteOk = $false }
	$state.Branch = (@(Invoke-Git $root @("rev-parse", "--abbrev-ref", "HEAD") -AllowFail)[0])
	$state.PorcelainCount = @(Invoke-Git $root @("status", "--porcelain") -AllowFail | Where-Object { $_ }).Count
	$headShort = (@(Invoke-Git $root @("rev-parse", "--short", "HEAD") -AllowFail)[0])
	$state.StampProblem = Get-StampProblem ($Source + ".stamp")
	$remote = (@(Invoke-Git $root @("remote", "get-url", "origin") -AllowFail)[0])
	$state.RemoteOk = [bool]($remote -match $remoteRegex)
	Write-Host ("publish_pages: " + $(if ($DoPush) { "REAL PUSH requested (-Push)" } else { "DRY RUN (nothing will be pushed)" }) + "; branch " + $state.Branch + ", " + $state.PorcelainCount + " uncommitted change(s)")
	if ($DoPush)
	{
		$early = @(Get-PushBlockers $state | Where-Object { $_ -notmatch '^(the leak guard failed|the committed tree differs|the prepared branch)' })
		if ($early.Count -gt 0) { $early | ForEach-Object { Write-Host ("REFUSED: " + $_) -ForegroundColor Red }; Write-Host "publish_pages: nothing was pushed." -ForegroundColor Red; return 1 }
	}
	if (-not (Test-Path -LiteralPath (Join-Path $Source "index.html"))) { Write-Host ("REFUSED: " + $Source + " is not a web build (no index.html); build it with checks\build_web.ps1") -ForegroundColor Red; return 1 }

	# 1. the guard on the finished build
	$g1 = Invoke-Guard $Source
	$g1.Lines | ForEach-Object { Write-Host $_ }
	if (-not $g1.Ok) { Write-Host "publish_pages: the leak guard blocks this folder; nothing was prepared and nothing was pushed." -ForegroundColor Red; return 1 }

	$tmp = Join-Path ([IO.Path]::GetTempPath()) ("s3air-pages-" + [Guid]::NewGuid().ToString("N"))
	$export = $tmp + "-export"
	$oldEnv = @{}; foreach ($n in @("GIT_AUTHOR_NAME", "GIT_AUTHOR_EMAIL", "GIT_COMMITTER_NAME", "GIT_COMMITTER_EMAIL")) { $oldEnv[$n] = [Environment]::GetEnvironmentVariable($n) }
	try
	{
		# 2. a fresh repository with one orphan commit
		New-Item -ItemType Directory -Force $tmp | Out-Null
		[void](Invoke-Git $tmp @("init", "-q", "."))
		[void](Invoke-Git $tmp @("symbolic-ref", "HEAD", "refs/heads/gh-pages"))
		foreach ($kv in @(@("core.autocrlf", "false"), @("core.safecrlf", "false"), @("core.quotepath", "false"), @("commit.gpgsign", "false"), @("user.name", $identityName), @("user.email", $identityMail))) { [void](Invoke-Git $tmp @("config", $kv[0], $kv[1])) }
		[IO.File]::WriteAllText((Join-Path $tmp ".git\info\attributes"), "* -text`n")		# bytes in, the same bytes out
		& robocopy $Source $tmp /E /NFL /NDL /NJH /NJS /NP | Out-Null
		if ($LASTEXITCODE -ge 8) { throw "robocopy failed ($LASTEXITCODE)" }
		if (-not (Test-Path -LiteralPath (Join-Path $tmp ".nojekyll"))) { [IO.File]::WriteAllBytes((Join-Path $tmp ".nojekyll"), (New-Object byte[] 0)) }		# GitHub Pages: no Jekyll processing
		[void](Invoke-Git $tmp @("add", "-A"))
		$srcFiles = Get-FileMap $Source
		$bytesTotal = [int64]0; foreach ($p in $srcFiles.Values) { $bytesTotal += (Get-Item -LiteralPath $p).Length }
		$msg = "Publish the web build of Sonic 3 AirNet`n`nOne commit made by checks/publish_pages.ps1 from the finished web build (" + $srcFiles.Count + " files, " + $bytesTotal + " bytes) of the source at " + $headShort + ". The history of this branch is replaced on purpose: nothing of the older history is kept.`n`n" + $coAuthor + "`n"
		$msgFile = $tmp + "-message.txt"
		[IO.File]::WriteAllText($msgFile, $msg)
		$env:GIT_AUTHOR_NAME = $identityName; $env:GIT_COMMITTER_NAME = $identityName; $env:GIT_AUTHOR_EMAIL = $identityMail; $env:GIT_COMMITTER_EMAIL = $identityMail
		[void](Invoke-Git $tmp @("commit", "-q", "-F", $msgFile))
		foreach ($n in $oldEnv.Keys) { [Environment]::SetEnvironmentVariable($n, $oldEnv[$n]) }
		$commit = (@(Invoke-Git $tmp @("rev-parse", "HEAD"))[0])

		# 3. what is in the commit is what is in web_build, and the commit is what it should be
		$state.SingleCommitByOwner = ((@(Invoke-Git $tmp @("rev-list", "--count", "HEAD"))[0]) -eq "1") `
			-and ((@(Invoke-Git $tmp @("log", "-1", "--format=%an|%ae|%cn|%ce"))[0]) -ceq ($identityName + "|" + $identityMail + "|" + $identityName + "|" + $identityMail)) `
			-and ((@(Invoke-Git $tmp @("log", "-1", "--format=%B")) -join "`n").TrimEnd() -like ("*" + $coAuthor))
		$entries = @{}
		foreach ($line in (Invoke-Git $tmp @("ls-tree", "-r", "-l", "HEAD")))
		{
			if ($line -match '^\d+ blob ([0-9a-f]{40})\s+(\d+)\t(.+)$') { $entries[$Matches[3]] = @{ Id = $Matches[1]; Size = [int64]$Matches[2] } } else { throw "unexpected line from git ls-tree: $line" }
		}
		$problems = New-Object System.Collections.ArrayList
		foreach ($rel in $srcFiles.Keys) { if (-not $entries.ContainsKey($rel)) { [void]$problems.Add("missing in the commit: $rel") } else { $id = (@(Invoke-Git $tmp @("hash-object", "--no-filters", "--", $srcFiles[$rel]))[0]); if ($id -ne $entries[$rel].Id) { [void]$problems.Add("differs from web_build: $rel") } } }
		foreach ($rel in $entries.Keys) { if (-not $srcFiles.ContainsKey($rel) -and $rel -ne ".nojekyll") { [void]$problems.Add("not in web_build: $rel") } }
		if (@(Invoke-Git $tmp @("status", "--porcelain") | Where-Object { $_ }).Count -gt 0) { [void]$problems.Add("the working tree of the temporary repository is not the commit") }
		$state.TreeMatches = ($problems.Count -eq 0)
		$problems | ForEach-Object { Write-Host ("TREE: " + $_) -ForegroundColor Red }

		# the guard again, on exactly the tree that would go out (exported from the commit, not from the folders it was made from)
		New-Item -ItemType Directory -Force $export | Out-Null
		[void](Invoke-Git $tmp @("checkout-index", "-a", "-f", ("--prefix=" + $export.Replace('\', '/') + "/")))
		$g2 = Invoke-Guard $export
		$g2.Lines | ForEach-Object { Write-Host ($_ -replace [regex]::Escape($export), "<the exported commit>") }
		$state.GuardOk = $g1.Ok -and $g2.Ok

		# 4. the list
		Write-Host ""
		Write-Host ("branch gh-pages, ONE commit " + $commit.Substring(0, 12) + " (author and committer " + $identityName + " <" + $identityMail + ">), " + $entries.Count + " files:")
		$total = [int64]0
		foreach ($rel in ($entries.Keys | Sort-Object)) { Write-Host ("  {0,12:N0}  {1}" -f $entries[$rel].Size, $rel); $total += $entries[$rel].Size }
		Write-Host ("  {0,12:N0}  in total" -f $total)
		# what the published page will use as relay (the one thing in the folder the owner has to decide on)
		$relayFile = Join-Path $Source "relay_config.js"
		if (Test-Path -LiteralPath $relayFile)
		{
			$relayLine = @(Get-Content -LiteralPath $relayFile | Where-Object { $_ -match '^\s*window\.S3AIR_RELAY_DEFAULT\s*=' })
			$relayValue = $(if ($relayLine.Count -eq 1 -and $relayLine[0] -match "=\s*'([^']*)'") { $Matches[1] } else { $null })
			if ($null -eq $relayValue) { Write-Host "relay address in relay_config.js: not readable (look at the file)" -ForegroundColor Yellow }
			elseif ($relayValue -eq "") { Write-Host "relay address in relay_config.js: EMPTY (the page plays offline; online play is off until a player enters a relay address)" }
			elseif ($relayValue -notmatch '^wss://') { Write-Host ("relay address in relay_config.js: " + $relayValue + "  (a page on https only accepts wss:// addresses)") -ForegroundColor Yellow }
			else { Write-Host ("relay address in relay_config.js: " + $relayValue) }
		}
		Write-Host ""

		$blockers = @(Get-PushBlockers $state)
		if ($blockers.Count -gt 0)
		{
			$blockers | ForEach-Object { Write-Host ($(if ($DoPush) { "REFUSED: " } else { "a real push would be refused: " }) + $_) -ForegroundColor $(if ($DoPush) { "Red" } else { "Yellow" }) }
			if ($DoPush) { Write-Host "publish_pages: nothing was pushed." -ForegroundColor Red; return 1 }
			Write-Host "publish_pages: DRY RUN done. Nothing was pushed (and a real push would be refused for the reason(s) above)."
			return 0
		}
		if (-not $DoPush)
		{
			Write-Host "publish_pages: DRY RUN done. Nothing was pushed, no branch or tag of the repository was touched. A real push would be: git push --force origin gh-pages:gh-pages (replaces the whole history of gh-pages by this one commit)."
			Write-Host "To publish, after the owner's explicit yes: powershell -NoProfile -ExecutionPolicy Bypass -File checks\publish_pages.ps1 -Push"
			return 0
		}
		# the real push: the commit that was checked is the commit that goes out
		[void](Invoke-Git $tmp @("remote", "add", "origin", $remote))
		Write-Host ("publish_pages: pushing " + $commit.Substring(0, 12) + " to the branch gh-pages of " + $remote + " (force, replaces the old history) ...")
		[void](Invoke-Git $tmp @("push", "--force", "origin", "gh-pages:refs/heads/gh-pages"))
		$remoteHead = (@(Invoke-Git $tmp @("ls-remote", "origin", "refs/heads/gh-pages"))[0]) -split '\s+' | Select-Object -First 1
		if ($remoteHead -ne $commit) { Write-Host ("publish_pages: the remote gh-pages is " + $remoteHead + ", expected " + $commit) -ForegroundColor Red; return 1 }
		Write-Host ("publish_pages: PUSHED. The remote gh-pages is now " + $commit.Substring(0, 12) + " (one commit). The page needs a minute to update; then open it and check it (see WEB_BUILD.md).")
		return 0
	}
	finally
	{
		foreach ($n in $oldEnv.Keys) { [Environment]::SetEnvironmentVariable($n, $oldEnv[$n]) }
		if (-not $Keep) { foreach ($p in @($tmp, $export, ($tmp + "-message.txt"))) { if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Recurse -Force -ErrorAction SilentlyContinue } } }
		else { Write-Host ("publish_pages: the temporary repository was kept: " + $tmp) }
	}
}

# ---------------------------------------------------------------------------------------------------------------------------------------------------------------
if ($SelfTest)
{
	$ok = $true
	function Check([string]$name, [bool]$good, [string]$detail) { if (-not $good) { Write-Host "FAIL publish_pages self test: $name $detail" -ForegroundColor Red; $script:ok = $false } }
	# the refusal rules: one wrong thing at a time, and the all-good state
	$good = @{ Branch = "browser-multiplayer"; PorcelainCount = 0; DefaultSource = $true; StampProblem = $null; GuardOk = $true; TreeMatches = $true; SingleCommitByOwner = $true; RemoteOk = $true }
	Check "the good state is allowed" ((@(Get-PushBlockers $good)).Count -eq 0) ""
	$onMain = $good.Clone(); $onMain["Branch"] = "main"
	Check "branch main is allowed" ((@(Get-PushBlockers $onMain)).Count -eq 0) ""
	$bad = @(@("Branch", "some-feature", "branch"), @("Branch", "HEAD", "branch"), @("PorcelainCount", 1, "not clean"), @("DefaultSource", $false, "-SourceDir"), @("StampProblem", "x", "web_build is not known"), @("GuardOk", $false, "leak guard"),
		@("TreeMatches", $false, "differs from web_build"), @("SingleCommitByOwner", $false, "not one commit"), @("RemoteOk", $false, "origin"))
	foreach ($case in $bad)
	{
		$s = $good.Clone(); $s[$case[0]] = $case[1]
		$r = @(Get-PushBlockers $s)
		Check ("a bad " + $case[0] + " is refused") ($r.Count -eq 1 -and $r[0] -like ("*" + $case[2] + "*")) ("(got: " + ($r -join "; ") + ")")
	}
	# the dry run with made-up fixtures, as a separate process (it never gets -Push)
	$tmp = Join-Path ([IO.Path]::GetTempPath()) ("pages-selftest-" + [Guid]::NewGuid().ToString("N"))
	try
	{
		$clean = Join-Path $tmp "clean_build"; New-Item -ItemType Directory -Force (Join-Path $clean "img") | Out-Null
		[IO.File]::WriteAllText((Join-Path $clean "index.html"), "<html>`r`n<p>a page with Windows line ends</p>`r`n</html>`r`n")
		[IO.File]::WriteAllText((Join-Path $clean "app.js"), "var a = 1;`nvar b = 2;`n")
		$rng = New-Object Random 3; $blob = New-Object byte[] 20000; $rng.NextBytes($blob); [IO.File]::WriteAllBytes((Join-Path $clean "app.data"), $blob); [IO.File]::WriteAllBytes((Join-Path $clean "img\icon.png"), $blob[0..999])
		$out = @(& powershell -NoProfile -ExecutionPolicy Bypass -File $PSCommandPath -SourceDir $clean 2>&1 | ForEach-Object { "$_" }); $code = $LASTEXITCODE
		Check "dry run of a clean fixture ends with 0" ($code -eq 0) ("(exit $code)")
		Check "dry run of a clean fixture says DRY RUN and pushes nothing" (($out | Where-Object { $_ -like "*DRY RUN done. Nothing was pushed*" }).Count -eq 1) ""
		Check "dry run lists the files with sizes and .nojekyll" (($out | Where-Object { $_ -match '^\s+[\d,\.]+\s+index\.html$' }).Count -eq 1 -and ($out | Where-Object { $_ -match '\s\.nojekyll$' }).Count -eq 1) ""
		Check "dry run of a fixture says that a real push would be refused (no stamp, custom folder)" (($out | Where-Object { $_ -like "a real push would be refused:*" }).Count -ge 2) ""
		Check "dry run runs the guard twice (the build folder and the exported commit)" (($out | Where-Object { $_ -like "guard passed:*" }).Count -eq 2) ""
		Check "for a good fixture the commit, the tree and the guard raise no objection (only the state of this checkout and the missing note)" (($out | Where-Object { $_ -like "a real push would be refused:*" -and $_ -match 'committed tree|prepared branch|leak guard' }).Count -eq 0 -and ($out | Where-Object { $_ -like "*one commit by the noreply identity*" }).Count -eq 0) ""
		Check "the dry run names the one commit and the identity" (($out | Where-Object { $_ -like "branch gh-pages, ONE commit *" -and $_ -like "*users.noreply.github.com*" }).Count -eq 1) ""
		# a file in the folder that git would not commit (a .gitignore inside the build): the commit would then lack files, and that must be seen
		$sneaky = Join-Path $tmp "sneaky_build"; Copy-Item $clean $sneaky -Recurse; [IO.File]::WriteAllText((Join-Path $sneaky ".gitignore"), "*.js`n")
		$out3 = @(& powershell -NoProfile -ExecutionPolicy Bypass -File $PSCommandPath -SourceDir $sneaky 2>&1 | ForEach-Object { "$_" })
		Check "a build folder that git would not commit completely is seen (a .gitignore inside it)" (($out3 | Where-Object { $_ -eq "TREE: missing in the commit: app.js" }).Count -eq 1 -and ($out3 | Where-Object { $_ -like "a real push would be refused: the committed tree differs from web_build" }).Count -eq 1) ("(" + (($out3 | Where-Object { $_ -like "TREE:*" }) -join "; ") + ")")
		Check "no CRLF was converted (the commit holds the bytes of the folder)" (($out | Where-Object { $_ -like "TREE:*" }).Count -eq 0) ""
		Check "the temporary repository is removed" (@(Get-ChildItem ([IO.Path]::GetTempPath()) -Filter "s3air-pages-*" -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTime -gt (Get-Date).AddMinutes(-3) }).Count -eq 0) ""
		# leaking fixtures are stopped by the guard before anything is prepared
		foreach ($leak in @(@("a user folder path", "C:\Users\someone_else\x"), @("a ROM-sized file", $null), @("a save folder", "savedata"), @("a token", ("ghp_" + ("a1B2c3D4e5" * 4))), @("an e-mail address", "jane.doe@mailhost.org")))
		{
			$d = Join-Path $tmp ("leak_" + [Guid]::NewGuid().ToString("N").Substring(0, 6)); Copy-Item $clean $d -Recurse
			if ($leak[0] -eq "a ROM-sized file") { [IO.File]::WriteAllBytes((Join-Path $d "img\x.dat"), (New-Object byte[] 4194304)) }
			elseif ($leak[0] -eq "a save folder") { New-Item -ItemType Directory -Force (Join-Path $d "savedata") | Out-Null; [IO.File]::WriteAllText((Join-Path $d "savedata\s.txt"), "x") }
			else { [IO.File]::AppendAllText((Join-Path $d "app.js"), "// " + $leak[1]) }
			$out2 = @(& powershell -NoProfile -ExecutionPolicy Bypass -File $PSCommandPath -SourceDir $d 2>&1 | ForEach-Object { "$_" }); $code2 = $LASTEXITCODE
			Check ("a fixture with " + $leak[0] + " is blocked") ($code2 -eq 1 -and ($out2 | Where-Object { $_ -like "guard FAILED:*" }).Count -ge 1 -and ($out2 | Where-Object { $_ -like "*nothing was prepared and nothing was pushed*" }).Count -eq 1 -and ($out2 | Where-Object { $_ -like "*DRY RUN done*" }).Count -eq 0) ("(exit $code2)")
		}
		# a folder that is no web build
		$empty = Join-Path $tmp "empty"; New-Item -ItemType Directory -Force $empty | Out-Null
		& powershell -NoProfile -ExecutionPolicy Bypass -File $PSCommandPath -SourceDir $empty *> $null
		Check "a folder without index.html is refused" ($LASTEXITCODE -eq 1) ""
	}
	finally { Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue }
	if ($ok) { Write-Host "PASS publish_pages self test (refusal rules one by one, dry run of a clean fixture, 5 leaking fixtures blocked by the guard, nothing pushed)"; exit 0 }
	Write-Host "FAIL publish_pages self test" -ForegroundColor Red; exit 1
}

$default = Join-Path $root "web_build"
$isDefault = (-not $SourceDir)
if (-not $SourceDir) { $SourceDir = $default }
if (-not (Test-Path -LiteralPath $SourceDir -PathType Container)) { Write-Host ("REFUSED: the folder " + $SourceDir + " does not exist; build it with checks\build_web.ps1") -ForegroundColor Red; exit 1 }
exit (Invoke-Publish (Resolve-Path -LiteralPath $SourceDir).ProviderPath $isDefault ([bool]$Push) ([bool]$KeepTemp))
