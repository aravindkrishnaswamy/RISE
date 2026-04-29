#!/usr/bin/env bash
##############################################################################
#
#  build.sh - Build Intel Open Image Denoise from source for RISE.
#
#  Builds the submoduled OIDN (extlib/oidn/source) into a self-contained
#  install at extlib/oidn/install/.  Configures CPU device on every
#  platform plus Metal on macOS, so denoise can actually run on the GPU
#  (the Homebrew formula doesn't ship the Metal device dylib — see
#  docs/OIDN.md OIDN-P0-3 for the install-gotcha).
#
#  RISE's build configs prefer extlib/oidn/install/ when present and fall
#  back to the system OIDN otherwise, so this script is opt-in: run it
#  once after `git submodule update --init`, and rebuild whenever the
#  submodule is bumped.
#
#  Prerequisites (macOS):
#    brew install cmake ispc tbb
#  Plus Xcode CLT for the system Metal SDK.
#
##############################################################################

set -euo pipefail

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
SOURCE_DIR="${SCRIPT_DIR}/source"
BUILD_DIR="${SOURCE_DIR}/build"
INSTALL_DIR="${SCRIPT_DIR}/install"

if [ ! -d "${SOURCE_DIR}" ]; then
	echo "ERROR: OIDN source not found at ${SOURCE_DIR}"
	echo "Run from the RISE repo root:"
	echo "    git submodule update --init extlib/oidn/source"
	exit 1
fi

# Make sure the trained-weights submodule is checked out — every device
# backend needs them at runtime.  We skip cutlass / composable_kernel
# (CUDA / HIP) since RISE only builds CPU + Metal on macOS; other
# platforms can extend this script later.
echo "==> Initialising required OIDN sub-submodules (weights)..."
( cd "${SOURCE_DIR}" && git submodule update --init weights )

# Platform-specific flags.  Default to CPU; macOS additionally gets
# Metal.  Linux/Windows can flip in CUDA / SYCL / HIP via the same
# pattern when needed.
EXTRA_FLAGS=()
case "$(uname -s)" in
	Darwin)
		EXTRA_FLAGS+=( "-DOIDN_DEVICE_METAL=ON" )
		;;
esac

# Tooling overrides.  ispc may live in /opt/homebrew/bin which CMake
# usually finds via PATH, but flag explicitly so a missing brew install
# fails loudly with a clear message instead of building a CPU-deviceless
# OIDN.
ISPC_EXEC="$(command -v ispc || true)"
if [ -z "${ISPC_EXEC}" ]; then
	echo "ERROR: ispc not found on PATH.  Install with: brew install ispc"
	exit 1
fi

mkdir -p "${BUILD_DIR}"

echo "==> Configuring OIDN (build dir: ${BUILD_DIR})..."
cmake -S "${SOURCE_DIR}" -B "${BUILD_DIR}" \
	-DCMAKE_INSTALL_PREFIX="${INSTALL_DIR}" \
	-DCMAKE_BUILD_TYPE=Release \
	-DOIDN_DEVICE_CPU=ON \
	-DOIDN_APPS=OFF \
	-DOIDN_ISPC_EXECUTABLE="${ISPC_EXEC}" \
	"${EXTRA_FLAGS[@]}"

# Parallel build.  sysctl on macOS, nproc elsewhere.
JOBS="$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)"

echo "==> Building OIDN (-j${JOBS})..."
cmake --build "${BUILD_DIR}" --config Release -j"${JOBS}"

echo "==> Installing to ${INSTALL_DIR}..."
cmake --install "${BUILD_DIR}" --config Release

echo
echo "==> Done.  OIDN install tree:"
ls -la "${INSTALL_DIR}/lib" 2>/dev/null || true
echo
echo "RISE's build configs (build/make/rise/Makefile etc.) will pick this"
echo "up automatically.  Verify Metal at render time by looking for:"
echo "    OIDN: creating Metal device (one-time per rasterizer)"
