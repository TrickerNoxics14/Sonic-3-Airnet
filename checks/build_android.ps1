<#
  Clean Android build of Sonic 3 AirNet (installable, signed APK). See checks\package_files\ANDROID_BUILD.md.

  -ToolsDir   folder that holds the Android toolchain, OUTSIDE the repository (see ANDROID_BUILD.md for what goes in it):
                jdk\<a JDK 17 or 21>   sdk\ (Android SDK: platform 36, build-tools 36.0.0, NDK 29.0.14206865, cmake 3.22.1)
              The script also keeps the Gradle cache (gradle-home\), a short temp folder (tmp\) and the signing keystore (keystore\) there.
  -Version    x.y.z of the APK (default: the newest local git tag vX.Y.Z)
  -OutDir     result folder (default: android_build in the workspace root, untracked, contains NO ROM)
  -PackExe    a Windows build of this fork (Sonic3AIR.exe). Its "-pack" option creates the data packages, and one short start
              creates the compiled script file. Default: the Release_x64 build inside the source tree.
  -Abis       comma separated: arm64-v8a (phones from ~2017 on), armeabi-v7a (older 32 bit phones), x86_64 / x86 (emulators, some Chromebooks)
  -DebugApk      build a debug APK instead of the signed release APK (no keystore needed, but it cannot update a release install)

  Signing: the release APK is signed with the keystore in <ToolsDir>\keystore (created on the first run, never in the repository).
  Every later version MUST be signed with the same keystore, otherwise Android refuses to install it over the old one. Keep a backup of it.
#>
param(
	[Parameter(Mandatory = $true)][string]$ToolsDir,
	[string]$Version,
	[string]$OutDir,
	[string]$PackExe,
	[string]$Abis = "arm64-v8a,armeabi-v7a,x86_64,x86",
	[switch]$DebugApk
)
$ErrorActionPreference = "Stop"
$PSNativeCommandUseErrorActionPreference = $false
$root = Split-Path -Parent $PSScriptRoot
$oxy  = Join-Path $root "sonic3air_online_stable_source\Oxygen"
$proj = Join-Path $oxy "sonic3air\build\_android"
$assets = Join-Path $proj "app\src\main\assets"
if (-not $OutDir) { $OutDir = Join-Path $root "android_build" }
if (-not $PackExe) { $PackExe = Join-Path $oxy "sonic3air\bin\Release_x64\Sonic3AIR.exe" }
$work = Join-Path $root "android_work"
$ToolsDir = (Resolve-Path $ToolsDir).Path
$jdkDir = Get-ChildItem (Join-Path $ToolsDir "jdk") -Directory | Select-Object -First 1
$sdk = Join-Path $ToolsDir "sdk"
if (-not $jdkDir -or -not (Test-Path (Join-Path $jdkDir.FullName "bin\java.exe"))) { throw "No JDK in $ToolsDir\jdk (use JDK 17 or 21; Gradle 8.14 does not run on newer ones)" }
if (-not (Test-Path (Join-Path $sdk "ndk\29.0.14206865"))) { throw "NDK 29.0.14206865 not found in $sdk (sdkmanager `"ndk;29.0.14206865`")" }
if (-not (Test-Path $PackExe)) { throw "Windows build for packing not found: $PackExe (build the Windows game first, or pass -PackExe)" }
if (-not $Version)
{
	$tags = & git -C $root tag --list "v*.*.*" 2>$null | Where-Object { $_ -match '^v\d+\.\d+\.\d+$' } | Sort-Object { [version]($_.Substring(1)) }
	if (-not $tags) { throw "No version given and no local tag vX.Y.Z found: pass -Version x.y.z" }
	$Version = ($tags | Select-Object -Last 1).Substring(1)
}
if ($Version -notmatch '^(\d{1,3})\.(\d{1,2})\.(\d{1,2})$') { throw "Version must look like 1.2.3 (minor and patch at most two digits for the version code), got '$Version'" }
$versionCode = [int]$Matches[1] * 10000 + [int]$Matches[2] * 100 + [int]$Matches[3]

# Gradle needs a SHORT temp folder (the 8.3 names of a long Windows temp path make its internal unix sockets fail: "Unable to establish loopback connection")
$tmp = Join-Path $ToolsDir "tmp"
New-Item -ItemType Directory -Force $tmp | Out-Null
$env:JAVA_HOME = $jdkDir.FullName
$env:ANDROID_HOME = $sdk
$env:ANDROID_SDK_ROOT = $sdk
$env:GRADLE_USER_HOME = Join-Path $ToolsDir "gradle-home"
$env:TEMP = $tmp
$env:TMP = $tmp
$env:JAVA_TOOL_OPTIONS = $null

# Clean start: nothing from an earlier build is reused
foreach ($d in @($work, $OutDir, $assets, (Join-Path $proj "app\build"), (Join-Path $proj "app\.cxx"), (Join-Path $proj ".gradle"))) { if (Test-Path $d) { Remove-Item $d -Recurse -Force } }
New-Item -ItemType Directory -Force $work, $OutDir, $assets | Out-Null
$sw = [Diagnostics.Stopwatch]::StartNew()

# 1. data packages, made by the engine's own packer from a scratch copy (so the source tree stays untouched)
$pack = Join-Path $work "pack"
$air  = Join-Path $pack "sonic3air"
New-Item -ItemType Directory -Force (Join-Path $air "saves"), (Join-Path $pack "oxygenengine") | Out-Null
robocopy (Join-Path $oxy "sonic3air\data") (Join-Path $air "data") /E /NFL /NDL /NJH /NJS /NP | Out-Null
robocopy (Join-Path $oxy "sonic3air\scripts") (Join-Path $air "scripts") /E /NFL /NDL /NJH /NJS /NP | Out-Null
robocopy (Join-Path $oxy "oxygenengine\data") (Join-Path $pack "oxygenengine\data") /E /NFL /NDL /NJH /NJS /NP | Out-Null
Copy-Item $PackExe $air
$dll = Join-Path (Split-Path $PackExe) "discord_game_sdk.dll"
if (Test-Path $dll) { Copy-Item $dll $air }
Copy-Item (Join-Path $oxy "sonic3air\_master_image_template\config.json") (Join-Path $air "config.json")
Push-Location $air
& ".\Sonic3AIR.exe" -pack
# compiled scripts: one start of the game writes saves\scripts.bin (it is stopped as soon as the file exists)
$p = Start-Process ".\Sonic3AIR.exe" -PassThru
$t = 0
while (-not (Test-Path "saves\scripts.bin") -and $t -lt 90) { Start-Sleep 1; $t++ }
Start-Sleep 2
Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
if (-not (Test-Path "saves\scripts.bin")) { throw "compiled scripts were not written" }
# the APK gets the same files as the web build. The remastered music (audioremaster.bin, 130 MB) is NOT part of it.
New-Item -ItemType Directory -Force (Join-Path $assets "data") | Out-Null
Copy-Item "config.json" $assets
foreach ($f in @("audiodata.bin", "enginedata.bin", "gamedata.bin")) { Copy-Item $f (Join-Path $assets "data") }
Copy-Item "data\metadata.json" (Join-Path $assets "data")
robocopy "data\font" (Join-Path $assets "data\font") /E /NFL /NDL /NJH /NJS /NP | Out-Null
Copy-Item "saves\scripts.bin" (Join-Path $assets "data\scripts.bin")
Pop-Location

# 2. the signing key (outside the repository)
$task = if ($DebugApk) { "assembleDebug" } else { "assembleRelease" }
if (-not $DebugApk)
{
	$ksDir = Join-Path $ToolsDir "keystore"
	$ks = Join-Path $ksDir "airnet-release.jks"
	$passFile = Join-Path $ksDir "airnet-release.password.txt"
	if (-not (Test-Path $ks))
	{
		New-Item -ItemType Directory -Force $ksDir | Out-Null
		$bytes = New-Object byte[] 24
		[Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($bytes)
		$password = [Convert]::ToBase64String($bytes).Replace('+', 'a').Replace('/', 'b').Replace('=', 'c')
		& (Join-Path $jdkDir.FullName "bin\keytool.exe") -genkeypair -keystore $ks -alias airnet -keyalg RSA -keysize 4096 -validity 36500 `
			-storepass $password -keypass $password -dname "CN=Sonic 3 AirNet"
		if ($LASTEXITCODE -ne 0) { throw "keytool failed" }
		Set-Content $passFile $password -Encoding ascii
		Write-Host "Created a NEW signing key: $ks (password in $passFile). Back both up: every later APK must use this same key." -ForegroundColor Yellow
	}
	$env:AIRNET_KEYSTORE = $ks
	$env:AIRNET_KEYSTORE_PASSWORD = (Get-Content $passFile -Raw).Trim()
	$env:AIRNET_KEY_ALIAS = "airnet"
}

# 3. compile (native libraries for every ABI), package, sign
Push-Location $proj
try
{
	& ".\gradlew.bat" --no-daemon "-PAIRNET_VERSION_NAME=$Version" "-PAIRNET_VERSION_CODE=$versionCode" "-PAIRNET_ABIS=$Abis" ":app:$task"
	if ($LASTEXITCODE -ne 0) { throw "gradle build failed" }
}
finally
{
	& ".\gradlew.bat" --stop 2>&1 | Out-Null
	Pop-Location
}
$variant = if ($DebugApk) { "debug" } else { "release" }
$built = Get-ChildItem (Join-Path $proj "app\build\outputs\apk\$variant") -Filter *.apk | Select-Object -First 1
if (-not $built) { throw "no APK was produced" }
$apk = Join-Path $OutDir ("Sonic3Airnet-$Version" + $(if ($DebugApk) { "-debug" } else { "" }) + ".apk")
Copy-Item $built.FullName $apk

# 4. the signature must verify
$buildTools = Get-ChildItem (Join-Path $sdk "build-tools") -Directory | Sort-Object Name | Select-Object -Last 1
& (Join-Path $buildTools.FullName "apksigner.bat") verify --verbose $apk
if ($LASTEXITCODE -ne 0) { throw "apksigner verify failed" }

# 5. guards
Add-Type -AssemblyName System.IO.Compression.FileSystem
# the APK must never contain a ROM
$zip = [IO.Compression.ZipFile]::OpenRead($apk)
try
{
	foreach ($e in $zip.Entries) { if ($e.Length -eq 4194304 -or $e.Name -match "Sonic_Knuckles") { throw "A 4 MB file (ROM?) is in the APK: $($e.FullName)" } }
	# the APK must never contain the builder's folders or Windows user name (the APK is published on a public page)
	$needles = @($root, $root.Replace('\', '/'), $ToolsDir, $ToolsDir.Replace('\', '/'), $env:USERNAME, $env:USERPROFILE, $env:USERPROFILE.Replace('\', '/')) | Where-Object { $_ }
	foreach ($e in $zip.Entries)
	{
		if ($e.Length -eq 0) { continue }
		$ms = New-Object IO.MemoryStream
		$s = $e.Open(); $s.CopyTo($ms); $s.Dispose()
		$text = [Text.Encoding]::GetEncoding(28591).GetString($ms.ToArray())
		$ms.Dispose()
		foreach ($n in $needles) { if ($text.Contains($n)) { throw "APK entry $($e.FullName) contains a local path or the user name" } }
	}
	Write-Host "Guards passed: no ROM-sized file, no local path or user name in any of $($zip.Entries.Count) entries"
}
finally { $zip.Dispose() }
Remove-Item $work -Recurse -Force
Remove-Item $assets -Recurse -Force
$hash = (Get-FileHash $apk -Algorithm SHA256).Hash.ToLower()
Set-Content ($apk + ".sha256") "$hash  $(Split-Path -Leaf $apk)" -Encoding ascii
"Android build done in $([int]$sw.Elapsed.TotalSeconds) s -> $apk ($([math]::Round((Get-Item $apk).Length / 1MB, 1)) MB, version $Version, code $versionCode, sha256 $hash)"
