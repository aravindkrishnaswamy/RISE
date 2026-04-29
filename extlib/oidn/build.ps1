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
$configureArgs = @(
	'-S', $SourceDir,
	'-B', $BuildDir,
	'-G', 'Visual Studio 17 2022',
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

Write-Host
Write-Host '==> Done.  OIDN install tree:'
Get-ChildItem -Path (Join-Path $InstallDir 'lib') -ErrorAction SilentlyContinue | Format-Table -AutoSize
Write-Host
Write-Host 'RISE''s VS2022 projects (build/VS2022/*.vcxproj) will pick this'
Write-Host 'up automatically.  Verify the selected device at render time:'
Write-Host '    OIDN: creating <Type> device (one-time per rasterizer)'
