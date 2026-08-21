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
    [switch]$ValidateLogDirOnly,
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
$FireProductionCalibrationDir = Join-Path $RepoRoot 'rendered\fire_production_calibration\r112_dyadic_smooth_open'
$FireProductionProtocolSHA = '42185c882c52e8c94db4b58f40674c53341eabe1b75b6922fdd1c7f56415a4ed'
$FireProductionTargetsSHA = 'd4947cb8eedbc57732190bf1833e68c3f83a356346c1662db321d7831bce958b'

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
        $LogDir = Join-Path $env:TEMP 'rise-tests-logs-managed'
    }
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
$failedBuildTargets = @{}

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
            $targetRc = $LASTEXITCODE
            if ($targetRc -ne 0) {
                $failedBuildTargets[$src.BaseName] = $targetRc
            }
        }
        $buildRc = if ($failedBuildTargets.Count -eq 0) { 0 } else { 1 }
    } else {
        & $cmake --build $CmakeBuildDir --config $Config --target rise_all_tests --parallel *>&1 |
            Out-File -FilePath $buildLog -Encoding utf8
        $buildRc = $LASTEXITCODE
        if ($buildRc -ne 0) {
            # The aggregate target cannot identify which stale executable owns
            # the failure. Re-run every target through CMake so its dependency
            # graph (including shared test headers and RISE.lib) determines the
            # result; never infer freshness from source/exe timestamps.
            foreach ($src in $testSources) {
                & $cmake --build $CmakeBuildDir --config $Config --target $src.BaseName *>&1 |
                    Out-File -FilePath $buildLog -Encoding utf8 -Append
                $targetRc = $LASTEXITCODE
                if ($targetRc -ne 0) {
                    $failedBuildTargets[$src.BaseName] = $targetRc
                }
            }
            $buildRc = if ($failedBuildTargets.Count -eq 0) { 0 } else { 1 }
        }
    }
    $bd = [int]((Get-Date) - $bs).TotalSeconds

    # Every nonzero target build is authoritative even if an older executable
    # remains on disk. Successful aggregate/individual CMake builds already
    # evaluated the complete dependency graph.
    foreach ($src in $testSources) {
        $exe = Join-Path $BinDir "$($src.BaseName).exe"
        if ($failedBuildTargets.ContainsKey($src.BaseName)) {
            $buildFailed++
            $buildFailures += $src.BaseName
        } elseif (Test-Path -LiteralPath $exe) {
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
    if ($failedBuildTargets.ContainsKey($name)) {
        Write-Host "$prefix SKIP (current build failed; stale exe ignored)"
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

if (-not $Filter) {
    $roundoffName = 'FireProductionCalibrationOracle.r130'
    $roundoffExe = Join-Path $BinDir 'FireSequenceTest.exe'
    $roundoffLog = Join-Path $LogDir "$roundoffName.log"
    Write-Host -NoNewline ('[ evidence ] {0,-46} ... ' -f $roundoffName)
    if (-not (Test-Path -LiteralPath $roundoffExe)) {
        $roundoffRC = 127
    } else {
        & $roundoffExe --fire-production-calibration-diagnose-roundoff `
            $FireProductionCalibrationDir $FireProductionProtocolSHA `
            $FireProductionTargetsSHA *>&1 | Out-File -FilePath $roundoffLog -Encoding utf8
        $roundoffRC = $LASTEXITCODE
    }
    if ($roundoffRC -eq 237) {
        Write-Host 'PASS (exact exit=237)'
        Remove-Item -LiteralPath $roundoffLog -ErrorAction SilentlyContinue
    } else {
        Write-Host ("FAIL (exit={0}; expected 237)" -f $roundoffRC)
        $runFailures += [pscustomobject]@{ Name = $roundoffName; Code = $roundoffRC;
            Log = $roundoffLog; Reason = 'fail' }
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

if ($failed -ne 0 -or $buildFailed -ne 0 -or $skipped -ne 0 -or $found -ne $total) {
    exit 1
}
Write-Host "All $found tests passed"
exit 0
