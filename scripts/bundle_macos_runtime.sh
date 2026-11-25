#!/bin/bash
# Resolve the Mach-O dependency graph, bundle notices, and confine runtime
# search paths to Contents/Frameworks for each supplied executable.
set -euo pipefail
APP="$1"
OIDN_SNAPSHOT_INSTALL="$2"
HOMEBREW_PREFIX="$3"
shift 3
FRAMEWORKS_DIR="${APP}/Contents/Frameworks"
OIDN_SNAPSHOT_LIB=""
if [ -n "${OIDN_SNAPSHOT_INSTALL}" ] && [ -d "${OIDN_SNAPSHOT_INSTALL}/lib" ]; then
	OIDN_SNAPSHOT_LIB="$(cd "${OIDN_SNAPSHOT_INSTALL}/lib" && pwd -P)"
fi
die() { echo "ERROR: $*" >&2; exit 1; }

mkdir -p "${FRAMEWORKS_DIR}"

# Each copied library is stored once by basename. Parallel arrays keep this
# compatible with the Bash 3.2 that ships with older supported macOS hosts.
BUNDLED_NAMES=()
BUNDLED_SOURCES=()
SCAN_BINARIES=("$@")
SCAN_SOURCES=("$@")
SCAN_INDEX=0
# OIDN comes from the staged in-tree install; the rest of the third-party
# stack still comes out of the Homebrew prefix.
SEARCH_DIRS=("${OIDN_SNAPSHOT_INSTALL}/lib" "/opt/homebrew/lib" "/usr/local/lib")
if [ -n "${HOMEBREW_PREFIX}" ]; then
	SEARCH_DIRS+=("${HOMEBREW_PREFIX}/lib")
fi

for prefix in "${OPENVDB_PREFIX:-}" "${TBB_PREFIX:-}" "${BLOSC_PREFIX:-}"; do
	[ -z "${prefix}" ] || SEARCH_DIRS=("${prefix}/lib" "${SEARCH_DIRS[@]}")
done

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

# The "(compatibility version X, current version Y)" pair from an image's
# own LC_ID_DYLIB -- the ABI contract a consumer was linked against.
dylib_abi_version() {
	otool -L "$1" | sed -n '2s/.*(\(compatibility version .*\))$/\1/p'
}

physical_path() {
	local path="$1" link guard=0
	while [ -L "${path}" ]; do
		guard=$((guard + 1))
		[ "${guard}" -le 32 ] || die "symlink chain too deep resolving $1"
		link="$(readlink "${path}")"
		case "${link}" in
			/*) path="${link}" ;;
			*) path="$(dirname "${path}")/${link}" ;;
		esac
	done
	printf '%s/%s\n' "$(cd "$(dirname "${path}")" && pwd -P)" "$(basename "${path}")"
}

copy_dependency_license() {
	local source="$1" package_root="" package_name="" license_file destination
	local physical copied=0
	physical="$(physical_path "${source}")"
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
	case "${physical}" in
	"${OIDN_SNAPSHOT_LIB:-/nonexistent-rise-oidn}"/*)
		# Covers the OIDN dylibs and the TBB shipped alongside them; the
		# distribution's doc/ directory carries both notices.
		package_name="open-image-denoise"
		package_root="${OIDN_SNAPSHOT_INSTALL}"
		;;
	esac
	if [ -n "${HOMEBREW_PREFIX}" ]; then
		# A bare prefix/lib entry is normally a symlink into the Cellar; follow
		# it so ordinary brew installs resolve without a per-formula case arm.
		if [ -z "${package_root}" ]; then
			case "${physical}" in
			"${HOMEBREW_PREFIX}"/Cellar/*/*)
				package_name="${physical#${HOMEBREW_PREFIX}/Cellar/}"
				package_name="${package_name%%/*}"
				package_root="${HOMEBREW_PREFIX}/opt/${package_name}"
				;;
			esac
		fi
	fi
	# A deployment-target build may use staged, version-pinned libraries
	# outside Homebrew. Locate the license alongside the actual binary.
	local prefix
	for prefix in "${OPENVDB_PREFIX:-}" "${TBB_PREFIX:-}" "${BLOSC_PREFIX:-}"; do
		[ -n "${prefix}" ] && [ -d "${prefix}/lib" ] || continue
		case "${physical}" in
			"$(cd "${prefix}/lib" && pwd -P)"/*)
				package_root="${prefix}"
				package_name="$(basename "${source}" .dylib)"
				break
				;;
		esac
	done
	[ -n "${package_root}" ] || \
		die "no third-party notice mapping for bundled library ${source}; extend copy_dependency_license"
	[ -d "${package_root}" ] || \
		die "third-party notice directory for ${package_name} is missing: ${package_root}"
	destination="${APP}/Contents/Resources/Third-Party Licenses/${package_name}"
	while IFS= read -r license_file; do
		[ -f "${license_file}" ] || continue
		mkdir -p "${destination}"
		cp -pfL "${license_file}" "${destination}/$(basename "${license_file}")"
		chmod 0644 "${destination}/$(basename "${license_file}")"
		copied=$((copied + 1))
	done < <(find -L "${package_root}" -maxdepth 5 -type f \( \
		-iname 'LICENSE' -o -iname 'LICENSE.*' -o -iname 'COPYING' -o \
		-iname 'COPYING.*' -o -iname 'NOTICE' -o -iname 'NOTICE.*' -o \
		-iname 'third-party-programs*.txt' \) -print)
	[ "${copied}" -gt 0 ] || \
		die "no license file found under ${package_root} for bundled library ${source}"
}

add_library() {
	local source="$1" name destination index existing_source existing_abi new_abi
	name="$(basename "${source}")"
	destination="${FRAMEWORKS_DIR}/${name}"
	index=0
	while [ "${index}" -lt "${#BUNDLED_NAMES[@]}" ]; do
		if [ "${BUNDLED_NAMES[$index]}" = "${name}" ]; then
			existing_source="${BUNDLED_SOURCES[$index]}"
			[ "${source}" = "${destination}" ] && return 0
			if [ "$(physical_path "${existing_source}")" != "$(physical_path "${source}")" ]; then
				# Two distributions can legitimately ship the same shared
				# runtime -- OIDN vendors its own oneTBB while openpgl links
				# the Homebrew one. A single copy serves both consumers
				# exactly when the ABI version pair matches, and shipping two
				# copies of a threading runtime in one process is worse than
				# unifying. Anything else is a genuine conflict.
				existing_abi="$(dylib_abi_version "${existing_source}")"
				new_abi="$(dylib_abi_version "${source}")"
				[ -n "${existing_abi}" ] && [ "${existing_abi}" = "${new_abi}" ] || \
					die "two external libraries share basename ${name} with different ABI versions: ${existing_source} (${existing_abi}) and ${source} (${new_abi})"
				echo "    unified ${name} (${existing_abi}): ${source} -> already bundled from ${existing_source}"
			fi
			return 0
		fi
		index=$((index + 1))
	done
	cp -pL "${source}" "${destination}"
	chmod u+w "${destination}"
	BUNDLED_NAMES+=("${name}")
	BUNDLED_SOURCES+=("${source}")
	SCAN_BINARIES+=("${destination}")
	SCAN_SOURCES+=("${source}")
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
	loader_source="${SCAN_SOURCES[$SCAN_INDEX]}"
	SCAN_INDEX=$((SCAN_INDEX + 1))
	# Only these private bundle copies are about to be rewritten. Remove
	# stale source signatures before relocations and sign the result later.
	if codesign -d "${binary}" >/dev/null 2>&1; then
		codesign --remove-signature "${binary}"
	fi
	dependency_start_line=2
	case "${binary}" in
		"${FRAMEWORKS_DIR}"/*) dependency_start_line=3 ;;
	esac
	while IFS= read -r dependency; do
		[ -n "${dependency}" ] || continue
		is_system_dependency "${dependency}" && continue
		if ! resolved="$(resolve_dependency "${dependency}" "${loader_source}")"; then
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
	local binary="$1" desired rpath guard=0
	case "${binary}" in
		"${FRAMEWORKS_DIR}"/*) desired="@loader_path" ;;
		*) desired="@executable_path/../Frameworks" ;;
	esac
	while :; do
		rpath="$(otool -l "${binary}" | awk '/cmd LC_RPATH/{want=1; next} want && /path /{print $2; exit}')"
		[ -n "${rpath}" ] || break
		guard=$((guard + 1))
		[ "${guard}" -le 64 ] || die "unable to clear LC_RPATH entries from ${binary}"
		install_name_tool -delete_rpath "${rpath}" "${binary}"
	done
	install_name_tool -add_rpath "${desired}" "${binary}"
}

# Never let @rpath resolve back to Homebrew or another build machine path.
# Each image gets exactly one bundle-local runpath before the final signature.
for executable in "$@"; do
	normalize_rpaths "${executable}"
done
for name in "${BUNDLED_NAMES[@]}"; do
	normalize_rpaths "${FRAMEWORKS_DIR}/${name}"
done

