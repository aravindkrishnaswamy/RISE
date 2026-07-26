#!/usr/bin/env bash
##############################################################################
#
#  fetch_prebuilt.sh - Install Intel's official Open Image Denoise binary
#  release into extlib/oidn/install/.
#
#  This is the alternative to build.sh.  Both produce the same layout and
#  RISE's build configs cannot tell them apart; pick whichever suits the
#  machine:
#
#    build.sh          Builds the pinned extlib/oidn/source submodule from
#                      source with CPU + Metal devices.  Needs cmake, ispc,
#                      tbb, AND -- on Xcode 26 and newer -- the separately
#                      downloaded Metal Toolchain component:
#                          xcodebuild -downloadComponent MetalToolchain
#                      Without it the Metal kernels fail to compile and you
#                      silently end up with a CPU-only install.
#
#    fetch_prebuilt.sh (this script) Downloads the official binary release
#                      for the same version, which already ships
#                      libOpenImageDenoise_device_metal.  No Metal Toolchain
#                      and no ~2 GB Xcode component download required.
#
#  Why this matters: the Homebrew bottle is built CPU-device-only, so an
#  app linked against it silently denoises on the CPU even on a
#  Metal-capable Mac.  See docs/OIDN.md (OIDN-P0-3) for the full gotcha.
#
#  The download is verified against a pinned SHA-256 before anything is
#  unpacked, and the pinned version is cross-checked against the
#  extlib/oidn/source submodule tag so the binary and the source tree can
#  never drift apart unnoticed.
#
##############################################################################

set -euo pipefail

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
SOURCE_DIR="${SCRIPT_DIR}/source"
INSTALL_DIR="${SCRIPT_DIR}/install"

# Pinned to the same release as the extlib/oidn/source submodule. Bump both
# together; the checksums come from the release's published digests
# (gh api repos/RenderKit/oidn/releases/tags/v<VERSION>).
OIDN_VERSION="2.4.1"
SHA256_arm64_macos="f1d7370bc09242bbd72d405b424ba240fd4d64103f3e607cdfeeaa2f2718cfb8"
SHA256_x86_64_macos="b9addf2855ee36d7768fd02d4d540e64612096487bad302e608d04e639ae1584"

die() {
	echo "ERROR: $*" >&2
	exit 1
}

[ "$(uname -s)" = "Darwin" ] || \
	die "this script packages the macOS binary release; use build.sh on other platforms"

case "$(uname -m)" in
	arm64)  ARCH_TAG="arm64";  EXPECTED_SHA256="${SHA256_arm64_macos}" ;;
	x86_64) ARCH_TAG="x86_64"; EXPECTED_SHA256="${SHA256_x86_64_macos}" ;;
	*)      die "unsupported architecture: $(uname -m)" ;;
esac

for tool in curl shasum tar; do
	command -v "${tool}" >/dev/null 2>&1 || die "required command not found: ${tool}"
done

# Refuse to install a binary that does not correspond to the source
# revision the repository pins, so the two can never silently diverge.
if [ -d "${SOURCE_DIR}/.git" ] || [ -f "${SOURCE_DIR}/.git" ]; then
	SUBMODULE_TAG="$(git -C "${SOURCE_DIR}" describe --tags --exact-match 2>/dev/null || true)"
	if [ -n "${SUBMODULE_TAG}" ] && [ "${SUBMODULE_TAG}" != "v${OIDN_VERSION}" ]; then
		die "extlib/oidn/source is pinned to ${SUBMODULE_TAG} but this script installs v${OIDN_VERSION}; update both together"
	fi
fi

TARBALL="oidn-${OIDN_VERSION}.${ARCH_TAG}.macos.tar.gz"
URL="https://github.com/RenderKit/oidn/releases/download/v${OIDN_VERSION}/${TARBALL}"
WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/rise-oidn-fetch.XXXXXX")"
trap 'rm -rf "${WORK_DIR}"' EXIT

echo "==> Downloading ${TARBALL}"
curl -fsSL -o "${WORK_DIR}/${TARBALL}" "${URL}"

echo "==> Verifying SHA-256"
echo "${EXPECTED_SHA256}  ${WORK_DIR}/${TARBALL}" | shasum -a 256 -c - >/dev/null || \
	die "checksum mismatch for ${TARBALL}; refusing to unpack"

echo "==> Unpacking to ${INSTALL_DIR}"
rm -rf "${INSTALL_DIR}"
mkdir -p "${INSTALL_DIR}"
tar xzf "${WORK_DIR}/${TARBALL}" -C "${INSTALL_DIR}" --strip-components=1

# The whole point of not using the Homebrew bottle: confirm the GPU device
# module is actually here rather than discovering it at render time.
METAL_DEVICE="$(find "${INSTALL_DIR}/lib" -maxdepth 1 -name 'libOpenImageDenoise_device_metal*.dylib' -print -quit 2>/dev/null || true)"
[ -n "${METAL_DEVICE}" ] || \
	die "install completed but no Metal device module is present under ${INSTALL_DIR}/lib"
[ -f "${INSTALL_DIR}/lib/libOpenImageDenoise.dylib" ] || \
	die "install completed but ${INSTALL_DIR}/lib/libOpenImageDenoise.dylib is missing"
[ -f "${INSTALL_DIR}/include/OpenImageDenoise/oidn.hpp" ] || \
	die "install completed but the OIDN headers are missing"

echo
echo "==> Done.  OIDN install tree:"
ls -la "${INSTALL_DIR}/lib"
echo
echo "Metal device module: $(basename "${METAL_DEVICE}")"
echo
echo "RISE's build configs (build/make/rise/Config.OSX, the Xcode project,"
echo "the VS2022 projects and the rise-tests CMakeLists) prefer this install"
echo "over any system OIDN.  Verify at render time by looking for:"
echo "    OIDN: creating Metal device (one-time per rasterizer)"
