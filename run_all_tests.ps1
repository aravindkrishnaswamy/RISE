# -----------------------------------------------------------------------------
# run_all_tests.ps1 - Windows test driver for RISE
#
# Mirrors run_all_tests.sh: discover every tests/*.cpp, build it, run it,
# capture per-test logs, and report a build/run summary. The bash script drives
# the GNU make build under build/make/rise; this one drives the CMake build
# under build/cmake/rise-tests.
#
# Usage:
#   .\run_all_tests.ps1                          # Release, Tier=all (extended on)
#   .\run_all_tests.ps1 -Config Debug            # Use dbin/RISE.lib
#   .\run_all_tests.ps1 -Tier fast               # Fast tier (RISE_TEST_EXTENDED=0)
#   .\run_all_tests.ps1 -Tier extended           # Extended tier only (RISE_TEST_EXTENDED=1)
#   .\run_all_tests.ps1 -TimeoutSeconds 60       # Kill any test exceeding 60s
#   .\run_all_tests.ps1 -Jobs 1                  # Run tests one at a time (default: one per core)
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
    [ValidateSet('all', 'fast', 'extended')]
    [string]$Tier = $(if ($env:RISE_TEST_TIER) { $env:RISE_TEST_TIER } else { 'all' }),
    [int]$TimeoutSeconds = 0,
    # How many test binaries may RUN at once.  Tests marked `exclusive` in
    # tests\test_schedule.tsv always run alone.  1 = one at a time, alphabetical
    # (single tests still get one render thread).
    # 0 = RISE_TEST_RUN_JOBS, else one per logical processor (checked below).
    [int]$Jobs = 0,
    [string]$LogDir,
    [switch]$NoBuild,
    [switch]$BuildOnly,
    [string[]]$Filter
)

$ErrorActionPreference = 'Stop'

$origRiseTestExtended = $env:RISE_TEST_EXTENDED
$origTmpDir = $env:TMPDIR
$origRiseMediaPath = $env:RISE_MEDIA_PATH
$origRiseOptionsFile = $env:RISE_OPTIONS_FILE
try {
# Clear any caller-inherited RISE_MEDIA_PATH (mirroring run_all_tests.sh) so
# tests running from repo root resolve relative `scenes/...` and `models/...`
# paths cleanly even if the user shell set RISE_MEDIA_PATH without a trailing
# separator or pointing at another checkout.
Remove-Item Env:RISE_MEDIA_PATH -ErrorAction SilentlyContinue
if ([string]::IsNullOrEmpty($env:TMPDIR) -and -not [string]::IsNullOrEmpty($env:TEMP)) {
    # Ensure tests that read std::getenv("TMPDIR") resolve to Windows TEMP rather than "/tmp".
    $env:TMPDIR = $env:TEMP
}
# Best-effort `\tmp` creation on the current drive root; tests resolve scratch
# paths via `RISE::TestSupport::ScratchBaseDir()` (`TMPDIR` -> `TEMP` -> `TMP`
# -> `temp_directory_path()`), so a read-only drive root on locked-down hosts
# must never abort the runner under `$ErrorActionPreference = 'Stop'`.
$driveRoot = [System.IO.Path]::GetPathRoot((Get-Location).Path)
if (-not [string]::IsNullOrEmpty($driveRoot)) {
    $rootTmp = Join-Path $driveRoot 'tmp'
    if (-not (Test-Path -LiteralPath $rootTmp)) {
        try {
            New-Item -ItemType Directory -Path $rootTmp -Force -ErrorAction Stop | Out-Null
        } catch {
            # Non-fatal: standalone tests use ScratchBaseDir() -> $env:TMPDIR / $env:TEMP.
        }
    }
}

$Tier = $Tier.ToLowerInvariant()
switch ($Tier) {
    'all' {
        if ([string]::IsNullOrEmpty($origRiseTestExtended)) {
            $env:RISE_TEST_EXTENDED = '1'
        }
    }
    'fast' {
        $env:RISE_TEST_EXTENDED = '0'
    }
    'extended' {
        $env:RISE_TEST_EXTENDED = '1'
    }
}

# Heavy multi-minute statistical/integration sweep suites assigned exclusively
# to the extended tier when -Tier fast or -Tier extended is requested.
# Rule: a test that takes more than about 200 s on its own does not belong in
# the fast tier.  Put it here, or give it a cheap fast mode
# ($DualModeExtendedTests below).  Keep both lists identical to
# run_all_tests.sh's is_extended_test / has_extended_mode.
$ExtendedTests = [System.Collections.Generic.HashSet[string]]::new(
    [string[]]@(
        'AgentProposeRenderTest', 'AgentViewModeRenderTest', 'BDPTDepthCapMISTest',
        'BDPTGuidedContinuationTest', 'BDPTStrategyBalanceTest',
        'CompositeEnergyConservationTest', 'ConnectionTypeSplitTest', 'CstDeriveGoldenTest',
        'ExteriorIndexInvarianceTest', 'GradedIndexInteriorFactorTest',
        'LayeredWhiteFurnaceTest', 'MediumEnclosureConnectibilityTest',
        'MediumInsideOutsideInvariantTest', 'MotionBlurTimeAverageTest',
        'OIDNAutoDeterminismTest', 'OpenSheetIndexConventionTest', 'PTGuidingMISPartitionTest',
        'RandomWalkFogEnergyTest', 'RefractiveRadianceScalingTest',
        'SignalIntegratorConsistencyTest', 'SMSDisplacedEnergyTest', 'SMSEmitterDirectionTest',
        'SMSExtendedHWSSTest', 'SMSExtendedPartitionTest', 'SMSExtendedReferenceTest',
        'SMSFourVertexVarianceTest', 'SMSHWSSNestedEnergyTest', 'SMSLegacyModeTest',
        'SMSMediumAnchorTest', 'SMSPhotonSeedEnergyTest', 'SMSSupersededRowsTest',
        'SMSUniformDispersionTest', 'SpectralOnlyEmitterTest', 'SSSContactContinuationTest',
        'SSSExteriorIndexInvarianceTest', 'UnlistedEmitterWeightTest',
        'VCMStrategyBalanceTest', 'VCMVolumeMergeTest', 'VolumeAbsorptionAttenuationTest',
        'WeaveGapShadowTransmittanceTest'
    ),
    [System.StringComparer]::Ordinal
)

# Dual-mode suites that run fast smoke/structural checks under -Tier fast
# (RISE_TEST_EXTENDED=0) and full extended workloads under -Tier extended / all
# (RISE_TEST_EXTENDED=1).
$DualModeExtendedTests = [System.Collections.Generic.HashSet[string]]::new(
    [string[]]@(
        'AgentSkillsTest', 'InteriorCandidateWalkCostTest', 'LightExitanceTest',
        'LuminarySamplerAreaTest', 'PavilionColonnadeShowcaseTest',
        'PolishedBRDFConsistencyTest', 'SchlickSPFPdfConsistencyTest',
        'ShelfBunnyShowcaseTest', 'SPFPdfConsistencyTest', 'TidalStonesShowcaseTest',
        'TimeIndexedMotionBlurTest'
    ),
    [System.StringComparer]::Ordinal
)

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
        # Per checkout (a hash of its path), so runs in two worktrees at once
        # do not delete each other's logs and run state.
        $sha = [System.Security.Cryptography.SHA1]::Create()
        $repoHash = ([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($RepoRoot))) -replace '-', '').Substring(0, 10)
        $LogDir = Join-Path $env:TEMP "rise-tests-logs-$repoHash"
    }
}
# Absolute (against the caller's directory, before the Push-Location below):
# tests get TMPDIRs under it and some change directory first.
$LogDir = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($LogDir)
if ($env:RISE_OPTIONS_FILE) {
    $env:RISE_OPTIONS_FILE = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($env:RISE_OPTIONS_FILE)
}
# Tests open scenes\... and read global.options relative to the working
# directory, so run them from the repo root; the finally block restores the
# caller's location.
Push-Location -LiteralPath $RepoRoot -StackName RiseRunAllTests

# -Jobs: the parameter, else RISE_TEST_RUN_JOBS, else one per processor.
if ($Jobs -eq 0) {
    if ($env:RISE_TEST_RUN_JOBS) {
        $parsedJobs = 0
        if (-not [int]::TryParse($env:RISE_TEST_RUN_JOBS, [ref]$parsedJobs) -or $parsedJobs -lt 1) {
            Write-Host "ERROR: RISE_TEST_RUN_JOBS='$($env:RISE_TEST_RUN_JOBS)' is not a positive integer" -ForegroundColor Red
            exit 64
        }
        $Jobs = $parsedJobs
    } else {
        $Jobs = [Environment]::ProcessorCount
    }
}
if ($Jobs -lt 1) {
    Write-Host "ERROR: -Jobs $Jobs is not a positive integer" -ForegroundColor Red
    exit 64
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
if ($Tier -eq 'fast') {
    $testSources = $testSources | Where-Object { -not $ExtendedTests.Contains($_.BaseName) }
} elseif ($Tier -eq 'extended') {
    $testSources = $testSources | Where-Object {
        $ExtendedTests.Contains($_.BaseName) -or $DualModeExtendedTests.Contains($_.BaseName)
    }
}
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
    Write-Host "ERROR: No test sources found under $SrcDir matching selection." -ForegroundColor Red
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
$buildRc = 0
$anyTargetFailed = $false

if (-not $NoBuild) {
    $buildLog = Join-Path $LogDir 'build.log'
    Write-Host ('Building {0} test(s) [{1}, tier={2}]...' -f $total, $Config, $Tier)
    $bs = Get-Date

    if ($Filter -or $Tier -eq 'extended') {
        # Filtered or extended subset: build each matching target individually so
        # we don't over-build. msbuild startup overhead is acceptable for small subsets.
        # Continue past per-test failures so one broken test doesn't mask the
        # state of the others (matches the bash script's loop semantics).
        foreach ($src in $testSources) {
            & $cmake --build $CmakeBuildDir --config $Config --target $src.BaseName *>&1 |
                Out-File -FilePath $buildLog -Encoding utf8 -Append
            # Remember ANY failed target, not only the last one.
            if ($LASTEXITCODE -ne 0) { $anyTargetFailed = $true }
        }
    } else {
        & $cmake --build $CmakeBuildDir --config $Config --target rise_all_tests --parallel *>&1 |
            Out-File -FilePath $buildLog -Encoding utf8
    }
    $buildRc = if ($anyTargetFailed) { 1 } else { $LASTEXITCODE }
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
# Phase 2: Run the built tests, up to $Jobs at a time, with captured output
# and an optional timeout.
#
# tests\test_schedule.tsv (shared with run_all_tests.sh) names the tests that
# must run ALONE (`exclusive`: multi-core renders, wall-clock gates), the tests
# that share the cores with their render threads forced to one (`single`), and
# the last measured run time of the long ones, so long tests start first.
# Exclusive tests run first, one at a time on an otherwise idle machine; the
# rest then share the cores.  Unlisted tests are `pool`.  -Jobs 1 runs one test
# at a time in alphabetical order (single tests still get one render thread).
#
# Every test gets its own TMPDIR/TEMP/TMP ($LogDir\.tmp\<name>), so two
# different tests that pick the same scratch-file name cannot collide.  Like
# the log, it is removed when the test passes and kept when it fails.
# -----------------------------------------------------------------------------

$found = 0
$passed = 0
$failed = 0
$skipped = 0
$runFailures = @()
$timings = @()
$cpuRows = @()

$schedule = @{}
$scheduleFile = Join-Path $SrcDir 'test_schedule.tsv'
if (Test-Path -LiteralPath $scheduleFile) {
    foreach ($line in Get-Content -LiteralPath $scheduleFile) {
        if ($line -match '^\s*(#|$)') { continue }
        $cols = $line -split "`t"
        if ($cols.Count -lt 2) { continue }
        $cls = $cols[1]
        if ($cls -cne 'pool' -and $cls -cne 'exclusive' -and $cls -cne 'single') {
            Write-Host ('warning: tests\test_schedule.tsv: unknown class "{0}" for {1}; running it exclusive' -f $cls, $cols[0])
            $cls = 'exclusive'
        }
        $sec = 0.0
        if ($cols.Count -ge 3) { [void][double]::TryParse($cols[2], [ref]$sec) }
        $schedule[$cols[0]] = [pscustomobject]@{ Class = $cls; Seconds = $sec }
    }
}

$plan = foreach ($src in $testSources) {
    $entry = $schedule[$src.BaseName]
    [pscustomobject]@{
        Name    = $src.BaseName
        Src     = $src
        Class   = if ($entry) { $entry.Class } else { 'pool' }
        Seconds = if ($entry) { $entry.Seconds } else { 0.0 }
    }
}
if ($Jobs -gt 1) {
    # Exclusive tests first; then the rest (pool and single), longest first.
    $plan = $plan | Sort-Object -Property @{ Expression = { if ($_.Class -ceq 'exclusive') { 0 } else { 1 } } }, @{ Expression = 'Seconds'; Descending = $true }, @{ Expression = 'Name' }
}
$plan = @($plan)

$tmpRoot = Join-Path $LogDir '.tmp'
$null = New-Item -ItemType Directory -Force -Path $tmpRoot

# `single` tests read this options file: the repo's options (or the caller's
# RISE_OPTIONS_FILE) plus force_number_of_threads 1.
$singleOptions = Join-Path $LogDir '.single-thread.options'
$optionsSource = if ($env:RISE_OPTIONS_FILE -and (Test-Path -LiteralPath $env:RISE_OPTIONS_FILE)) { $env:RISE_OPTIONS_FILE } else { Join-Path $RepoRoot 'global.options' }
$optionsText = if (Test-Path -LiteralPath $optionsSource) { Get-Content -Raw -LiteralPath $optionsSource } else { '' }
# The force line goes FIRST: Options keeps the first occurrence of a key.
Set-Content -LiteralPath $singleOptions -Value ("force_number_of_threads 1`n" + $optionsText) -Encoding ascii

# Kill a test and every process it started (agent tests spawn rise.exe).
function Stop-RiseTestTree($proc) {
    if (-not $proc -or $proc.HasExited) { return }
    try { & taskkill.exe /T /F /PID $proc.Id *> $null } catch { }
    try { if (-not $proc.HasExited) { $proc.Kill() } } catch { }
}

function Start-RiseTest($item) {
    $exe = Join-Path $BinDir "$($item.Name).exe"
    $log = Join-Path $LogDir "$($item.Name).log"
    $tmp = Join-Path $tmpRoot $item.Name
    if (Test-Path -LiteralPath $tmp) { Remove-Item -Recurse -Force -LiteralPath $tmp }
    $null = New-Item -ItemType Directory -Force -Path $tmp
    $saved = @($env:TMPDIR, $env:TEMP, $env:TMP, $env:RISE_OPTIONS_FILE)
    $env:TMPDIR = $tmp
    $env:TEMP = $tmp
    $env:TMP = $tmp
    if ($item.Class -ceq 'single') { $env:RISE_OPTIONS_FILE = $singleOptions }
    $proc = $null
    $launchError = $null
    try {
        $proc = Start-Process -FilePath $exe -PassThru -NoNewWindow `
            -RedirectStandardOutput "$log.stdout" -RedirectStandardError "$log.stderr"
        # Touch Handle now: without it PowerShell may never report ExitCode.
        $null = $proc.Handle
    } catch {
        # A launch that fails (antivirus holding a fresh exe, ...) is that
        # test's FAIL, not the end of the run.
        $launchError = $_.Exception.Message
    } finally {
        $env:TMPDIR = $saved[0]
        $env:TEMP = $saved[1]
        $env:TMP = $saved[2]
        $env:RISE_OPTIONS_FILE = $saved[3]
    }
    return [pscustomobject]@{
        Item = $item; Proc = $proc; Start = Get-Date; Log = $log; Tmp = $tmp
        Cpu = 0.0; TimedOut = $false; LaunchError = $launchError
    }
}

function Complete-RiseTest($run) {
    $name = $run.Item.Name
    if ($run.LaunchError) {
        Set-Content -LiteralPath $run.Log -Value "launch failed: $($run.LaunchError)" -Encoding utf8
        $script:doneCount++
        Write-Host ('[ {0,3}/{1,3} ] {2,-46} FAIL (launch failed: {3})' -f $script:doneCount, $total, $name, $run.LaunchError)
        $script:runFailures += [pscustomobject]@{ Name = $name; Code = -1; Log = $run.Log; Reason = 'launch' }
        $script:failed++
        return
    }
    $run.Proc.WaitForExit()
    $wall = ((Get-Date) - $run.Start).TotalSeconds
    $dur = [int]$wall
    try { $run.Cpu = $run.Proc.TotalProcessorTime.TotalSeconds } catch { }
    $rc = if ($run.TimedOut) { 124 } else { $run.Proc.ExitCode }
    $stdout = if (Test-Path -LiteralPath "$($run.Log).stdout") { Get-Content -Raw -LiteralPath "$($run.Log).stdout" } else { '' }
    $stderr = if (Test-Path -LiteralPath "$($run.Log).stderr") { Get-Content -Raw -LiteralPath "$($run.Log).stderr" } else { '' }
    Set-Content -LiteralPath $run.Log -Value ($stdout + $stderr) -Encoding utf8
    Remove-Item -Force -LiteralPath "$($run.Log).stdout", "$($run.Log).stderr" -ErrorAction SilentlyContinue

    $cores = if ($wall -gt 0) { $run.Cpu / $wall } else { 0.0 }
    $script:timings += [pscustomobject]@{ Name = $name; Seconds = $dur }
    $script:cpuRows += [pscustomobject]@{ Name = $name; Seconds = $dur; Cores = $cores; Class = $run.Item.Class }
    $script:doneCount++
    $prefix = '[ {0,3}/{1,3} ] {2,-46}' -f $script:doneCount, $total, $name
    if ($rc -eq 0) {
        Write-Host ('{0} PASS ({1}s, {2:N1} cores)' -f $prefix, $dur, $cores)
        Remove-Item -LiteralPath $run.Log -ErrorAction SilentlyContinue
        Remove-Item -Recurse -Force -LiteralPath $run.Tmp -ErrorAction SilentlyContinue
        $script:passed++
    } elseif ($rc -eq 124) {
        Write-Host ('{0} TIMEOUT (>{1}s)' -f $prefix, $TimeoutSeconds)
        $script:runFailures += [pscustomobject]@{ Name = $name; Code = $rc; Log = $run.Log; Reason = 'timeout' }
        $script:failed++
    } else {
        Write-Host ('{0} FAIL (exit={1}, {2}s)' -f $prefix, $rc, $dur)
        $script:runFailures += [pscustomobject]@{ Name = $name; Code = $rc; Log = $run.Log; Reason = 'fail' }
        $script:failed++
    }
}

Write-Host ('Running {0} test(s), up to {1} at a time (exclusive tests alone)' -f $plan.Count, $Jobs)
$runStart = Get-Date
$doneCount = 0
$running = New-Object System.Collections.ArrayList
$next = 0
try {
    while ($true) {
        # Reap finished tests; kill any past the timeout.
        $still = New-Object System.Collections.ArrayList
        foreach ($run in @($running)) {
            if (-not $run.LaunchError -and -not $run.Proc.HasExited) {
                try { $run.Cpu = $run.Proc.TotalProcessorTime.TotalSeconds } catch { }
                if ($TimeoutSeconds -gt 0 -and ((Get-Date) - $run.Start).TotalSeconds -gt $TimeoutSeconds) {
                    $run.TimedOut = $true
                    Stop-RiseTestTree $run.Proc
                }
                [void]$still.Add($run)
                continue
            }
            try {
                Complete-RiseTest $run
            } catch {
                # Bookkeeping failed (e.g. a log could not be read): count the
                # test as failed rather than abort the whole run.
                Write-Host ('[ {0,3}/{1,3} ] {2,-46} FAIL (runner error: {3})' -f (++$script:doneCount), $total, $run.Item.Name, $_.Exception.Message)
                $script:runFailures += [pscustomobject]@{ Name = $run.Item.Name; Code = -1; Log = $run.Log; Reason = 'runner' }
                $script:failed++
            }
        }
        $running = $still

        # Start as many as the schedule allows: an exclusive test only on an
        # idle machine, and nothing beside a running exclusive test.
        while ($next -lt $plan.Count) {
            $item = $plan[$next]
            $exe = Join-Path $BinDir "$($item.Name).exe"
            $doneSkip = $null
            if (-not (Test-Path -LiteralPath $exe)) {
                $doneSkip = 'SKIP (build failed or missing)'
            } elseif (-not $NoBuild -and (Get-Item -LiteralPath $exe).LastWriteTime -lt $item.Src.LastWriteTime) {
                # Source newer than exe means a recent build attempt failed.
                $doneSkip = 'SKIP (build failed, exe is stale)'
            }
            if ($doneSkip) {
                $doneCount++
                Write-Host ('[ {0,3}/{1,3} ] {2,-46} {3}' -f $doneCount, $total, $item.Name, $doneSkip)
                $skipped++
                $next++
                continue
            }
            if ($running.Count -gt 0) {
                $exclusiveRunning = @($running | Where-Object { $_.Item.Class -ceq 'exclusive' }).Count -gt 0
                if ($exclusiveRunning -or $item.Class -ceq 'exclusive' -or $running.Count -ge $Jobs) { break }
            }
            $found++
            [void]$running.Add((Start-RiseTest $item))
            $next++
        }

        if ($running.Count -eq 0 -and $next -ge $plan.Count) { break }
        Start-Sleep -Milliseconds 200
    }
} finally {
    # Ctrl-C or an error: do not leave test processes behind.
    foreach ($run in @($running)) {
        if (-not $run.LaunchError) { Stop-RiseTestTree $run.Proc }
    }
}
$runWall = [int]((Get-Date) - $runStart).TotalSeconds

# -----------------------------------------------------------------------------
# Summary
# -----------------------------------------------------------------------------

Write-Host '============================================================'
Write-Host ('Build: {0} built, {1} failed (of {2})' -f $built, $buildFailed, $total)
Write-Host ('Run:   {0} passed, {1} failed, {2} skipped (of {3} run)' `
    -f $passed, $failed, $skipped, $found)
if ($found -gt 0) {
    $runSum = ($timings | Measure-Object -Property Seconds -Sum).Sum
    Write-Host ('Time:  {0}s wall for {1}s of test time (-Jobs {2})' -f $runWall, $runSum, $Jobs)
}

if ($found -gt 1) {
    Write-Host ''
    Write-Host 'Slowest tests (s):'
    foreach ($t in ($timings | Sort-Object -Property Seconds -Descending | Select-Object -First 10)) {
        Write-Host ('  {0,4}s  {1}' -f $t.Seconds, $t.Name)
    }
}

# A pool test that keeps several cores busy competes with its neighbours and
# breaks the one-multi-core-render-at-a-time rule, so flag it for an
# `exclusive` entry.  Only meaningful when tests ran side by side.
if ($Jobs -gt 1) {
    $busy = @($cpuRows | Where-Object { $_.Class -cne 'exclusive' -and $_.Seconds -ge 20 -and $_.Cores -ge 1.6 })
    if ($busy.Count -gt 0) {
        Write-Host ''
        Write-Host 'Pool / single tests that kept several cores busy (mark them exclusive in tests\test_schedule.tsv):'
        foreach ($b in $busy) { Write-Host ('  {0,-46} {1,-6} {2,5:N1} cores over {3}s' -f $b.Name, $b.Class, $b.Cores, $b.Seconds) }
    }
}

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

# A failed build fails the run even when every exe (possibly stale against a
# changed header or library) ran green -- as run_all_tests.sh does.
if ($failed -ne 0 -or $buildFailed -ne 0 -or $buildRc -ne 0) {
    if ($buildRc -ne 0 -and $failed -eq 0 -and $buildFailed -eq 0) {
        Write-Host "Build step failed (exit=$buildRc); see the build log"
    }
    exit 1
}
Write-Host "All $found tests passed"
exit 0

} finally {
    Pop-Location -StackName RiseRunAllTests -ErrorAction SilentlyContinue
    $env:RISE_OPTIONS_FILE = $origRiseOptionsFile
    $env:RISE_TEST_EXTENDED = $origRiseTestExtended
    $env:TMPDIR = $origTmpDir
    $env:RISE_MEDIA_PATH = $origRiseMediaPath
}
