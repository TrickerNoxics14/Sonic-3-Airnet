param([switch]$Rebuild)
$ErrorActionPreference = "Stop"
$workspaceRoot = Split-Path -Parent $PSScriptRoot
$sourceRoot = Join-Path $workspaceRoot "sonic3air_online_stable_source"
$solution = Join-Path $sourceRoot "Oxygen\sonic3air\build\_vstudio\sonic3air.sln"
$msbuild = "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
$logPath = Join-Path $PSScriptRoot "stable-build.log"

if (-not (Test-Path -LiteralPath $solution)) { throw "Stable solution not found: $solution" }
if (-not (Test-Path -LiteralPath $msbuild)) { throw "MSBuild not found: $msbuild" }

# Keep the builder's folder out of the binary: __FILE__/assert strings (cl /d1trimfile) and the PDB path (link /PDBALTPATH).
$env:CL = "/d1trimfile:`"$workspaceRoot`""
$env:_LINK_ = "/PDBALTPATH:%_PDB%"
$targets = if ($Rebuild) { "/target:sonic3air:Rebuild" } else { "/target:sonic3air" }

& $msbuild $solution $targets /p:Configuration=Release /p:Platform=x64 /m /nologo /verbosity:minimal "/flp:logfile=$logPath;verbosity=minimal"
if ($LASTEXITCODE -ne 0 -and (Select-String -LiteralPath $logPath -Pattern "C1001" -Quiet)) {
    # MSVC 14.44 sometimes hits an internal compiler error (C1001) in parallel link-time code generation. Retry once with one codegen thread.
    Write-Warning "Internal compiler error C1001 in parallel code generation; retrying the link with /cgthreads:1"
    $env:_LINK_ = "/PDBALTPATH:%_PDB% /cgthreads:1"
    & $msbuild $solution "/target:sonic3air" /p:Configuration=Release /p:Platform=x64 /m:1 /nologo /verbosity:minimal "/flp:logfile=$logPath;verbosity=minimal"
}
if ($LASTEXITCODE -ne 0) {
    Write-Error "Stable online fork build failed (exit $LASTEXITCODE). See $logPath"
    exit $LASTEXITCODE
}

$exePath = Join-Path $sourceRoot "Oxygen\sonic3air\bin\Release_x64\Sonic3AIR.exe"
if (-not (Test-Path -LiteralPath $exePath)) { throw "Build returned success but executable is missing: $exePath" }
Write-Output "PASS stable online fork build: $exePath"

# Remove the builder's workspace path left in prebuilt third-party libraries (SDL, Dear ImGui).
& (Join-Path $PSScriptRoot "scrub_exe_paths.ps1") -ExePath $exePath -Prefix $workspaceRoot
