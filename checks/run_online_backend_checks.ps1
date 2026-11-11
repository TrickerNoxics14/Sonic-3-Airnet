$ErrorActionPreference = "Stop"
$checksDir = $PSScriptRoot
$reportPath = Join-Path $checksDir "online_backend_report.txt"
$workspaceRoot = Split-Path -Parent $checksDir
$stableRoot = Join-Path $workspaceRoot "sonic3air_online_stable_source"
$results = [System.Collections.Generic.List[string]]::new()
try {
    $coreOutput = & (Join-Path $checksDir "run_online_core_tests.ps1") 2>&1
    $coreExitCode = $LASTEXITCODE
    $coreOutput | ForEach-Object {
        Write-Output $_
        $line = $_.ToString()
        if ($line.StartsWith("PASS ") -or $line.StartsWith("FAIL ")) { $results.Add($line) }
    }
    if ($coreExitCode -ne 0) { throw "Core tests returned exit code $coreExitCode" }
    $results.Add("PASS offline online-core regression tests completed")

    $buildOutput = & (Join-Path $checksDir "build_online_stable.ps1") 2>&1
    $buildExitCode = $LASTEXITCODE
    $buildOutput | ForEach-Object { Write-Output $_ }
    if ($buildExitCode -ne 0) { throw "Stable build returned exit code $buildExitCode" }
    $results.Add("PASS stable v26.03.28.0 online fork Release x64 build")

    $packageOutput = & (Join-Path $checksDir "make_package.ps1") 2>&1
    $packageExitCode = $LASTEXITCODE
    $packageOutput | ForEach-Object { Write-Output $_ }
    if ($packageExitCode -ne 0) { throw "Online package build/guard returned exit code $packageExitCode" }
    $results.Add("PASS online package built from the stable fork and passed content guards")

    $runtimeScript = Join-Path $checksDir "run_checks.ps1"
    $runtimeOutput = & powershell -NoProfile -ExecutionPolicy Bypass -File $runtimeScript -RuntimeOnly -EngineRoot $stableRoot -Only D,E,M,N,L 2>&1
    $runtimeExitCode = $LASTEXITCODE
    $runtimeOutput | ForEach-Object {
        Write-Output $_
    }
    $runtimeReport = Join-Path $checksDir "online_stable_runtime_report.txt"
    if (Test-Path -LiteralPath $runtimeReport) {
        foreach ($line in Get-Content -LiteralPath $runtimeReport) {
            if ($line -match '^(PASS|FAIL)\s+') { $results.Add($line.Trim()) }
        }
    }
    if ($runtimeExitCode -ne 0) { throw "Stable online runtime checks returned exit code $runtimeExitCode" }
    $results.Add("PASS stable online runtime checks: two-player join, network stress, four peers and reconnect")
}
catch {
    $results.Add("FAIL $($_.Exception.Message)")
}

$results | Set-Content -LiteralPath $reportPath -Encoding UTF8
$results | ForEach-Object { Write-Output $_ }
if ($results | Where-Object { $_.StartsWith("FAIL ") }) { exit 1 }
Write-Output "ONLINE BACKEND CHECKS PASS"
