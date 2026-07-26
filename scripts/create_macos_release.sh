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

for tool in awk codesign ditto file find git hdiutil install_name_tool lipo \
	otool plutil sed shasum xcodebuild xcrun; do
	require_command "${tool}"
done
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
cleanup() {
	[ -z "${PUBLISH_DIR}" ] || rm -rf "${PUBLISH_DIR}"
	if [ "${LOCK_HELD}" -eq 1 ]; then
		rmdir "${LOCK_DIR}" >/dev/null 2>&1 || true
	fi
	if [ "${KEEP_WORK_DIR}" -eq 1 ]; then
		echo "Preserved work directory: ${WORK_DIR}"
	else
		if [ -d "${SOURCE_ROOT}" ]; then
			git -C "${REPO_ROOT}" worktree remove --force "${SOURCE_ROOT}" >/dev/null 2>&1 || true
		fi
		rm -rf "${WORK_DIR}"
	fi
}
trap cleanup EXIT

echo "==> Creating immutable source snapshot at ${SOURCE_HEAD}"
git -C "${REPO_ROOT}" worktree add --detach --quiet "${SOURCE_ROOT}" "${SOURCE_HEAD}"
PROJECT="${SOURCE_ROOT}/build/XCode/rise/rise.xcodeproj"
[ -d "${PROJECT}" ] || die "Xcode project not found in source snapshot: ${PROJECT}"

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

DERIVED_DATA="${WORK_DIR}/DerivedData"
ARTIFACT_BASENAME="RISE-${VERSION}-build${BUILD_NUMBER}-macOS-${ARTIFACT_ARCH}"
BUILD_LOG="${OUTPUT_DIR}/${ARTIFACT_BASENAME}-build.log"
FAILED_BUILD_LOG="${OUTPUT_DIR}/${ARTIFACT_BASENAME}-failed-build.log"
APP="${DERIVED_DATA}/Build/Products/${CONFIGURATION}/RISE-GUI.app"
DSYM="${DERIVED_DATA}/Build/Products/${CONFIGURATION}/RISE-GUI.app.dSYM"
APP_EXECUTABLE="${APP}/Contents/MacOS/RISE-GUI"
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
	ARCHS="${XCODE_ARCHS}" ONLY_ACTIVE_ARCH=NO \
	MARKETING_VERSION="${VERSION}" CURRENT_PROJECT_VERSION="${BUILD_NUMBER}" \
	GCC_PREPROCESSOR_DEFINITIONS="RISE_VER_MAJOR_VERSION=${VERSION_MAJOR} RISE_VER_MINOR_VERSION=${VERSION_MINOR} RISE_VER_REVISION_VERSION=${VERSION_REVISION} RISE_VER_BUILD_VERSION=${BUILD_NUMBER}" \
	CODE_SIGNING_ALLOWED=NO GCC_TREAT_WARNINGS_AS_ERRORS=YES \
	SWIFT_TREAT_WARNINGS_AS_ERRORS=YES 2>&1 | tee "${STAGED_BUILD_LOG}"
build_status=${PIPESTATUS[0]}
set -e
if [ "${build_status}" -ne 0 ]; then
	cp -p "${STAGED_BUILD_LOG}" "${FAILED_BUILD_LOG}"
	die "Opto build failed (see ${FAILED_BUILD_LOG})"
fi
[ -d "${APP}" ] || die "build succeeded but app bundle is missing: ${APP}"
[ -x "${APP_EXECUTABLE}" ] || die "app executable is missing: ${APP_EXECUTABLE}"
plutil -lint "${APP}/Contents/Info.plist" >/dev/null
[ "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "${APP}/Contents/Info.plist")" = "${VERSION}" ] || \
	die "built app marketing version does not match ${VERSION}"
[ "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' "${APP}/Contents/Info.plist")" = "${BUILD_NUMBER}" ] || \
	die "built app build number does not match ${BUILD_NUMBER}"
for definition in \
	"RISE_VER_MAJOR_VERSION=${VERSION_MAJOR}" \
	"RISE_VER_MINOR_VERSION=${VERSION_MINOR}" \
	"RISE_VER_REVISION_VERSION=${VERSION_REVISION}" \
	"RISE_VER_BUILD_VERSION=${BUILD_NUMBER}"; do
	grep -Fq -- "-D${definition}" "${STAGED_BUILD_LOG}" || \
		die "Opto compiler commands did not receive ${definition}"
done

mkdir -p "${FRAMEWORKS_DIR}"

# Each copied library is stored once by basename. Parallel arrays keep this
# compatible with the Bash 3.2 that ships with older supported macOS hosts.
BUNDLED_NAMES=()
BUNDLED_SOURCES=()
SCAN_BINARIES=("${APP_EXECUTABLE}")
SCAN_INDEX=0
SEARCH_DIRS=("${FRAMEWORKS_DIR}" "/opt/homebrew/lib" "/usr/local/lib")
[ -n "${HOMEBREW_PREFIX}" ] && SEARCH_DIRS+=("${HOMEBREW_PREFIX}/lib")
[ -d "${SOURCE_ROOT}/extlib/oidn/install/lib" ] && SEARCH_DIRS+=("${SOURCE_ROOT}/extlib/oidn/install/lib")

is_system_dependency() {
	case "$1" in
		/System/Library/*|/usr/lib/*) return 0 ;;
		*) return 1 ;;
	esac
}

add_search_dir() {
	local candidate="$1" existing
	[ -d "${candidate}" ] || return 0
	for existing in "${SEARCH_DIRS[@]}"; do
		[ "${existing}" = "${candidate}" ] && return 0
	done
	SEARCH_DIRS+=("${candidate}")
}

expand_runtime_path() {
	local value="$1" loader="$2"
	case "${value}" in
		@loader_path/*) printf '%s/%s\n' "$(dirname "${loader}")" "${value#@loader_path/}" ;;
		@executable_path/*) printf '%s/%s\n' "${APP}/Contents/MacOS" "${value#@executable_path/}" ;;
		*) printf '%s\n' "${value}" ;;
	esac
}

resolve_dependency() {
	local dependency="$1" loader="$2" suffix candidate search_dir rpath
	case "${dependency}" in
		/*)
			[ -f "${dependency}" ] && { printf '%s\n' "${dependency}"; return 0; }
			;;
		@loader_path/*|@executable_path/*)
			candidate="$(expand_runtime_path "${dependency}" "${loader}")"
			[ -f "${candidate}" ] && { printf '%s\n' "${candidate}"; return 0; }
			;;
		@rpath/*)
			suffix="${dependency#@rpath/}"
			while IFS= read -r rpath; do
				[ -n "${rpath}" ] || continue
				rpath="$(expand_runtime_path "${rpath}" "${loader}")"
				candidate="${rpath}/${suffix}"
				[ -f "${candidate}" ] && { printf '%s\n' "${candidate}"; return 0; }
			done < <(otool -l "${loader}" | awk '/cmd LC_RPATH/{want=1; next} want && /path /{print $2; want=0}')
			for search_dir in "${SEARCH_DIRS[@]}"; do
				candidate="${search_dir}/${suffix}"
				[ -f "${candidate}" ] && { printf '%s\n' "${candidate}"; return 0; }
			done
			;;
	esac
	return 1
}

copy_dependency_license() {
	local source="$1" package_root="" package_name="" license_file destination
	if [ -n "${HOMEBREW_PREFIX}" ]; then
		case "${source}" in
		"${HOMEBREW_PREFIX}"/opt/*/*)
			package_name="${source#${HOMEBREW_PREFIX}/opt/}"
			package_name="${package_name%%/*}"
			package_root="${HOMEBREW_PREFIX}/opt/${package_name}"
			;;
		"${HOMEBREW_PREFIX}"/Cellar/*/*)
			package_name="${source#${HOMEBREW_PREFIX}/Cellar/}"
			package_name="${package_name%%/*}"
			package_root="${HOMEBREW_PREFIX}/opt/${package_name}"
			;;
		"${HOMEBREW_PREFIX}"/lib/libopenpgl*)
			package_name="openpgl"
			package_root="${HOMEBREW_PREFIX}/share/doc/openpgl"
			;;
		esac
	fi
	case "${source}" in
		"${SOURCE_ROOT}"/extlib/oidn/install/lib/*)
			package_name="open-image-denoise"
			package_root="${SOURCE_ROOT}/extlib/oidn/source"
			;;
	esac
	[ -n "${package_root}" ] || return 0
	[ -d "${package_root}" ] || return 0
	destination="${APP}/Contents/Resources/Third-Party Licenses/${package_name}"
	while IFS= read -r license_file; do
		[ -f "${license_file}" ] || continue
		mkdir -p "${destination}"
		cp -pL "${license_file}" "${destination}/$(basename "${license_file}")"
	done < <(find -L "${package_root}" -maxdepth 5 -type f \( \
		-iname 'LICENSE' -o -iname 'LICENSE.*' -o -iname 'COPYING' -o \
		-iname 'COPYING.*' -o -iname 'NOTICE' -o -iname 'NOTICE.*' -o \
		-iname 'third-party-programs*.txt' \) -print)
}

add_library() {
	local source="$1" name destination index existing_source
	name="$(basename "${source}")"
	destination="${FRAMEWORKS_DIR}/${name}"
	index=0
	while [ "${index}" -lt "${#BUNDLED_NAMES[@]}" ]; do
		if [ "${BUNDLED_NAMES[$index]}" = "${name}" ]; then
			existing_source="${BUNDLED_SOURCES[$index]}"
			[ "${source}" = "${destination}" ] && return 0
			[ "$(cd "$(dirname "${existing_source}")" && pwd -P)/$(basename "${existing_source}")" = \
			  "$(cd "$(dirname "${source}")" && pwd -P)/$(basename "${source}")" ] || \
				die "two external libraries share basename ${name}: ${existing_source} and ${source}"
			return 0
		fi
		index=$((index + 1))
	done
	cp -pL "${source}" "${destination}"
	chmod u+w "${destination}"
	BUNDLED_NAMES+=("${name}")
	BUNDLED_SOURCES+=("${source}")
	SCAN_BINARIES+=("${destination}")
	add_search_dir "$(dirname "${source}")"
	copy_dependency_license "${source}"
	case "${name}" in
		libOpenImageDenoise.*.dylib|libOpenImageDenoise.dylib)
			local plugin
			for plugin in "$(dirname "${source}")"/libOpenImageDenoise_core*.dylib \
				"$(dirname "${source}")"/libOpenImageDenoise_device_*.dylib; do
				[ -f "${plugin}" ] || continue
				add_library "${plugin}"
			done
			;;
	esac
}

echo "==> Bundling external runtime libraries"
while [ "${SCAN_INDEX}" -lt "${#SCAN_BINARIES[@]}" ]; do
	binary="${SCAN_BINARIES[$SCAN_INDEX]}"
	SCAN_INDEX=$((SCAN_INDEX + 1))
	dependency_start_line=2
	case "${binary}" in
		"${FRAMEWORKS_DIR}"/*) dependency_start_line=3 ;;
	esac
	while IFS= read -r dependency; do
		[ -n "${dependency}" ] || continue
		is_system_dependency "${dependency}" && continue
		if ! resolved="$(resolve_dependency "${dependency}" "${binary}")"; then
			die "cannot resolve dependency '${dependency}' required by ${binary}"
		fi
		add_library "${resolved}"
		install_name_tool -change "${dependency}" "@rpath/$(basename "${resolved}")" "${binary}"
	done < <(otool -L "${binary}" | sed -n "${dependency_start_line},\$s/^[[:space:]]*\\([^[:space:]]*\\).*/\\1/p")
	case "${binary}" in
		"${FRAMEWORKS_DIR}"/*) install_name_tool -id "@rpath/$(basename "${binary}")" "${binary}" ;;
	esac
done

normalize_rpaths() {
	local binary="$1" desired rpaths rpath
	case "${binary}" in
		"${FRAMEWORKS_DIR}"/*) desired="@loader_path" ;;
		*) desired="@executable_path/../Frameworks" ;;
	esac
	rpaths="$(otool -l "${binary}" | awk '/cmd LC_RPATH/{want=1; next} want && /path /{if (!seen[$2]++) print $2; want=0}')"
	while IFS= read -r rpath; do
		[ -n "${rpath}" ] || continue
		install_name_tool -delete_rpath "${rpath}" "${binary}"
	done <<< "${rpaths}"
	install_name_tool -add_rpath "${desired}" "${binary}"
}

# Never let @rpath resolve back to Homebrew or another build machine path.
# Each image gets exactly one bundle-local runpath before the final signature.
normalize_rpaths "${APP_EXECUTABLE}"
for binary in "${FRAMEWORKS_DIR}"/*.dylib; do
	[ -f "${binary}" ] || continue
	normalize_rpaths "${binary}"
done

cp -p "${SOURCE_ROOT}/LICENSE.TXT" "${APP}/Contents/Resources/RISE-LICENSE.txt"
mkdir -p "${APP}/Contents/Resources/Third-Party Licenses/cgltf"
cp -p "${SOURCE_ROOT}/extlib/cgltf/LICENSE.txt" \
	"${APP}/Contents/Resources/Third-Party Licenses/cgltf/LICENSE.txt"
mkdir -p "${APP}/Contents/Resources/Third-Party Licenses/mt19937"
cp -p "${SOURCE_ROOT}/extlib/mt19937/LICENSE.txt" \
	"${APP}/Contents/Resources/Third-Party Licenses/mt19937/LICENSE.txt"

expected_arches="${XCODE_ARCHS}"
verify_architectures() {
	local binary="$1" actual required
	actual="$(lipo -archs "${binary}")"
	for required in ${expected_arches}; do
		case " ${actual} " in
			*" ${required} "*) ;;
			*) die "${binary} is missing required architecture ${required} (has: ${actual})" ;;
		esac
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

verify_architectures "${APP_EXECUTABLE}"
verify_dependencies "${APP_EXECUTABLE}"
verify_rpaths "${APP_EXECUTABLE}"
for binary in "${FRAMEWORKS_DIR}"/*.dylib; do
	[ -f "${binary}" ] || continue
	verify_architectures "${binary}"
	verify_dependencies "${binary}"
	verify_rpaths "${binary}"
done

echo "==> Signing app (${SIGN_IDENTITY})"
if [ "${SIGN_IDENTITY}" = "-" ]; then
	for binary in "${FRAMEWORKS_DIR}"/*.dylib; do
		[ -f "${binary}" ] || continue
		codesign --force --sign - --timestamp=none "${binary}"
	done
	codesign --force --sign - --timestamp=none "${APP}"
else
	for binary in "${FRAMEWORKS_DIR}"/*.dylib; do
		[ -f "${binary}" ] || continue
		codesign --force --sign "${SIGN_IDENTITY}" --options runtime --timestamp "${binary}"
	done
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
EOF

VOLUME_DIR="${WORK_DIR}/dmg-root"
mkdir -p "${VOLUME_DIR}"
ditto "${APP}" "${VOLUME_DIR}/RISE-GUI.app"
cp -p "${INFO_FILE}" "${VOLUME_DIR}/Release-Info.txt"
cp -p "${SOURCE_ROOT}/LICENSE.TXT" "${VOLUME_DIR}/LICENSE.txt"
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

echo
echo "Release complete:"
echo "  DMG:       ${DMG}"
[ -f "${DSYM_ZIP}" ] && echo "  dSYM:      ${DSYM_ZIP}"
echo "  Checksums: ${CHECKSUMS}"
echo "  Build log: ${BUILD_LOG}"
if [ "${SIGN_IDENTITY}" = "-" ]; then
	echo "  Signing:   ad hoc (use Developer ID + notarization for public distribution)"
elif [ -z "${NOTARY_PROFILE}" ]; then
	echo "  Signing:   Developer ID (not notarized)"
else
	echo "  Signing:   Developer ID, notarized and stapled"
fi
