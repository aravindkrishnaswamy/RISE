# -----------------------------------------------------------------------------
# run_all_tests.ps1 - Windows test driver for RISE
#
# Mirrors run_all_tests.sh: discover every tests/*.cpp, build it, run it,
# capture per-test logs, and report a build/run summary. The bash script drives
# the GNU make build under build/make/rise; this one drives the CMake build
# under build/cmake/rise-tests.
#
# Usage:
#   .\run_all_tests.ps1                          # Release, no timeout
#   .\run_all_tests.ps1 -Config Debug            # Use dbin/RISE.lib
#   .\run_all_tests.ps1 -TimeoutSeconds 60       # Kill any test exceeding 60s
#   .\run_all_tests.ps1 -NoBuild                 # Run pre-built tests only
#   .\run_all_tests.ps1 -BuildOnly               # Build only, don't run
#   .\run_all_tests.ps1 -Filter Math3D*,*Noise3D # Run subset matching wildcards
#
# Exit code: 0 if every built test passed, 1 if any build or run failed.
# -----------------------------------------------------------------------------

#Requires -Version 5.1
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Config = 'Release',
    [int]$TimeoutSeconds = 0,
    [string]$LogDir,
    [switch]$NoBuild,
    [switch]$BuildOnly,
    [string[]]$Filter
)

$ErrorActionPreference = 'Stop'

$RepoRoot      = Split-Path -Parent $PSCommandPath
$BinSubdir     = if ($Config -eq 'Debug') { 'dbin' } else { 'bin' }
$BinDir        = Join-Path $RepoRoot "$BinSubdir\tests"
$SrcDir        = Join-Path $RepoRoot 'tests'
$CmakeSrcDir   = Join-Path $RepoRoot 'build\cmake\rise-tests'
$CmakeBuildDir = Join-Path $CmakeSrcDir '_out'

# Logs go outside the repo to mirror the .sh script's intent.
if (-not $LogDir) {
    if ($env:RISE_TEST_LOG_DIR) {
        $LogDir = $env:RISE_TEST_LOG_DIR
    } else {
        $LogDir = Join-Path $env:TEMP 'rise-tests-logs'
    }
}

# Honor RISE_TEST_TIMEOUT env var when -TimeoutSeconds wasn't passed.
if ($TimeoutSeconds -eq 0 -and $env:RISE_TEST_TIMEOUT) {
    $TimeoutSeconds = [int]$env:RISE_TEST_TIMEOUT
}

# -----------------------------------------------------------------------------
# Locate cmake
#
# Prefer PATH; fall back to common VS install locations. CMake 3.20+ is
# required to consume build/cmake/rise-tests/CMakeLists.txt.
# -----------------------------------------------------------------------------

$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) {
    $cmakeCandidates = @(
        "$env:ProgramFiles\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "$env:ProgramFiles\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "$env:ProgramFiles\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "$env:ProgramFiles\Microsoft Visual Studio\2022\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    )
    foreach ($c in $cmakeCandidates) {
        if (Test-Path -LiteralPath $c) { $cmake = $c; break }
    }
}
if (-not $cmake) {
    Write-Host "ERROR: cmake not found on PATH or in known VS install locations." -ForegroundColor Red
    Write-Host "Install Visual Studio 2022+ with C++ workload (which bundles CMake) or add cmake to PATH."
    exit 1
}

# -----------------------------------------------------------------------------
# Discover tests
# -----------------------------------------------------------------------------

if (-not (Test-Path -LiteralPath $SrcDir)) {
    Write-Host "ERROR: tests directory not found: $SrcDir" -ForegroundColor Red
    exit 1
}

$testSources = Get-ChildItem -Path $SrcDir -Filter '*.cpp' -File | Sort-Object Name
if ($Filter) {
    # Accept "-Filter A,B,C" passed via powershell -File (which arrives as a
    # single string with commas) by splitting it into a real array.
    if ($Filter.Count -eq 1 -and $Filter[0] -match ',') {
        $Filter = $Filter[0] -split ','
    }
    $testSources = $testSources | Where-Object {
        $name = $_.BaseName
        foreach ($pattern in $Filter) {
            if ($name -like $pattern) { return $true }
        }
        return $false
    }
}

$total = $testSources.Count
if ($total -eq 0) {
    Write-Host "ERROR: No test sources found under $SrcDir matching filter." -ForegroundColor Red
    exit 1
}

# -----------------------------------------------------------------------------
# Setup output dirs
# -----------------------------------------------------------------------------

if (Test-Path -LiteralPath $LogDir) { Remove-Item -Recurse -Force -LiteralPath $LogDir }
$null = New-Item -ItemType Directory -Force -Path $LogDir
$null = New-Item -ItemType Directory -Force -Path $BinDir

# -----------------------------------------------------------------------------
# Phase 0: Configure CMake (one-time, or when CMakeCache.txt is missing)
# -----------------------------------------------------------------------------

$cacheFile = Join-Path $CmakeBuildDir 'CMakeCache.txt'
if (-not (Test-Path -LiteralPath $cacheFile)) {
    Write-Host "Configuring CMake build tree (one-time)..."
    & $cmake -S $CmakeSrcDir -B $CmakeBuildDir -A x64
    if ($LASTEXITCODE -ne 0) {
        Write-Host "ERROR: cmake configure failed (exit=$LASTEXITCODE)" -ForegroundColor Red
        exit 1
    }
    Write-Host ""
}

# -----------------------------------------------------------------------------
# Phase 1: Build (one aggregated msbuild invocation, parallelized)
#
# Why aggregated: per-target msbuild invocations have ~1-2s startup overhead
# each, so running 60 of them adds ~2 min of pure overhead. The aggregate
# `rise_all_tests` target depends on every test, and msbuild's parallel build
# (/m via --parallel) keeps building siblings when one project fails.
# -----------------------------------------------------------------------------

$built = 0
$buildFailed = 0
$buildFailures = @()

if (-not $NoBuild) {
    $buildLog = Join-Path $LogDir 'build.log'
    Write-Host ('Building {0} test(s) [{1}]...' -f $total, $Config)
    $bs = Get-Date

    if ($Filter) {
        # Filtered subset: build each matching target individually so we don't
        # over-build. msbuild startup overhead is acceptable for small subsets.
        # Continue past per-test failures so one broken test doesn't mask the
        # state of the others (matches the bash script's loop semantics).
        foreach ($src in $testSources) {
            & $cmake --build $CmakeBuildDir --config $Config --target $src.BaseName *>&1 |
                Out-File -FilePath $buildLog -Encoding utf8 -Append
        }
    } else {
        & $cmake --build $CmakeBuildDir --config $Config --target rise_all_tests --parallel *>&1 |
            Out-File -FilePath $buildLog -Encoding utf8
    }
    $buildRc = $LASTEXITCODE
    $bd = [int]((Get-Date) - $bs).TotalSeconds

    # Walk the test list and check which exes were produced. Source-newer-than-exe
    # signals a stale binary from a previous build whose current rebuild failed.
    foreach ($src in $testSources) {
        $exe = Join-Path $BinDir "$($src.BaseName).exe"
        if ((Test-Path -LiteralPath $exe) -and `
            ((Get-Item -LiteralPath $exe).LastWriteTime -ge $src.LastWriteTime)) {
            $built++
        } else {
            $buildFailed++
            $buildFailures += $src.BaseName
        }
    }

    if ($buildRc -eq 0) {
        Write-Host ('Build: {0} built ({1}s)' -f $built, $bd)
    } else {
        Write-Host ('Build: {0} built, {1} failed (of {2}, {3}s) - see {4}' `
            -f $built, $buildFailed, $total, $bd, $buildLog)
        Write-Host ""
        if (Test-Path -LiteralPath $buildLog) {
            Get-Content -LiteralPath $buildLog -Tail 80 | Write-Host
        }
    }
    Write-Host ""
} else {
    # -NoBuild: assume binaries are present and skip Phase 1.
    foreach ($src in $testSources) {
        if (Test-Path -LiteralPath (Join-Path $BinDir "$($src.BaseName).exe")) {
            $built++
        }
    }
    Write-Host "Skipping build (-NoBuild); $built pre-built test exe(s) found."
    Write-Host ""
}

if ($BuildOnly) {
    if ($buildFailed -gt 0) { exit 1 } else { exit 0 }
}

# -----------------------------------------------------------------------------
# Phase 2: Run each built test with captured output and optional timeout
# -----------------------------------------------------------------------------

$found = 0
$passed = 0
$failed = 0
$skipped = 0
$runFailures = @()

$i = 0
foreach ($src in $testSources) {
    $i++
    $name = $src.BaseName
    $exe = Join-Path $BinDir "$name.exe"
    $prefix = '[ {0,3}/{1,3} ] {2,-46}' -f $i, $total, $name

    if (-not (Test-Path -LiteralPath $exe)) {
        Write-Host "$prefix SKIP (build failed or missing)"
        $skipped++
        continue
    }
    # Stale check: source newer than exe means a recent build attempt failed.
    if (-not $NoBuild -and `
        (Get-Item -LiteralPath $exe).LastWriteTime -lt $src.LastWriteTime) {
        Write-Host "$prefix SKIP (build failed, exe is stale)"
        $skipped++
        continue
    }

    $found++
    $log = Join-Path $LogDir "$name.log"
    Write-Host -NoNewline "$prefix ... "
    $start = Get-Date

    if ($TimeoutSeconds -gt 0) {
        # Start-Process with WaitForExit(ms) gives us a kill-on-timeout path.
        # Redirect to per-stream files because Start-Process can't merge
        # stdout+stderr into a single file natively; we concatenate after.
        $stdoutTmp = "$log.stdout"
        $stderrTmp = "$log.stderr"
        $proc = Start-Process -FilePath $exe -PassThru -NoNewWindow `
            -RedirectStandardOutput $stdoutTmp -RedirectStandardError $stderrTmp
        if ($proc.WaitForExit($TimeoutSeconds * 1000)) {
            $rc = $proc.ExitCode
        } else {
            $proc.Kill()
            $proc.WaitForExit()
            $rc = 124   # mimic GNU `timeout` exit code
        }
        $stdout = if (Test-Path -LiteralPath $stdoutTmp) { Get-Content -Raw -LiteralPath $stdoutTmp } else { '' }
        $stderr = if (Test-Path -LiteralPath $stderrTmp) { Get-Content -Raw -LiteralPath $stderrTmp } else { '' }
        Set-Content -LiteralPath $log -Value ($stdout + $stderr) -Encoding utf8
        Remove-Item -Force -LiteralPath $stdoutTmp, $stderrTmp -ErrorAction SilentlyContinue
    } else {
        & $exe *>&1 | Out-File -FilePath $log -Encoding utf8
        $rc = $LASTEXITCODE
    }
    $dur = [int]((Get-Date) - $start).TotalSeconds

    if ($rc -eq 0) {
        Write-Host ("PASS ({0}s)" -f $dur)
        Remove-Item -LiteralPath $log -ErrorAction SilentlyContinue
        $passed++
    } elseif ($rc -eq 124) {
        Write-Host ("TIMEOUT (>{0}s)" -f $TimeoutSeconds)
        $runFailures += [pscustomobject]@{ Name = $name; Code = $rc; Log = $log; Reason = 'timeout' }
        $failed++
    } else {
        Write-Host ("FAIL (exit={0}, {1}s)" -f $rc, $dur)
        $runFailures += [pscustomobject]@{ Name = $name; Code = $rc; Log = $log; Reason = 'fail' }
        $failed++
    }
}

# -----------------------------------------------------------------------------
# Summary
# -----------------------------------------------------------------------------

Write-Host '============================================================'
Write-Host ('Build: {0} built, {1} failed (of {2})' -f $built, $buildFailed, $total)
Write-Host ('Run:   {0} passed, {1} failed, {2} skipped (of {3} run)' `
    -f $passed, $failed, $skipped, $found)

if ($buildFailed -gt 0 -and $buildFailures.Count -gt 0) {
    Write-Host ''
    Write-Host 'Build failures:'
    foreach ($n in $buildFailures) { Write-Host "  - $n" }
}

foreach ($f in $runFailures) {
    Write-Host ''
    Write-Host ('--- RUN FAIL: {0} (exit={1}) - see {2} ---' -f $f.Name, $f.Code, $f.Log)
    if (Test-Path -LiteralPath $f.Log) { Get-Content -LiteralPath $f.Log | Write-Host }
}

if ($runFailures.Count -gt 0) {
    Write-Host ''
    Write-Host 'Run failures:'
    foreach ($f in $runFailures) { Write-Host "  - $($f.Name)" }
}

if ($failed -ne 0 -or $buildFailed -ne 0) {
    exit 1
}
Write-Host "All $found tests passed"
exit 0
