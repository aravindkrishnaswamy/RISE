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
#   .\run_all_tests.ps1 -Filter Polynomial*,*BVH* # Run subset matching wildcards
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
    # How many run slots the tests share.  A test takes one, or N when
    # tests\test_schedule.tsv marks it `wide:N`; tests marked `exclusive` always
    # run alone.  1 = one at a time, alphabetical (single tests still get one
    # render thread).
    # 0 = RISE_TEST_RUN_JOBS, else one per logical processor (checked below).
    [int]$Jobs = 0,
    [string]$LogDir,
    [switch]$ValidateLogDirOnly,
    [switch]$NoBuild,
    [switch]$BuildOnly,
    [string[]]$Filter
)

$ErrorActionPreference = 'Stop'

$origRiseTestExtended = $env:RISE_TEST_EXTENDED
$origTmpDir = $env:TMPDIR
$origRiseMediaPath = $env:RISE_MEDIA_PATH
$origRiseOptionsFile = $env:RISE_OPTIONS_FILE
$ownsLock = $false
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
        'MediumEnclosureConnectibilityTest', 'MediumInsideOutsideInvariantTest'
    ),
    [System.StringComparer]::Ordinal
)

# Dual-mode suites that run fast smoke/structural checks under -Tier fast
# (RISE_TEST_EXTENDED=0) and full extended workloads under -Tier extended / all
# (RISE_TEST_EXTENDED=1).
$DualModeExtendedTests = [System.Collections.Generic.HashSet[string]]::new(
    [string[]]@(
        'AgentSkillsTest', 'ClothValidationTest', 'FireSolverTest', 'LightExitanceTest', 'LiquidRigidValidationTest', 'LiquidValidationTest',
        'SPFPdfConsistencyTest', 'TimeIndexedMotionBlurTest'
    ),
    [System.StringComparer]::Ordinal
)

$RepoRoot      = Split-Path -Parent $PSCommandPath
$BinSubdir     = if ($Config -eq 'Debug') { 'dbin' } else { 'bin' }
$BinDir        = Join-Path $RepoRoot "$BinSubdir\tests"
$SrcDir        = Join-Path $RepoRoot 'tests'
$CmakeSrcDir   = Join-Path $RepoRoot 'build\cmake\rise-tests'
$CmakeBuildDir = Join-Path $CmakeSrcDir '_out'
$LibraryProject = Join-Path $RepoRoot 'build\VS2022\Library\Library.vcxproj'
$SolutionDir = [IO.Path]::GetFullPath(
    (Join-Path $RepoRoot 'build\VS2022')).TrimEnd(
        [IO.Path]::DirectorySeparatorChar,
        [IO.Path]::AltDirectorySeparatorChar)
$SolutionDir += [IO.Path]::DirectorySeparatorChar
$RiseLibrary = if ($Config -eq 'Debug') {
    Join-Path $RepoRoot 'dbin\RISE.lib'
} else {
    Join-Path $RepoRoot 'bin\RISE.lib'
}
$FireOpticsGenerator = Join-Path $RepoRoot 'tools\generate_fire_optics_records.py'
$FireOpticsData = Join-Path $RepoRoot 'docs\data'
$FireOpticsEmbedded = Join-Path $RepoRoot 'src\Library\Utilities\FireOpticsRecordData.inc'
$FireSimulationGenerator = Join-Path $RepoRoot 'tools\generate_fire_simulation_records.py'
$FireSimulationData = Join-Path $RepoRoot 'docs\data\source_pulls\fire_sim_open_sources_v1.json'
$FireSimulationMethaneConstants = Join-Path $RepoRoot 'docs\data\fire_fuel_methane_v1.draft.json'
$FireSimulationEmbedded = Join-Path $RepoRoot 'src\Library\Utilities\FireSimulationRecordData.inc'
$FireSimulationGeneratorTest = Join-Path $RepoRoot 'tests\test_fire_simulation_record_generator.py'
$FireGasOpacityGenerator = Join-Path $RepoRoot 'tools\generate_fire_gas_opacity_record.py'
$FireGasOpacityManifest = Join-Path $RepoRoot 'tests\fixtures\fire_gas_opacity\synthetic_manifest.json'
$FireGasOpacityTable = Join-Path $RepoRoot 'docs\data\fire_gas_opacity_synthetic_v1.json'
$FireGasOpacityTest = Join-Path $RepoRoot 'tests\test_fire_gas_opacity_tools.py'
$FireGasPlanckGenerator = Join-Path $RepoRoot 'tools\generate_fire_gas_opacity_planck_record.py'
$FireGasPlanckManifest = Join-Path $RepoRoot 'docs\data\gas_opacity\hitemp_sources_v1.json'
$FireGasPlanckEmbedded = Join-Path $RepoRoot 'src\Library\Utilities\FireGasOpacityRecordData.inc'
$FireGasPlanckRecord = Join-Path $RepoRoot 'docs\data\gas_opacity\fire_gas_opacity_hitemp_planck_mean_v1.cbor'
$FireGasPlanckTest = Join-Path $RepoRoot 'tests\test_fire_gas_opacity_planck_record.py'

$python = (Get-Command python3 -ErrorAction SilentlyContinue).Source
if (-not $python) {
    $python = (Get-Command python -ErrorAction SilentlyContinue).Source
}
if (-not $python) {
    Write-Host 'ERROR: Python is required for the fire-optics record parity gate.' -ForegroundColor Red
    exit 1
}
Write-Host -NoNewline 'Checking embedded fire-optics records ... '
& $python $FireOpticsGenerator --check $FireOpticsData $FireOpticsEmbedded
if ($LASTEXITCODE -ne 0) {
    Write-Host 'FAILED' -ForegroundColor Red
    exit $LASTEXITCODE
}
Write-Host 'pass'
Write-Host -NoNewline 'Checking embedded fire-simulation records ... '
& $python $FireSimulationGenerator --check --methane-constants `
    $FireSimulationMethaneConstants $FireSimulationData $FireSimulationEmbedded
if ($LASTEXITCODE -ne 0) {
    Write-Host 'FAILED' -ForegroundColor Red
    exit $LASTEXITCODE
}
Write-Host 'pass'
Write-Host -NoNewline 'Testing physical methane record arithmetic ... '
& $python $FireSimulationGeneratorTest
if ($LASTEXITCODE -ne 0) {
    Write-Host 'FAILED' -ForegroundColor Red
    exit $LASTEXITCODE
}
Write-Host 'pass'
Write-Host -NoNewline 'Checking production HITEMP Planck-mean record ... '
& $python $FireGasPlanckGenerator --check $FireGasPlanckManifest `
    $FireGasPlanckEmbedded --record $FireGasPlanckRecord
if ($LASTEXITCODE -ne 0) {
    Write-Host 'FAILED' -ForegroundColor Red
    exit $LASTEXITCODE
}
Write-Host 'pass'
Write-Host -NoNewline 'Testing production HITEMP Planck-mean tools ... '
& $python $FireGasPlanckTest
if ($LASTEXITCODE -ne 0) {
    Write-Host 'FAILED' -ForegroundColor Red
    exit $LASTEXITCODE
}
Write-Host 'pass'
Write-Host -NoNewline 'Checking quarantined synthetic HITEMP LBL record ... '
& $python $FireGasOpacityGenerator --check $FireGasOpacityManifest $FireGasOpacityTable
if ($LASTEXITCODE -ne 0) {
    Write-Host 'FAILED' -ForegroundColor Red
    exit $LASTEXITCODE
}
Write-Host 'pass'
Write-Host -NoNewline 'Testing quarantined HITEMP LBL tools ... '
& $python $FireGasOpacityTest
if ($LASTEXITCODE -ne 0) {
    Write-Host 'FAILED' -ForegroundColor Red
    exit $LASTEXITCODE
}
Write-Host 'pass'

# Logs go outside the repo to mirror the .sh script's intent.
if (-not $LogDir) {
    if ($env:RISE_TEST_LOG_DIR) {
        $LogDir = $env:RISE_TEST_LOG_DIR
    } else {
        # Per checkout (a hash of its path), so runs in two worktrees at once
        # do not delete each other's logs and run state.
        $sha = [System.Security.Cryptography.SHA1]::Create()
        $repoHash = ([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($RepoRoot))) -replace '-', '').Substring(0, 10)
        $LogDir = Join-Path $env:TEMP "rise-tests-logs-managed-$repoHash"
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

function Get-NormalizedPath([string]$Path) {
    return [IO.Path]::GetFullPath($Path).TrimEnd(
        [IO.Path]::DirectorySeparatorChar,
        [IO.Path]::AltDirectorySeparatorChar)
}

function Test-IsSameOrParent([string]$Parent, [string]$Child) {
    $separator = [IO.Path]::DirectorySeparatorChar
    $parentWithSeparator = (Get-NormalizedPath $Parent) + $separator
    $childWithSeparator = (Get-NormalizedPath $Child) + $separator
    return $childWithSeparator.StartsWith(
        $parentWithSeparator,[StringComparison]::OrdinalIgnoreCase)
}

$normalizedLogDir = Get-NormalizedPath $LogDir
$normalizedRepoRoot = Get-NormalizedPath $RepoRoot
$normalizedUserProfile = Get-NormalizedPath (
    [Environment]::GetFolderPath([Environment+SpecialFolder]::UserProfile))
$normalizedVolumeRoot = Get-NormalizedPath ([IO.Path]::GetPathRoot($normalizedLogDir))
$logLeaf = Split-Path -Leaf $normalizedLogDir
if (($logLeaf -ne 'rise-tests-logs' -and $logLeaf -notlike 'rise-tests-logs-*') -or
    $normalizedLogDir -eq $normalizedVolumeRoot -or
    $normalizedLogDir -eq $normalizedUserProfile -or
    (Test-IsSameOrParent $normalizedLogDir $normalizedRepoRoot) -or
    (Test-IsSameOrParent $normalizedRepoRoot $normalizedLogDir)) {
    Write-Host "ERROR: Refusing unsafe test log directory: $LogDir" -ForegroundColor Red
    Write-Host 'Choose a dedicated directory outside the repository, user profile root, and volume root.'
    exit 1
}
$logMarker = Join-Path $normalizedLogDir '.rise-test-log-directory'
$logDirExists = Test-Path -LiteralPath $normalizedLogDir -PathType Container
$logMarkerExists = Test-Path -LiteralPath $logMarker -PathType Leaf
$logHasEntries = $logDirExists -and [bool](
    Get-ChildItem -LiteralPath $normalizedLogDir -Force | Select-Object -First 1)
if ($logDirExists -and -not $logMarkerExists -and $logHasEntries) {
    Write-Host "ERROR: Refusing unowned nonempty test log directory: $LogDir" -ForegroundColor Red
    exit 1
}
$LogDir = $normalizedLogDir
if ($ValidateLogDirOnly) {
    Write-Host "Safe test log directory: $LogDir"
    exit 0
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

$msbuild = $null
if (-not $NoBuild) {
    $msbuildCommand = Get-Command msbuild.exe -ErrorAction SilentlyContinue
    if ($msbuildCommand) {
        $msbuild = $msbuildCommand.Source
    } else {
        $vswhere = Join-Path ([Environment]::GetFolderPath('ProgramFilesX86')) `
            'Microsoft Visual Studio\Installer\vswhere.exe'
        if (Test-Path -LiteralPath $vswhere) {
            $installationPath = & $vswhere '-latest' '-products' '*' `
                '-requires' 'Microsoft.Component.MSBuild' '-property' 'installationPath' |
                Select-Object -First 1
            if ($installationPath) {
                $candidate = Join-Path $installationPath 'MSBuild\Current\Bin\MSBuild.exe'
                if (Test-Path -LiteralPath $candidate) { $msbuild = $candidate }
            }
        }
    }
    if (-not $msbuild) {
        Write-Host 'ERROR: MSBuild not found; cannot prove RISE.lib is current.' -ForegroundColor Red
        exit 1
    }
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

# One run per checkout at a time: a second run would rebuild and relink the
# test binaries the first is running, write the same scene output, and (with
# the default log dir) delete the first one's logs and per-test TEMP dirs.
# The lock is an exclusively opened file held for the whole run; Windows
# releases it when the process ends, so a crashed run leaves no stale lock.
$lockDir = Join-Path $RepoRoot 'bin'
$null = New-Item -ItemType Directory -Force -Path $lockDir
$lockPath = Join-Path $lockDir '.run_all_tests.lock'
try {
    $lockStream = [System.IO.File]::Open($lockPath, [System.IO.FileMode]::OpenOrCreate, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
} catch [System.IO.IOException] {
    Write-Host "ERROR: another run_all_tests.ps1 is running in this checkout (it holds $lockPath). Wait for it to finish." -ForegroundColor Red
    exit 1
}
$ownsLock = $true
if (Test-Path -LiteralPath $LogDir) { Remove-Item -Recurse -Force -LiteralPath $LogDir }
$null = New-Item -ItemType Directory -Force -Path $LogDir
$null = New-Item -ItemType File -Force -Path (Join-Path $LogDir '.rise-test-log-directory')
$null = New-Item -ItemType Directory -Force -Path $BinDir

# -----------------------------------------------------------------------------
# Phase 0: Build the production library before configuring any test target.
#
# The CMake test projects link the VS Library project's RISE.lib as an imported
# file; CMake therefore cannot discover changes under src/Library by itself.
# An incremental MSBuild here is the authoritative dependency check. A failed
# library build aborts before any stale test executable can run.
# -----------------------------------------------------------------------------

if (-not $NoBuild) {
    $libraryBuildLog = Join-Path $LogDir 'library-build.log'
    Write-Host -NoNewline ("Building RISE.lib [{0}] ... " -f $Config)
    $libraryStart = Get-Date
    & $msbuild $LibraryProject /nologo /m `
        "/p:Configuration=$Config" '/p:Platform=x64' `
        "/p:SolutionDir=$SolutionDir" *>&1 |
        Out-File -FilePath $libraryBuildLog -Encoding utf8
    $libraryBuildRc = $LASTEXITCODE
    $libraryDuration = [int]((Get-Date) - $libraryStart).TotalSeconds
    if ($libraryBuildRc -ne 0 -or -not (Test-Path -LiteralPath $RiseLibrary)) {
        Write-Host ("FAILED (exit={0}, {1}s) - see {2}" -f `
            $libraryBuildRc, $libraryDuration, $libraryBuildLog) -ForegroundColor Red
        if (Test-Path -LiteralPath $libraryBuildLog) {
            Get-Content -LiteralPath $libraryBuildLog -Tail 80 | Write-Host
        }
        exit 1
    }
    Write-Host ("done ({0}s)" -f $libraryDuration)
    Remove-Item -LiteralPath $libraryBuildLog -ErrorAction SilentlyContinue
    Write-Host ""
}

# -----------------------------------------------------------------------------
# Phase 0.5: Configure CMake after the selected RISE.lib exists.
#
# A fresh checkout has no imported library or vcpkg tree. Configuring before
# the production build made the runner unable to bootstrap the state it
# advertised that it created. -NoBuild does not need a CMake tree at all.
# -----------------------------------------------------------------------------

$cacheFile = Join-Path $CmakeBuildDir 'CMakeCache.txt'
if (-not $NoBuild -and -not (Test-Path -LiteralPath $cacheFile)) {
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
$failedTargets = @{}
$failedBuildTargets = @{}   # fire: -NoBuild stale/missing-exe check (shared inputs)

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
            if ($LASTEXITCODE -ne 0) {
                $failedTargets[$src.BaseName] = $true
                $buildRc = 1
            }
        }
    } else {
        & $cmake --build $CmakeBuildDir --config $Config --target rise_all_tests --parallel *>&1 |
            Out-File -FilePath $buildLog -Encoding utf8
        $buildRc = $LASTEXITCODE
        if ($buildRc -ne 0) {
            # Isolate exact failing targets (matching run_all_tests.sh Phase 1.5 -> Phase 2)
            # so header-broken rebuilds whose stale .exe is newer than tests/<Name>.cpp
            # are still recorded in $failedTargets and skipped in Phase 2.
            foreach ($src in $testSources) {
                & $cmake --build $CmakeBuildDir --config $Config --target $src.BaseName *>&1 |
                    Out-File -FilePath $buildLog -Encoding utf8 -Append
                if ($LASTEXITCODE -ne 0) {
                    $failedTargets[$src.BaseName] = $true
                }
            }
            if ($failedTargets.Count -eq 0) {
                # Solution-level build error before per-target invocations succeeded.
                foreach ($src in $testSources) {
                    $failedTargets[$src.BaseName] = $true
                }
            }
        }
    }
    $bd = [int]((Get-Date) - $bs).TotalSeconds

    # Walk the test list and check which exes were produced. Source-newer-than-exe
    # or an explicit per-target cmake failure signals a broken rebuild even when a
    # header-only edit left a stale binary newer than its test .cpp.
    foreach ($src in $testSources) {
        $exe = Join-Path $BinDir "$($src.BaseName).exe"
        if ((-not $failedTargets.ContainsKey($src.BaseName)) -and `
            (Test-Path -LiteralPath $exe) -and `
            ((Get-Item -LiteralPath $exe).LastWriteTime -ge $src.LastWriteTime)) {
            $built++
        } else {
            $buildFailed++
            $buildFailures += $src.BaseName
        }
    }

    if ($buildRc -eq 0 -and $buildFailed -eq 0) {
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
    # -NoBuild is still fail-closed: every selected binary must exist and be
    # at least as new as its own source, the shared test headers, and the
    # selected production library. It never turns a missing/stale suite into
    # a successful zero-test run.
    $productionInputs = Get-ChildItem -Path (Join-Path $RepoRoot 'src\Library') `
        -Recurse -File -Include '*.cpp','*.h','*.inc'
    $productionInputs += Get-Item -LiteralPath $LibraryProject
    $latestProductionInput = $productionInputs |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    $libraryCurrent = $false
    if ((Test-Path -LiteralPath $RiseLibrary) -and
        $null -ne $latestProductionInput) {
        $libraryCurrent = (Get-Item -LiteralPath $RiseLibrary).LastWriteTime -ge
            $latestProductionInput.LastWriteTime
    }
    $sharedInputs = Get-ChildItem -Path $SrcDir -Filter '*.h' -File |
        ForEach-Object { $_.FullName }
    foreach ($src in $testSources) {
        $exe = Join-Path $BinDir "$($src.BaseName).exe"
        $invalid = -not (Test-Path -LiteralPath $exe)
        if (-not $libraryCurrent) { $invalid = $true }
        if (-not $invalid) {
            $exeTime = (Get-Item -LiteralPath $exe).LastWriteTime
            if ($exeTime -lt $src.LastWriteTime) { $invalid = $true }
            if ($exeTime -lt (Get-Item -LiteralPath $RiseLibrary).LastWriteTime) {
                $invalid = $true
            }
            foreach ($inputPath in $sharedInputs) {
                if (-not (Test-Path -LiteralPath $inputPath) -or
                    $exeTime -lt (Get-Item -LiteralPath $inputPath).LastWriteTime) {
                    $invalid = $true
                    break
                }
            }
        }
        if ($invalid) {
            $buildFailed++
            $buildFailures += $src.BaseName
            $failedBuildTargets[$src.BaseName] = 1
        } else {
            $built++
        }
    }
    Write-Host ("Skipping build (-NoBuild); {0} current, {1} missing/stale test exe(s)." `
        -f $built, $buildFailed)
    Write-Host ""
}

if ($BuildOnly) {
    if ($buildFailed -gt 0 -or $buildRc -ne 0) { exit 1 } else { exit 0 }
}

# -----------------------------------------------------------------------------
# Phase 2: Run the built tests, up to $Jobs at a time, with captured output
# and an optional timeout.
#
# Tests share $Jobs run slots.  tests\test_schedule.tsv (shared with
# run_all_tests.sh) names the tests that must run ALONE (`exclusive`:
# wall-clock gates), the tests that take N slots (`wide:N`: multi-core
# renders), the tests that take one slot with their render threads forced to
# one (`single`), and a run time for the long ones.  Exclusive tests run first,
# one at a time on an otherwise idle machine.  The wide tests then start in
# order, widest first, while the pool and single tests fill the slots the next
# wide test cannot use, longest first by the recorded times
# (bin\.test_timings.tsv, else the schedule's times).  Unlisted tests are
# `pool` (one slot).  -Jobs 1 runs one test at a time in alphabetical order
# (single tests still get one render thread).
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
        if ($cls -cne 'pool' -and $cls -cne 'exclusive' -and $cls -cne 'single' -and $cls -cnotmatch '^wide:[1-9][0-9]*$') {
            Write-Host ('warning: tests\test_schedule.tsv: unknown class "{0}" for {1}; running it exclusive' -f $cls, $cols[0])
            $cls = 'exclusive'
        }
        $sec = 0.0
        if ($cols.Count -ge 3) { [void][double]::TryParse($cols[2], [ref]$sec) }
        $schedule[$cols[0]] = [pscustomobject]@{ Class = $cls; Seconds = $sec }
    }
}

# Each test's time in the last run that recorded it (name<TAB>seconds; see
# the end of this script for which runs do), kept outside the log dir, which
# every run deletes; it orders the next run.
$timingsHistory = Join-Path $RepoRoot 'bin\.test_timings.tsv'
$lastSeconds = @{}
if (Test-Path -LiteralPath $timingsHistory) {
    foreach ($line in Get-Content -LiteralPath $timingsHistory) {
        $cols = $line.TrimEnd("`r") -split "`t"
        $sec = 0
        if ($cols.Count -ge 2 -and $cols[0] -and [int]::TryParse($cols[1], [ref]$sec)) { $lastSeconds[$cols[0]] = $sec }
    }
}

# The run slots a test takes: exclusive all of them, wide:N N (at most $Jobs),
# pool and single 1.
function Get-RiseTestSlots([string]$class) {
    if ($class -ceq 'exclusive') { return $Jobs }
    if ($class -cmatch '^wide:([1-9][0-9]*)$') {
        $n = 0
        if (-not [int]::TryParse($Matches[1], [ref]$n)) { return $Jobs }
        return [Math]::Min($n, $Jobs)
    }
    return 1
}

$plan = foreach ($src in $testSources) {
    $entry = $schedule[$src.BaseName]
    $cls = if ($entry) { $entry.Class } else { 'pool' }
    [pscustomobject]@{
        Name    = $src.BaseName
        Src     = $src
        Class   = $cls
        Slots   = Get-RiseTestSlots $cls
        Group   = if ($Jobs -le 1) { 2 } elseif ($cls -ceq 'exclusive') { 0 } elseif ($cls -cmatch '^wide:') { 1 } else { 2 }
        Seconds = if ($lastSeconds.ContainsKey($src.BaseName)) { [double]$lastSeconds[$src.BaseName] } elseif ($entry) { $entry.Seconds } else { 0.0 }
    }
}
if ($Jobs -gt 1) {
    # Exclusive tests longest first, then the wide tests widest first, then
    # the pool and single tests longest first.
    $plan = $plan | Sort-Object -Property @{ Expression = 'Group' }, @{ Expression = 'Slots'; Descending = $true }, @{ Expression = 'Seconds'; Descending = $true }, @{ Expression = 'Name' }
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

Write-Host ('Running {0} test(s) in {1} run slots (exclusive tests alone)' -f $plan.Count, $Jobs)
$runStart = Get-Date
$doneCount = 0
$running = New-Object System.Collections.ArrayList
$slotsUsed = 0
# The plan's three runs: exclusive, wide, then pool and single (all in the
# last run with -Jobs 1).
# (Element by element: @( @(a), @(b) ) would flatten into one array.)
$queues = New-Object 'object[]' 3
foreach ($g in 0..2) { $queues[$g] = @($plan | Where-Object { $_.Group -eq $g }) }
$nextIn = @(0, 0, 0)

# Start the next test of queue $q if it fits in the free slots (or skip it).
# Returns $true when that test is used up (started or skipped), $false when
# it must wait for slots.  On an idle machine every test fits: slots never
# exceed $Jobs.
function Start-NextRiseTest([int]$q) {
    $item = $script:queues[$q][$script:nextIn[$q]]
    $name = $item.Name
    $exe = Join-Path $BinDir "$name.exe"
    $doneSkip = $null
    if (-not $NoBuild -and $failedTargets.ContainsKey($name)) {
        $doneSkip = 'SKIP (build failed)'
    } elseif (-not (Test-Path -LiteralPath $exe)) {
        $doneSkip = 'SKIP (build failed or missing)'
    } elseif ($failedBuildTargets.ContainsKey($name)) {
        $doneSkip = 'SKIP (current build failed; stale exe ignored)'
    } elseif (-not $NoBuild -and (Get-Item -LiteralPath $exe).LastWriteTime -lt $item.Src.LastWriteTime) {
        # Source newer than exe means a recent build attempt failed.
        $doneSkip = 'SKIP (build failed, exe is stale)'
    }
    if ($doneSkip) {
        $script:doneCount++
        Write-Host ('[ {0,3}/{1,3} ] {2,-46} {3}' -f $script:doneCount, $total, $name, $doneSkip)
        $script:skipped++
        $script:nextIn[$q]++
        return $true
    }
    if ($script:running.Count -gt 0 -and ($script:slotsUsed + $item.Slots) -gt $Jobs) { return $false }
    $script:found++
    [void]$script:running.Add((Start-RiseTest $item))
    $script:slotsUsed += $item.Slots
    $script:nextIn[$q]++
    return $true
}

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
            $slotsUsed -= $run.Item.Slots
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

        # Start what the slots allow.  Exclusive tests come first, one at a
        # time on an idle machine; nothing else starts until they are done.
        # Then the wide tests in plan order (widest first), while the pool and
        # single tests fill whatever slots the next wide test cannot use.
        while ($nextIn[0] -lt $queues[0].Count -and $running.Count -eq 0) {
            if (-not (Start-NextRiseTest 0)) { break }
        }
        if ($nextIn[0] -ge $queues[0].Count) {
            while ($nextIn[1] -lt $queues[1].Count -and $slotsUsed -lt $Jobs) {
                if (-not (Start-NextRiseTest 1)) { break }
            }
            while ($nextIn[2] -lt $queues[2].Count -and $slotsUsed -lt $Jobs) {
                if (-not (Start-NextRiseTest 2)) { break }
            }
        }

        if ($running.Count -eq 0 -and $nextIn[0] -ge $queues[0].Count -and $nextIn[1] -ge $queues[1].Count -and $nextIn[2] -ge $queues[2].Count) { break }
        Start-Sleep -Milliseconds 200
    }
} finally {
    # Ctrl-C or an error: do not leave test processes behind.
    foreach ($run in @($running)) {
        if (-not $run.LaunchError) { Stop-RiseTestTree $run.Proc }
    }
}
$runWall = [int]((Get-Date) - $runStart).TotalSeconds

# Remember each test's time for the next run's order: this run's times replace
# the old ones, tests this run did not reach keep theirs, and tests whose
# source is gone are dropped.  Only passing tests of an unfiltered full-tier
# run (RISE_TEST_EXTENDED=1) on several slots count: a failure can stop early,
# the fast tier runs cheaper modes, and with -Jobs 1 or a filter a test has few
# or no neighbours.  An interrupted run writes nothing.  LF line endings, so
# run_all_tests.sh in the same checkout reads it as well.
if ($timings.Count -gt 0 -and $Tier -eq 'all' -and $env:RISE_TEST_EXTENDED -eq '1' -and
    -not $Filter -and $Jobs -gt 1) {
    $failedNames = @{}
    foreach ($f in $runFailures) { $failedNames[$f.Name] = $true }
    foreach ($t in $timings) {
        if (-not $failedNames.ContainsKey($t.Name)) { $lastSeconds[$t.Name] = [int]$t.Seconds }
    }
    $existing = @{}
    foreach ($src in (Get-ChildItem -LiteralPath $SrcDir -Filter '*.cpp' -File)) { $existing[$src.BaseName] = $true }
    $historyLines = foreach ($k in ($lastSeconds.Keys | Sort-Object)) {
        if ($existing.ContainsKey($k)) { "$k`t$($lastSeconds[$k])" }
    }
    $historyTmp = "$timingsHistory.tmp.$PID"
    try {
        $null = New-Item -ItemType Directory -Force -Path (Split-Path -Parent $timingsHistory)
        [IO.File]::WriteAllText($historyTmp, ((@($historyLines) -join "`n") + "`n"))
        Move-Item -Force -LiteralPath $historyTmp -Destination $timingsHistory
    } catch {
        Remove-Item -Force -LiteralPath $historyTmp -ErrorAction SilentlyContinue
        Write-Host "warning: could not update $timingsHistory"
    }
}

# The r136-r175 historical evidence replays that ran here were retired on
# 2026-11-14 (run_all_tests.sh already dropped them): each bound milestone
# evidence to source and grid sizes that later commits intentionally changed
# (see docs/FIRE_SMOKE_STATUS.md, "Retired test gates").

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

# A test that keeps more cores busy than the slots it holds competes with its
# neighbours, so flag it for a wider `wide:N` (or `exclusive`) entry.  Only
# meaningful when tests ran side by side.
if ($Jobs -gt 1) {
    $busy = @($cpuRows | Where-Object { $_.Class -cne 'exclusive' -and $_.Seconds -ge 20 -and $_.Cores -ge ((Get-RiseTestSlots $_.Class) + 0.6) })
    if ($busy.Count -gt 0) {
        Write-Host ''
        Write-Host 'Tests that kept more cores busy than their run slots (give them wide:N in tests\test_schedule.tsv):'
        foreach ($b in $busy) { Write-Host ('  {0,-46} {1,-7} {2,5:N1} cores over {3}s' -f $b.Name, $b.Class, $b.Cores, $b.Seconds) }
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
# changed header or library) ran green -- as run_all_tests.sh does; so does a
# skipped or missing test.
if ($failed -ne 0 -or $buildFailed -ne 0 -or $buildRc -ne 0 -or $skipped -ne 0 -or $found -ne $total) {
    if ($buildRc -ne 0 -and $failed -eq 0 -and $buildFailed -eq 0) {
        Write-Host "Build step failed (exit=$buildRc); see the build log"
    }
    exit 1
}
Write-Host "All $found tests passed"
exit 0

} finally {
    if ($ownsLock) {
        $lockStream.Dispose()
        Remove-Item -Force -LiteralPath $lockPath -ErrorAction SilentlyContinue
    }
    Pop-Location -StackName RiseRunAllTests -ErrorAction SilentlyContinue
    $env:RISE_OPTIONS_FILE = $origRiseOptionsFile
    $env:RISE_TEST_EXTENDED = $origRiseTestExtended
    $env:TMPDIR = $origTmpDir
    $env:RISE_MEDIA_PATH = $origRiseMediaPath
}
