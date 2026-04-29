<#
.SYNOPSIS
    Build Intel Open Image Denoise from source for RISE on Windows.

.DESCRIPTION
    Mirror of build.sh for Windows.  Builds the submoduled OIDN
    (extlib/oidn/source) into a self-contained install at
    extlib/oidn/install/ with CPU device on every machine, plus
    optional CUDA / HIP / SYCL when the user passes -EnableCuda,
    -EnableHip, or -EnableSycl.  RISE's vcxproj files prefer this
    install over the user's hand-installed OIDN.

    Prerequisites:
      - Visual Studio 2022 with C++ workload + CMake
      - ispc on PATH
        (download from https://github.com/ispc/ispc/releases or
        `winget install Intel.ImplicitSPMDProgramCompiler`)
      - For CUDA backend: CUDA Toolkit 11+ on PATH (nvcc.exe)
      - For SYCL backend: oneAPI Base Toolkit (icx, dpcpp)
      - For HIP backend: ROCm SDK

    Run from repo root:
        pwsh -File extlib\oidn\build.ps1
        pwsh -File extlib\oidn\build.ps1 -EnableCuda
        pwsh -File extlib\oidn\build.ps1 -Clean

.PARAMETER Clean
    Remove the build directory before configuring.

.PARAMETER EnableCuda
    Build the CUDA device backend (NVIDIA GPUs).

.PARAMETER EnableHip
    Build the HIP device backend (AMD GPUs).

.PARAMETER EnableSycl
    Build the SYCL device backend (Intel Arc + cross-vendor).
#>

[CmdletBinding()]
param(
	[switch]$Clean,
	[switch]$EnableCuda,
	[switch]$EnableHip,
	[switch]$EnableSycl
)

$ErrorActionPreference = 'Stop'

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$SourceDir = Join-Path $ScriptDir 'source'
$BuildDir  = Join-Path $SourceDir 'build'
$InstallDir = Join-Path $ScriptDir 'install'

# RISE repo root is two levels up from extlib/oidn/.  Used for the
# bin/ + dbin/ runtime-DLL staging step at the end of this script.
$RiseRoot = Resolve-Path (Join-Path $ScriptDir '..\..')

if (-not (Test-Path $SourceDir)) {
	Write-Error "OIDN source not found at $SourceDir.`nRun from repo root: git submodule update --init extlib/oidn/source"
}

# Required: trained-weights submodule.  Skip cutlass / composable_kernel
# unless the user opted in to CUDA / HIP.
Write-Host '==> Initialising OIDN sub-submodules...'
Push-Location $SourceDir
try {
	& git submodule update --init weights
	if ($EnableCuda) { & git submodule update --init external/cutlass }
	if ($EnableHip)  { & git submodule update --init external/composable_kernel }
}
finally {
	Pop-Location
}

# Sanity-check ispc.
$ispcExec = (Get-Command ispc -ErrorAction SilentlyContinue).Source
if (-not $ispcExec) {
	Write-Error "ispc not found on PATH.  Install via:`n    winget install Intel.ImplicitSPMDProgramCompiler"
}

if ($Clean -and (Test-Path $BuildDir)) {
	Write-Host "==> Cleaning $BuildDir..."
	Remove-Item -Recurse -Force $BuildDir
}

New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null

# Assemble device-backend flags.  CPU is always on; everything else
# is opt-in.  Default `Default` device on Windows will pick whichever
# GPU backend was actually compiled in.
$DeviceFlags = @('-DOIDN_DEVICE_CPU=ON')
if ($EnableCuda) { $DeviceFlags += '-DOIDN_DEVICE_CUDA=ON' }
if ($EnableHip)  { $DeviceFlags += '-DOIDN_DEVICE_HIP=ON' }
if ($EnableSycl) { $DeviceFlags += '-DOIDN_DEVICE_SYCL=ON' }

Write-Host "==> Configuring OIDN (build dir: $BuildDir)..."
# Omit -G so cmake auto-detects the latest installed Visual Studio
# (VS17/VS18/...).  Hardcoding "Visual Studio 17 2022" failed on
# machines that only have a newer VS where the v143 toolset is
# absent (`MSB8020: build tools for v143 cannot be found`).
$configureArgs = @(
	'-S', $SourceDir,
	'-B', $BuildDir,
	'-A', 'x64',
	"-DCMAKE_INSTALL_PREFIX=$InstallDir",
	'-DOIDN_APPS=OFF',
	"-DOIDN_ISPC_EXECUTABLE=$ispcExec"
) + $DeviceFlags

& cmake @configureArgs
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed (exit $LASTEXITCODE)" }

Write-Host '==> Building OIDN (Release)...'
& cmake --build $BuildDir --config Release --parallel
if ($LASTEXITCODE -ne 0) { throw "cmake build failed (exit $LASTEXITCODE)" }

Write-Host "==> Installing to $InstallDir..."
& cmake --install $BuildDir --config Release
if ($LASTEXITCODE -ne 0) { throw "cmake install failed (exit $LASTEXITCODE)" }

# Stage runtime DLLs into bin/ and dbin/.  The VS projects link
# against the new import lib at extlib/oidn/install/lib but Windows
# DLL search starts in the EXE directory at runtime.  If bin/ still
# holds an older OIDN DLL with a different ABI (different MSVC / TBB
# / ispc build of the same OIDN version), the loader picks that one
# and you get a heap-corruption crash deep inside ucrtbase the first
# time a denoise runs.  Hit on a clean migration from system OIDN to
# the in-tree build (2026-04-29).
Write-Host '==> Staging OIDN DLLs to bin/ and dbin/...'
$tbbRedist = if ($env:TBB_ROOT) { Join-Path $env:TBB_ROOT 'redist\intel64\vc14' } else { $null }
foreach ($entry in @(
	@{ Dest = 'bin';  TbbPattern = @('tbb12.dll',       'tbbbind.dll',       'tbbbind_2_0.dll',       'tbbbind_2_5.dll'      ) },
	@{ Dest = 'dbin'; TbbPattern = @('tbb12_debug.dll', 'tbbbind_debug.dll', 'tbbbind_2_0_debug.dll', 'tbbbind_2_5_debug.dll') }
)) {
	$destDir = Join-Path $RiseRoot $entry.Dest
	if (-not (Test-Path $destDir)) {
		New-Item -ItemType Directory -Force -Path $destDir | Out-Null
	}

	$oidnBin = Join-Path $InstallDir 'bin'
	if (Test-Path $oidnBin) {
		Get-ChildItem -Path $oidnBin -Filter 'OpenImageDenoise*.dll' -ErrorAction SilentlyContinue |
			ForEach-Object { Copy-Item -Force -Path $_.FullName -Destination $destDir }
	}

	# Stage only the TBB DLLs OIDN actually depends on.  Critically,
	# do NOT stage `tbbmalloc*.dll` — `tbbmalloc_proxy.dll` is a
	# malloc-replacement DLL that, if loaded into the process, hijacks
	# every malloc/free in every loaded module.  When mixed with Qt's
	# heap allocations that's a recipe for heap-corruption crashes
	# deep in QClipData / QPainter (observed 2026-04-29).  OIDN imports
	# only `tbb12.dll`; the `tbbbind*.dll` family is dynamically
	# loaded by tbb12 for NUMA-aware thread binding (optional but
	# improves perf — small file, worth shipping).  bin/ gets release
	# variants, dbin/ gets the `_debug.dll` counterparts.
	if ($tbbRedist -and (Test-Path $tbbRedist)) {
		foreach ($name in $entry.TbbPattern) {
			$src = Join-Path $tbbRedist $name
			if (Test-Path $src) {
				Copy-Item -Force -Path $src -Destination $destDir
			}
		}
	}
}

Write-Host
Write-Host '==> Done.  OIDN install tree:'
Get-ChildItem -Path (Join-Path $InstallDir 'lib') -ErrorAction SilentlyContinue | Format-Table -AutoSize
Write-Host
Write-Host 'RISE''s VS2022 projects (build/VS2022/*.vcxproj) link against'
Write-Host 'extlib/oidn/install/lib at compile time; the matching DLLs are'
Write-Host 'now staged in bin/ and dbin/ for runtime.  Verify the selected'
Write-Host 'device at render time:'
Write-Host '    OIDN: creating <Type> device (one-time per rasterizer)'
