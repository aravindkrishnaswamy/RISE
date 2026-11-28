#!/usr/bin/env bash
#
# Build a clean, self-contained RISE macOS Opto release and package it in a
# drag-to-Applications DMG. External dylibs are copied into the app bundle and
# relinked so the result does not depend on Homebrew being installed.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
SCHEME="RISE-GUI-Opto"
CONFIGURATION="Opto"

VERSION=""
BUILD_NUMBER=""
OUTPUT_DIR="${REPO_ROOT}/dist/macos"
SIGN_IDENTITY="${RISE_CODESIGN_IDENTITY:--}"
NOTARY_PROFILE="${RISE_NOTARY_PROFILE:-}"
KEEP_WORK_DIR=0
OVERWRITE=0

usage() {
	cat <<'EOF'
Usage: scripts/create_macos_release.sh [options]

Build a fresh RISE-GUI Opto app, bundle all non-system dylibs, sign it, and
create a compressed DMG containing RISE-GUI.app and an Applications shortcut.

Options:
  --version VERSION       Override the RISE library and bundle version.
  --build NUMBER          Override the RISE library and bundle build number.
  --output-dir PATH       Artifact directory (default: dist/macos).
  --sign-identity NAME    Developer ID Application identity; default is ad hoc.
                          RISE_CODESIGN_IDENTITY provides the same setting.
  --notary-profile NAME   notarytool keychain profile. Requires Developer ID
                          signing. RISE_NOTARY_PROFILE provides the same setting.
  --keep-work-dir         Preserve the temporary build/staging directory.
  --overwrite             Replace an existing artifact with the same version
                          and build number after the new release is complete.
  -h, --help              Show this help.

Examples:
  scripts/create_macos_release.sh
  scripts/create_macos_release.sh --version 1.2.0 --build 42
  RISE_CODESIGN_IDENTITY='Developer ID Application: Example (TEAMID)' \
    RISE_NOTARY_PROFILE=rise-notary scripts/create_macos_release.sh

The default ad-hoc signature is suitable for local/internal distribution but
does not satisfy Gatekeeper on another Mac. For public distribution, supply a
Developer ID Application identity and a notarytool keychain profile.

Version and build defaults come from src/Library/Version.h. Overrides are
applied consistently to the compiled library and the app bundle.

The source working tree must be clean. The build runs from a detached worktree
at the captured commit so every translation unit comes from one immutable
source revision.

The current Xcode dependency stack produces Apple Silicon (arm64) releases and
inherits the project's macOS deployment target. Intel/universal packaging will
require universal third-party dependencies and is intentionally not advertised.
EOF
}

die() {
	echo "ERROR: $*" >&2
	exit 1
}

require_command() {
	command -v "$1" >/dev/null 2>&1 || die "required command not found: $1"
}

while [ "$#" -gt 0 ]; do
	case "$1" in
		--version)
			[ "$#" -ge 2 ] || die "--version requires a value"
			VERSION="$2"
			shift 2
			;;
		--build)
			[ "$#" -ge 2 ] || die "--build requires a value"
			BUILD_NUMBER="$2"
			shift 2
			;;
		--output-dir)
			[ "$#" -ge 2 ] || die "--output-dir requires a value"
			OUTPUT_DIR="$2"
			shift 2
			;;
		--sign-identity)
			[ "$#" -ge 2 ] || die "--sign-identity requires a value"
			SIGN_IDENTITY="$2"
			shift 2
			;;
		--notary-profile)
			[ "$#" -ge 2 ] || die "--notary-profile requires a value"
			NOTARY_PROFILE="$2"
			shift 2
			;;
		--keep-work-dir)
			KEEP_WORK_DIR=1
			shift
			;;
		--overwrite)
			OVERWRITE=1
			shift
			;;
		-h|--help)
			usage
			exit 0
			;;
		*)
			die "unknown option: $1 (use --help)"
			;;
	esac
done

[ "$(uname -s)" = "Darwin" ] || die "macOS is required"
[ "$(uname -m)" = "arm64" ] || die "the current release pipeline requires an Apple Silicon build host"

for tool in awk codesign ditto find git hdiutil install_name_tool lipo \
	otool paste plutil readlink sed shasum spctl xcodebuild xcrun; do
	require_command "${tool}"
done
[ -x /usr/libexec/PlistBuddy ] || die "required command not found: /usr/libexec/PlistBuddy"
HOMEBREW_PREFIX=""
if command -v brew >/dev/null 2>&1; then
	HOMEBREW_PREFIX="$(brew --prefix 2>/dev/null || true)"
fi

XCODE_ARCHS="arm64"
ARTIFACT_ARCH="arm64"
DESTINATION="platform=macOS,arch=arm64"

if [ -n "${NOTARY_PROFILE}" ] && [ "${SIGN_IDENTITY}" = "-" ]; then
	die "notarization requires --sign-identity or RISE_CODESIGN_IDENTITY"
fi

SOURCE_HEAD="$(git -C "${REPO_ROOT}" rev-parse HEAD)"
SOURCE_STATUS="$(git -C "${REPO_ROOT}" status --porcelain=v1 --untracked-files=all)"
[ -z "${SOURCE_STATUS}" ] || die "Git working tree is dirty; commit or stash changes before releasing"

mkdir -p "${OUTPUT_DIR}"
OUTPUT_DIR="$(cd "${OUTPUT_DIR}" && pwd)"
WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/rise-macos-release.XXXXXX")"
SOURCE_ROOT="${WORK_DIR}/source"
PUBLISH_DIR=""
LOCK_DIR=""
LOCK_HELD=0
RELEASE_COMPLETED=0
cleanup() {
	# A bash EXIT trap that ends on a successful command rewrites the
	# shell's exit status, and bash enters the trap with $? already 0 after
	# a fatal shell error such as an unbound variable -- so capturing the
	# status is necessary but not sufficient. RELEASE_COMPLETED below is
	# what actually keeps an aborted release from reporting success.
	local status=$?
	[ -z "${PUBLISH_DIR}" ] || rm -rf "${PUBLISH_DIR}"
	if [ "${LOCK_HELD}" -eq 1 ]; then
		rmdir "${LOCK_DIR}" >/dev/null 2>&1 || true
	fi
	if [ "${KEEP_WORK_DIR}" -eq 1 ]; then
		echo "Preserved work directory: ${WORK_DIR}"
		if [ -d "${SOURCE_ROOT}" ]; then
			echo "Release it with: git -C ${REPO_ROOT} worktree remove --force ${SOURCE_ROOT}"
		fi
	else
		if [ -d "${SOURCE_ROOT}" ]; then
			git -C "${REPO_ROOT}" worktree remove --force "${SOURCE_ROOT}" >/dev/null 2>&1 || true
		fi
		rm -rf "${WORK_DIR}"
		# rm -rf leaves the registration behind whenever the remove above
		# failed, so prune to keep repeated releases from accumulating
		# stale worktree entries in the repository.
		git -C "${REPO_ROOT}" worktree prune >/dev/null 2>&1 || true
	fi
	if [ "${RELEASE_COMPLETED}" -ne 1 ] && [ "${status}" -eq 0 ]; then
		echo "ERROR: release aborted before completion" >&2
		exit 1
	fi
	return "${status}"
}
trap cleanup EXIT

echo "==> Creating immutable source snapshot at ${SOURCE_HEAD}"
git -C "${REPO_ROOT}" worktree add --detach --quiet "${SOURCE_ROOT}" "${SOURCE_HEAD}"
PROJECT="${SOURCE_ROOT}/build/XCode/rise/rise.xcodeproj"
[ -d "${PROJECT}" ] || die "Xcode project not found in source snapshot: ${PROJECT}"

# extlib/oidn/install is gitignored build output, so the worktree above
# never carries it. The Xcode project resolves OIDN through $(SRCROOT) and
# falls back to the Homebrew bottle when that path is absent -- and the
# Homebrew bottle is built CPU-device-only, so the fallback silently ships
# an app that cannot denoise on the GPU. Stage the install into the
# snapshot, and refuse to build without a GPU-capable one.
OIDN_INSTALL="${REPO_ROOT}/extlib/oidn/install"
OIDN_SNAPSHOT_INSTALL="${SOURCE_ROOT}/extlib/oidn/install"
[ -d "${OIDN_INSTALL}/lib" ] || \
	die "no in-tree OIDN install at ${OIDN_INSTALL}; run extlib/oidn/fetch_prebuilt.sh (or extlib/oidn/build.sh) first"
OIDN_METAL_MODULE="$(find "${OIDN_INSTALL}/lib" -maxdepth 1 -name 'libOpenImageDenoise_device_metal*.dylib' -print -quit)"
[ -n "${OIDN_METAL_MODULE}" ] || \
	die "the OIDN install at ${OIDN_INSTALL} has no Metal device module; re-run extlib/oidn/fetch_prebuilt.sh"
echo "==> Staging GPU-capable OIDN ($(basename "${OIDN_METAL_MODULE}")) into the snapshot"
mkdir -p "$(dirname "${OIDN_SNAPSHOT_INSTALL}")"
ditto "${OIDN_INSTALL}" "${OIDN_SNAPSHOT_INSTALL}"
# Xcode records this dependency through the unnormalised
# $(SRCROOT)/../../.. form, so every match against it has to be made on
# the resolved path rather than the literal one.

read_project_setting() {
	local setting="$1"
	xcodebuild -project "${PROJECT}" -scheme "${SCHEME}" \
		-configuration "${CONFIGURATION}" -showBuildSettings 2>/dev/null |
		awk -F ' = ' -v key="${setting}" '$1 ~ "^[[:space:]]*" key "$" { print $2; exit }'
}

read_version_define() {
	local define="$1"
	awk -v key="${define}" '$1 == "#define" && $2 == key { print $3; exit }' \
		"${SOURCE_ROOT}/src/Library/Version.h"
}

if [ -z "${VERSION}" ]; then
	VERSION_MAJOR="$(read_version_define RISE_VER_MAJOR_VERSION)"
	VERSION_MINOR="$(read_version_define RISE_VER_MINOR_VERSION)"
	VERSION_REVISION="$(read_version_define RISE_VER_REVISION_VERSION)"
	VERSION="${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_REVISION}"
fi
if [ -z "${BUILD_NUMBER}" ]; then
	BUILD_NUMBER="$(read_version_define RISE_VER_BUILD_VERSION)"
fi
[ -n "${VERSION}" ] || die "unable to determine the RISE library version"
[ -n "${BUILD_NUMBER}" ] || die "unable to determine the RISE library build number"
[[ "${VERSION}" =~ ^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$ ]] || \
	die "version must be canonical X.Y.Z integers without leading zeros: ${VERSION}"
[[ "${BUILD_NUMBER}" =~ ^(0|[1-9][0-9]*)$ ]] || \
	die "build number must be a nonnegative integer without leading zeros: ${BUILD_NUMBER}"
IFS=. read -r VERSION_MAJOR VERSION_MINOR VERSION_REVISION <<< "${VERSION}"
DEPLOYMENT_TARGET="$(read_project_setting MACOSX_DEPLOYMENT_TARGET)"
[ -n "${DEPLOYMENT_TARGET}" ] || die "unable to determine MACOSX_DEPLOYMENT_TARGET"

# Every signature below is applied by hand after a CODE_SIGNING_ALLOWED=NO
# build, so any entitlement the project grows would be silently dropped from
# the shipped app. Refuse to release until the signing steps learn to carry
# them.
PROJECT_ENTITLEMENTS="$(read_project_setting CODE_SIGN_ENTITLEMENTS)"
PROJECT_APP_SANDBOX="$(read_project_setting ENABLE_APP_SANDBOX)"
[ -z "${PROJECT_ENTITLEMENTS}" ] || \
	die "project sets CODE_SIGN_ENTITLEMENTS (${PROJECT_ENTITLEMENTS}); teach the codesign steps to pass --entitlements before releasing"
[ "${PROJECT_APP_SANDBOX}" != "YES" ] || \
	die "project enables App Sandbox; the manual signing here ships no entitlements, so teach the codesign steps to pass --entitlements before releasing"

DERIVED_DATA="${WORK_DIR}/DerivedData"
ARTIFACT_BASENAME="RISE-${VERSION}-build${BUILD_NUMBER}-macOS-${ARTIFACT_ARCH}"
BUILD_LOG="${OUTPUT_DIR}/${ARTIFACT_BASENAME}-build.log"
FAILED_BUILD_LOG="${OUTPUT_DIR}/${ARTIFACT_BASENAME}-failed-build.log"
APP="${DERIVED_DATA}/Build/Products/${CONFIGURATION}/RISE-GUI.app"
DSYM="${DERIVED_DATA}/Build/Products/${CONFIGURATION}/RISE-GUI.app.dSYM"
APP_EXECUTABLE="${APP}/Contents/MacOS/RISE-GUI"
IMPORT_EXECUTABLE="${APP}/Contents/MacOS/rise-cache-import"
# The simulation bake worker (Xcode target rise-sim, embedded by RISE-GUI);
# the bake manager runs it from here first (docs/SIMULATION_GUI_STATUS.md).
SIM_EXECUTABLE="${APP}/Contents/MacOS/rise-sim"
FRAMEWORKS_DIR="${APP}/Contents/Frameworks"
DMG="${OUTPUT_DIR}/${ARTIFACT_BASENAME}.dmg"
DSYM_ZIP="${OUTPUT_DIR}/${ARTIFACT_BASENAME}.dSYM.zip"
CHECKSUMS="${OUTPUT_DIR}/${ARTIFACT_BASENAME}-SHA256SUMS.txt"
STAGED_DMG="${WORK_DIR}/$(basename "${DMG}")"
STAGED_DSYM_ZIP="${WORK_DIR}/$(basename "${DSYM_ZIP}")"
STAGED_CHECKSUMS="${WORK_DIR}/$(basename "${CHECKSUMS}")"
STAGED_BUILD_LOG="${WORK_DIR}/$(basename "${BUILD_LOG}")"
LOCK_DIR="${OUTPUT_DIR}/.${ARTIFACT_BASENAME}.lock"

if ! mkdir "${LOCK_DIR}" 2>/dev/null; then
	die "another ${ARTIFACT_BASENAME} release is running, or a stale lock remains: ${LOCK_DIR}"
fi
LOCK_HELD=1

if [ "${OVERWRITE}" -eq 0 ]; then
	for artifact in "${DMG}" "${DSYM_ZIP}" "${CHECKSUMS}" "${BUILD_LOG}"; do
		[ ! -e "${artifact}" ] || die "release artifact already exists: ${artifact} (use --overwrite to replace it)"
	done
fi

echo "==> Building RISE-GUI (${CONFIGURATION}, ${XCODE_ARCHS}) from a fresh DerivedData directory"
set +e
xcodebuild -project "${PROJECT}" -scheme "${SCHEME}" \
	-configuration "${CONFIGURATION}" -destination "${DESTINATION}" \
	-derivedDataPath "${DERIVED_DATA}" build \
	ARCHS="${XCODE_ARCHS}" ONLY_ACTIVE_ARCH=NO RISE_RELEASE_PACKAGING=YES \
	MARKETING_VERSION="${VERSION}" CURRENT_PROJECT_VERSION="${BUILD_NUMBER}" \
	GCC_PREPROCESSOR_DEFINITIONS="\$(inherited) RISE_VER_MAJOR_VERSION=${VERSION_MAJOR} RISE_VER_MINOR_VERSION=${VERSION_MINOR} RISE_VER_REVISION_VERSION=${VERSION_REVISION} RISE_VER_BUILD_VERSION=${BUILD_NUMBER}" \
	CODE_SIGNING_ALLOWED=NO GCC_TREAT_WARNINGS_AS_ERRORS=YES \
	${OPENVDB_PREFIX:+RISE_OPENVDB_PREFIX="${OPENVDB_PREFIX}"} ${TBB_PREFIX:+RISE_TBB_PREFIX="${TBB_PREFIX}"} \
	${BLOSC_PREFIX:+RISE_BLOSC_PREFIX="${BLOSC_PREFIX}"} \
	SWIFT_TREAT_WARNINGS_AS_ERRORS=YES 2>&1 | tee "${STAGED_BUILD_LOG}"
build_status=${PIPESTATUS[0]}
set -e
if [ "${build_status}" -ne 0 ]; then
	cp -p "${STAGED_BUILD_LOG}" "${FAILED_BUILD_LOG}"
	die "Opto build failed (see ${FAILED_BUILD_LOG})"
fi
[ -d "${APP}" ] || die "build succeeded but app bundle is missing: ${APP}"
[ -x "${IMPORT_EXECUTABLE}" ] || die "bundled simulation importer is missing: ${IMPORT_EXECUTABLE}"
[ -x "${SIM_EXECUTABLE}" ] || die "bundled simulation worker is missing: ${SIM_EXECUTABLE}"
[ -x "${APP_EXECUTABLE}" ] || die "app executable is missing: ${APP_EXECUTABLE}"
plutil -lint "${APP}/Contents/Info.plist" >/dev/null
[ "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "${APP}/Contents/Info.plist")" = "${VERSION}" ] || \
	die "built app marketing version does not match ${VERSION}"
[ "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' "${APP}/Contents/Info.plist")" = "${BUILD_NUMBER}" ] || \
	die "built app build number does not match ${BUILD_NUMBER}"
[ "$(/usr/libexec/PlistBuddy -c 'Print :LSMinimumSystemVersion' "${APP}/Contents/Info.plist")" = "${DEPLOYMENT_TARGET}" ] || \
	die "built app LSMinimumSystemVersion does not match the advertised minimum macOS ${DEPLOYMENT_TARGET}"
for definition in \
	"RISE_VER_MAJOR_VERSION=${VERSION_MAJOR}" \
	"RISE_VER_MINOR_VERSION=${VERSION_MINOR}" \
	"RISE_VER_REVISION_VERSION=${VERSION_REVISION}" \
	"RISE_VER_BUILD_VERSION=${BUILD_NUMBER}"; do
	grep -Fq -- "-D${definition}" "${STAGED_BUILD_LOG}" || \
		die "Opto compiler commands did not receive ${definition}"
done

bash "${SOURCE_ROOT}/scripts/bundle_macos_runtime.sh" "${APP}" "${OIDN_SNAPSHOT_INSTALL}" "${HOMEBREW_PREFIX}" \
	"${APP_EXECUTABLE}" "${IMPORT_EXECUTABLE}" "${SIM_EXECUTABLE}"

# Notices for third-party sources compiled directly into the executable;
# these have no bundled dylib for copy_dependency_license to hang off.
copy_static_license() {
	local source="$1" package="$2" destination
	[ -f "${source}" ] || die "in-tree third-party notice is missing: ${source}"
	destination="${APP}/Contents/Resources/Third-Party Licenses/${package}"
	mkdir -p "${destination}"
	if [ -e "${destination}/$(basename "${source}")" ] && \
		! cmp -s "${source}" "${destination}/$(basename "${source}")"; then
		die "two different ${package} notices are both named $(basename "${source}")"
	fi
	cp -pfL "${source}" "${destination}/$(basename "${source}")"
	chmod 0644 "${destination}/$(basename "${source}")"
}

cp -pfL "${SOURCE_ROOT}/LICENSE.TXT" "${APP}/Contents/Resources/RISE-LICENSE.txt"
chmod 0644 "${APP}/Contents/Resources/RISE-LICENSE.txt"
# In-repository notices: licenses/third_party_notices.tsv is the one list,
# shared with the Android, Windows and Blender add-on packagers.
NOTICE_MANIFEST="${SOURCE_ROOT}/licenses/third_party_notices.tsv"
[ -f "${NOTICE_MANIFEST}" ] || die "third-party notice manifest is missing: ${NOTICE_MANIFEST}"
macos_notices=0
notice_seen=$'\n'
while IFS= read -r notice_line || [ -n "${notice_line}" ]; do
	# Same rules as tools/third_party_notices.py read_manifest().
	notice_line="${notice_line%$'\r'}"
	notice_trimmed="${notice_line#"${notice_line%%[![:space:]]*}"}"
	case "${notice_trimmed}" in ''|'#'*) continue ;; esac
	notice_tabs="${notice_line//[!$'\t']/}"
	[ "${#notice_tabs}" -eq 2 ] || die "malformed line in ${NOTICE_MANIFEST} (want 3 tab-separated fields): ${notice_line}"
	notice_package="${notice_line%%$'\t'*}"
	notice_rest="${notice_line#*$'\t'}"
	notice_path="${notice_rest%%$'\t'*}"
	notice_dists="${notice_rest#*$'\t'}"
	notice_package="${notice_package#"${notice_package%%[![:space:]]*}"}"
	notice_package="${notice_package%"${notice_package##*[![:space:]]}"}"
	notice_path="${notice_path#"${notice_path%%[![:space:]]*}"}"
	notice_path="${notice_path%"${notice_path##*[![:space:]]}"}"
	[[ "${notice_package}" =~ ^[A-Za-z0-9][A-Za-z0-9._+@-]*$ ]] || \
		die "bad package name in ${NOTICE_MANIFEST}: ${notice_line}"
	case "${notice_path}" in ''|/*|..|../*|*/..|*/../*) die "path must be repo-relative in ${NOTICE_MANIFEST}: ${notice_line}" ;; esac
	case "${notice_seen}" in
		*$'\n'"${notice_package}"$'\t'"${notice_path}"$'\n'*)
			die "${notice_package} ${notice_path} is listed twice in ${NOTICE_MANIFEST}" ;;
	esac
	notice_seen="${notice_seen}${notice_package}"$'\t'"${notice_path}"$'\n'
	notice_for_macos=0
	notice_dists="${notice_dists#"${notice_dists%%[![:space:]]*}"}"
	notice_dists="${notice_dists%"${notice_dists##*[![:space:]]}"}"
	if [ "${notice_dists}" = "all" ]; then
		notice_for_macos=1
		notice_dists=""
	else
		notice_dists="${notice_dists},"
	fi
	while [ -n "${notice_dists}" ]; do
		notice_token="${notice_dists%%,*}"
		notice_dists="${notice_dists#*,}"
		notice_token="${notice_token#"${notice_token%%[![:space:]]*}"}"
		notice_token="${notice_token%"${notice_token##*[![:space:]]}"}"
		case "${notice_token}" in
			macos) notice_for_macos=1 ;;
			android|windows|blender) ;;
			*) die "unknown distribution '${notice_token}' in ${NOTICE_MANIFEST}: ${notice_line}" ;;
		esac
	done
	[ "${notice_for_macos}" -eq 1 ] || continue
	copy_static_license "${SOURCE_ROOT}/${notice_path}" "${notice_package}"
	macos_notices=$((macos_notices + 1))
done < "${NOTICE_MANIFEST}"
[ "${macos_notices}" -gt 0 ] || die "${NOTICE_MANIFEST} lists no notices for macos"

expected_arches="${XCODE_ARCHS}"
verify_architectures() {
	local binary="$1" actual required minimum
	actual="$(lipo -archs "${binary}")"
	for required in ${expected_arches}; do
		case " ${actual} " in
			*" ${required} "*) ;;
			*) die "${binary} is missing required architecture ${required} (has: ${actual})" ;;
		esac
		minimum="$(otool -arch "${required}" -l "${binary}" | awk '
			$1=="cmd" { version=($2=="LC_VERSION_MIN_MACOSX"); build=($2=="LC_BUILD_VERSION") }
			(version && $1=="version") || (build && $1=="minos") { print $2 }
		')"
		[ -n "${minimum}" ] || die "${binary} does not declare a minimum macOS version for ${required}"
		awk -v min="${minimum}" -v target="${DEPLOYMENT_TARGET}" 'BEGIN {
			split(min,a,"[.]"); split(target,b,"[.]")
			for (i=1;i<=3;++i) { if (a[i]+0<b[i]+0) exit 0; if (a[i]+0>b[i]+0) exit 1 }
		}' || die "${binary} (${required}) needs macOS ${minimum}; target is ${DEPLOYMENT_TARGET}"
	done
}

verify_dependencies() {
	local binary="$1" dependency
	while IFS= read -r dependency; do
		[ -n "${dependency}" ] || continue
		case "${dependency}" in
			/System/Library/*|/usr/lib/*|@rpath/*|@loader_path/*|@executable_path/*) ;;
			*) die "unbundled dependency '${dependency}' remains in ${binary}" ;;
		esac
	done < <(otool -L "${binary}" | sed -n '2,$s/^[[:space:]]*\([^[:space:]]*\).*/\1/p')
}

verify_rpaths() {
	local binary="$1" expected rpath count=0
	case "${binary}" in
		"${FRAMEWORKS_DIR}"/*) expected="@loader_path" ;;
		*) expected="@executable_path/../Frameworks" ;;
	esac
	while IFS= read -r rpath; do
		[ -n "${rpath}" ] || continue
		[ "${rpath}" = "${expected}" ] || \
			die "external or bundle-escaping LC_RPATH '${rpath}' remains in ${binary}"
		count=$((count + 1))
	done < <(otool -l "${binary}" | awk '/cmd LC_RPATH/{want=1; next} want && /path /{print $2; want=0}')
	[ "${count}" -eq 1 ] || die "${binary} must contain exactly one bundle-local LC_RPATH"
}

# The scan above starts at the three intended executables. Anything else Mach-O
# in the bundle would ship unrelinked and unverified, so fail closed rather
# than quietly shipping a binary still bound to this build machine.
# `lipo -archs` succeeds on thin and universal Mach-O alike and fails on
# everything else, which beats parsing file(1) -- its per-architecture
# lines turn one universal binary into several mangled paths.
while IFS= read -r -d '' macho; do
	lipo -archs "${macho}" >/dev/null 2>&1 || continue
	case "${macho}" in
		"${APP_EXECUTABLE}"|"${IMPORT_EXECUTABLE}"|"${SIM_EXECUTABLE}"|"${FRAMEWORKS_DIR}"/*.dylib) ;;
		*) die "unprocessed Mach-O image in the app bundle: ${macho} (extend the bundling scan)" ;;
	esac
done < <(find "${APP}" -type f -print0)

verify_architectures "${APP_EXECUTABLE}"
verify_dependencies "${APP_EXECUTABLE}"
verify_rpaths "${APP_EXECUTABLE}"
verify_architectures "${IMPORT_EXECUTABLE}"
verify_dependencies "${IMPORT_EXECUTABLE}"
verify_rpaths "${IMPORT_EXECUTABLE}"
verify_architectures "${SIM_EXECUTABLE}"
verify_dependencies "${SIM_EXECUTABLE}"
verify_rpaths "${SIM_EXECUTABLE}"
for binary in "${FRAMEWORKS_DIR}"/*.dylib; do
	[ -f "${binary}" ] || continue
	verify_architectures "${binary}"
	verify_dependencies "${binary}"
	verify_rpaths "${binary}"
done

# Fail closed if the denoiser we shipped cannot reach the GPU. OIDN loads
# device modules by filename from the directory holding the core library,
# so the module has to sit in Frameworks next to it.
BUNDLED_OIDN_METAL="$(find "${FRAMEWORKS_DIR}" -maxdepth 1 -name 'libOpenImageDenoise_device_metal*.dylib' -print -quit)"
[ -n "${BUNDLED_OIDN_METAL}" ] || \
	die "no OIDN Metal device module was bundled; the app would denoise on the CPU"
[ -f "${FRAMEWORKS_DIR}/$(basename "${OIDN_METAL_MODULE}")" ] || \
	die "bundled OIDN Metal module is not the one from ${OIDN_INSTALL}"

echo "==> Signing app (${SIGN_IDENTITY})"
if [ "${SIGN_IDENTITY}" = "-" ]; then
	for binary in "${FRAMEWORKS_DIR}"/*.dylib; do
		[ -f "${binary}" ] || continue
		codesign --force --sign - --timestamp=none "${binary}"
	done
	codesign --force --sign - --timestamp=none "${IMPORT_EXECUTABLE}"
	codesign --force --sign - --timestamp=none "${SIM_EXECUTABLE}"
	codesign --force --sign - --timestamp=none "${APP}"
else
	for binary in "${FRAMEWORKS_DIR}"/*.dylib; do
		[ -f "${binary}" ] || continue
		codesign --force --sign "${SIGN_IDENTITY}" --options runtime --timestamp "${binary}"
	done
	codesign --force --sign "${SIGN_IDENTITY}" --options runtime --timestamp "${IMPORT_EXECUTABLE}"
	codesign --force --sign "${SIGN_IDENTITY}" --options runtime --timestamp "${SIM_EXECUTABLE}"
	codesign --force --sign "${SIGN_IDENTITY}" --options runtime --timestamp "${APP}"
fi
codesign --verify --deep --strict --verbose=2 "${APP}"

INFO_FILE="${WORK_DIR}/Release-Info.txt"
cat > "${INFO_FILE}" <<EOF
RISE ${VERSION} (build ${BUILD_NUMBER})
Architecture: ${ARTIFACT_ARCH}
Configuration: ${CONFIGURATION}
Minimum macOS: ${DEPLOYMENT_TARGET}
Git commit: ${SOURCE_HEAD}
Dirty working tree: no
Built: $(date -u '+%Y-%m-%dT%H:%M:%SZ')
Built with: $(xcodebuild -version | paste -sd ' ' -)
OIDN devices: $(find "${FRAMEWORKS_DIR}" -maxdepth 1 -name 'libOpenImageDenoise_device_*.dylib' -exec basename {} \; |
	sed -e 's/^libOpenImageDenoise_device_//' -e 's/\..*$//' | sort | paste -sd ', ' -)
EOF

VOLUME_DIR="${WORK_DIR}/dmg-root"
mkdir -p "${VOLUME_DIR}"
ditto "${APP}" "${VOLUME_DIR}/RISE-GUI.app"
cp -p "${INFO_FILE}" "${VOLUME_DIR}/Release-Info.txt"
cp -pfL "${SOURCE_ROOT}/LICENSE.TXT" "${VOLUME_DIR}/LICENSE.txt"
chmod 0644 "${VOLUME_DIR}/LICENSE.txt"
ln -s /Applications "${VOLUME_DIR}/Applications"

echo "==> Creating DMG"
hdiutil create -volname "RISE ${VERSION}" -srcfolder "${VOLUME_DIR}" \
	-ov -format UDZO "${STAGED_DMG}" >/dev/null

if [ "${SIGN_IDENTITY}" != "-" ]; then
	codesign --force --sign "${SIGN_IDENTITY}" --timestamp "${STAGED_DMG}"
	codesign --verify --verbose=2 "${STAGED_DMG}"
fi

if [ -n "${NOTARY_PROFILE}" ]; then
	echo "==> Notarizing DMG with keychain profile '${NOTARY_PROFILE}'"
	xcrun notarytool submit "${STAGED_DMG}" --keychain-profile "${NOTARY_PROFILE}" --wait
	xcrun stapler staple "${STAGED_DMG}"
	xcrun stapler validate "${STAGED_DMG}"
	spctl --assess --type open --context context:primary-signature --verbose=2 "${STAGED_DMG}"
fi

[ -d "${DSYM}" ] || die "Opto build did not produce the required dSYM bundle"
ditto -c -k --sequesterRsrc --keepParent "${DSYM}" "${STAGED_DSYM_ZIP}"

(
	cd "${WORK_DIR}"
	: > "$(basename "${STAGED_CHECKSUMS}")"
	shasum -a 256 "$(basename "${STAGED_DMG}")" >> "$(basename "${STAGED_CHECKSUMS}")"
	shasum -a 256 "$(basename "${STAGED_DSYM_ZIP}")" >> "$(basename "${STAGED_CHECKSUMS}")"
	shasum -a 256 "$(basename "${STAGED_BUILD_LOG}")" >> "$(basename "${STAGED_CHECKSUMS}")"
)

current_source_head="$(git -C "${SOURCE_ROOT}" rev-parse HEAD)"
current_source_status="$(git -C "${SOURCE_ROOT}" status --porcelain=v1 --untracked-files=all)"
[ "${current_source_head}" = "${SOURCE_HEAD}" ] || \
	die "immutable source snapshot changed commits during the release build"
[ -z "${current_source_status}" ] || \
	die "immutable source snapshot was modified during the release build"

# Copy into a hidden directory on the destination filesystem. The final moves
# are then atomic renames even when --output-dir is on a different volume.
PUBLISH_DIR="$(mktemp -d "${OUTPUT_DIR}/.rise-release-publish.XXXXXX")"
cp -p "${STAGED_DMG}" "${PUBLISH_DIR}/$(basename "${DMG}")"
cp -p "${STAGED_DSYM_ZIP}" "${PUBLISH_DIR}/$(basename "${DSYM_ZIP}")"
cp -p "${STAGED_BUILD_LOG}" "${PUBLISH_DIR}/$(basename "${BUILD_LOG}")"
cp -p "${STAGED_CHECKSUMS}" "${PUBLISH_DIR}/$(basename "${CHECKSUMS}")"
(
	cd "${PUBLISH_DIR}"
	shasum -a 256 -c "$(basename "${CHECKSUMS}")" >/dev/null
)

# The checksum file is the publication marker: move it last so an interrupted
# publish cannot leave new artifacts paired with a stale checksum manifest.
[ "${OVERWRITE}" -eq 1 ] && rm -f "${CHECKSUMS}"
mv -f "${PUBLISH_DIR}/$(basename "${DMG}")" "${DMG}"
mv -f "${PUBLISH_DIR}/$(basename "${DSYM_ZIP}")" "${DSYM_ZIP}"
mv -f "${PUBLISH_DIR}/$(basename "${BUILD_LOG}")" "${BUILD_LOG}"
(
	cd "${OUTPUT_DIR}"
	shasum -a 256 -c "${PUBLISH_DIR}/$(basename "${CHECKSUMS}")" >/dev/null
)
mv -f "${PUBLISH_DIR}/$(basename "${CHECKSUMS}")" "${CHECKSUMS}"
rmdir "${PUBLISH_DIR}"
PUBLISH_DIR=""
rmdir "${LOCK_DIR}"
LOCK_HELD=0
RELEASE_COMPLETED=1

echo
echo "Release complete:"
echo "  DMG:       ${DMG}"
echo "  dSYM:      ${DSYM_ZIP}"
echo "  Checksums: ${CHECKSUMS}"
echo "  Build log: ${BUILD_LOG}"
if [ "${SIGN_IDENTITY}" = "-" ]; then
	echo "  Signing:   ad hoc (use Developer ID + notarization for public distribution)"
elif [ -z "${NOTARY_PROFILE}" ]; then
	echo "  Signing:   Developer ID (not notarized)"
else
	echo "  Signing:   Developer ID, notarized and stapled"
fi
