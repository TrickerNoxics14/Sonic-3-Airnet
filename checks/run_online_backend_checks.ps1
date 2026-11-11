$ErrorActionPreference = "Stop"
$checksDir = $PSScriptRoot
$reportPath = Join-Path $checksDir "online_backend_report.txt"
$workspaceRoot = Split-Path -Parent $checksDir
$stableRoot = Join-Path $workspaceRoot "sonic3air_online_stable_source"
$results = [System.Collections.Generic.List[string]]::new()
try {
    # The build comes first: the offline core tests link the freshly built network library.
    $buildOutput = & (Join-Path $checksDir "build_online_stable.ps1") 2>&1
    $buildExitCode = $LASTEXITCODE
    $buildOutput | ForEach-Object { Write-Output $_ }
    if ($buildExitCode -ne 0) { throw "Stable build returned exit code $buildExitCode" }
    $results.Add("PASS stable v26.03.28.0 online fork Release x64 build")

    $coreOutput = & (Join-Path $checksDir "run_online_core_tests.ps1") 2>&1
    $coreExitCode = $LASTEXITCODE
    $coreOutput | ForEach-Object {
        Write-Output $_
        $line = $_.ToString()
        if ($line.StartsWith("PASS ") -or $line.StartsWith("FAIL ")) { $results.Add($line) }
    }
    if ($coreExitCode -ne 0) { throw "Core tests returned exit code $coreExitCode" }
    $results.Add("PASS offline online-core regression tests completed")

    $packageOutput = & (Join-Path $checksDir "make_package.ps1") 2>&1
    $packageExitCode = $LASTEXITCODE
    $packageOutput | ForEach-Object { Write-Output $_ }
    if ($packageExitCode -ne 0) { throw "Online package build/guard returned exit code $packageExitCode" }
    $results.Add("PASS online package built from the stable fork and passed content guards (no test-hooks marker shipped)")

    # D join by code, E bad connection, M public matchmaking, N four players, R reconnect, L lobby,
    # S join secret (wrong code refused), X failure reasons and Retry, W a lag proxy with 200 ms / jitter / loss, L also the lobby features, U the unzipped package ignores developer variables
    $runtimeScript = Join-Path $checksDir "run_checks.ps1"
    $runtimeOutput = & powershell -NoProfile -ExecutionPolicy Bypass -File $runtimeScript -RuntimeOnly -EngineRoot $stableRoot -Only D,E,M,N,R,L,S,X,W,U 2>&1
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
    $results.Add("PASS stable online runtime checks: join by code, join secret, failure reasons, network stress, four peers, reconnect, lobby, packaged game")
}
catch {
    $results.Add("FAIL $($_.Exception.Message)")
}

$results | Set-Content -LiteralPath $reportPath -Encoding UTF8
$results | ForEach-Object { Write-Output $_ }
if ($results | Where-Object { $_.StartsWith("FAIL ") }) { exit 1 }
Write-Output "ONLINE BACKEND CHECKS PASS"
