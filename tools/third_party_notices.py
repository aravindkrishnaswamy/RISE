#!/usr/bin/env python3
"""Stage the third-party licence notices a RISE binary distribution carries.

The list of in-repository notices is licenses/third_party_notices.tsv (see
its header).  This script copies the entries for one distribution into a
folder, adds the notices of libraries that come from outside the
repository (a vcpkg tree, an install prefix such as OIDN's), writes an
index, and exits non-zero if anything it was asked to ship is missing.

Used by the Windows RISE-CLI / RISE-GUI post-build steps and by
tools/package_blender_addon.py.  The macOS release script and the Android
Gradle build read the same manifest themselves.

Examples:
  python tools/third_party_notices.py --distribution windows \\
      --dest "bin/Third-Party Licenses" \\
      --vcpkg-installed build/VS2022/vcpkg_installed/x64-windows \\
      --vcpkg-triplet x64-windows \\
      --license-root open-image-denoise=extlib/oidn/install \\
      --license-root openpgl=C:/Dev/openpgl-install
"""

import argparse
import hashlib
import os
import re
import shutil
import sys
import tempfile
import time

DISTRIBUTIONS = ("macos", "android", "windows", "blender")
MANIFEST_RELPATH = os.path.join("licenses", "third_party_notices.tsv")
INDEX_NAME = "README.txt"
INDEX_MARKER = "RISE third-party notices"

# The file names copy_dependency_license in scripts/create_macos_release.sh
# accepts as a licence; kept identical so the two packagers agree.
_LICENSE_NAME = re.compile(
    r"^(license|license\..*|copying|copying\..*|notice|notice\..*|"
    r"third-party-programs.*\.txt)$",
    re.IGNORECASE,
)
_PACKAGE_NAME = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._+@-]*$")


class NoticeError(Exception):
    """A notice that must ship is missing, or the inputs are malformed."""


def default_repo_root():
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def read_manifest(repo_root):
    """Return [(package, relpath, frozenset(distributions))] in file order."""
    path = os.path.join(repo_root, MANIFEST_RELPATH)
    if not os.path.isfile(path):
        raise NoticeError("notice manifest is missing: %s" % path)
    entries = []
    with open(path, "r", encoding="utf-8") as handle:
        for number, raw in enumerate(handle, 1):
            line = raw.rstrip("\r\n")
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            fields = line.split("\t")
            if len(fields) != 3:
                raise NoticeError(
                    "%s:%d: expected 3 tab-separated fields, got %d"
                    % (path, number, len(fields)))
            package, relpath, dists = (field.strip() for field in fields)
            if not _PACKAGE_NAME.match(package):
                raise NoticeError("%s:%d: bad package name %r" % (path, number, package))
            if os.path.isabs(relpath) or ".." in relpath.replace("\\", "/").split("/"):
                raise NoticeError("%s:%d: path must be repo-relative: %r" % (path, number, relpath))
            if dists == "all":
                dist_set = frozenset(DISTRIBUTIONS)
            else:
                dist_set = frozenset(d.strip() for d in dists.split(","))
                unknown = sorted(dist_set - set(DISTRIBUTIONS))
                if unknown or not dist_set:
                    raise NoticeError(
                        "%s:%d: unknown distribution(s) %s" % (path, number, unknown or dists))
            if any(e[0] == package and e[1] == relpath for e in entries):
                raise NoticeError("%s:%d: %s %s is listed twice" % (path, number, package, relpath))
            entries.append((package, relpath, dist_set))
    if not entries:
        raise NoticeError("notice manifest lists no notices: %s" % path)
    return entries


def manifest_files(repo_root, distribution):
    """[(package, absolute source path)] the manifest lists for one distribution."""
    if distribution not in DISTRIBUTIONS:
        raise NoticeError("unknown distribution %r" % distribution)
    return [
        (package, os.path.join(repo_root, relpath))
        for package, relpath, dists in read_manifest(repo_root)
        if distribution in dists
    ]


def license_files_under(root, max_depth=5):
    """Licence files under an install prefix, as the macOS packager finds them."""
    if not os.path.isdir(root):
        raise NoticeError("licence directory is missing: %s" % root)
    found = []
    root = os.path.abspath(root)
    base_depth = root.rstrip(os.sep).count(os.sep)
    for directory, subdirs, files in os.walk(root, followlinks=True):
        # Files at most max_depth levels below root, as `find -maxdepth`.
        if directory.count(os.sep) - base_depth >= max_depth - 1:
            subdirs[:] = []
        subdirs.sort()
        for name in sorted(files):
            if _LICENSE_NAME.match(name):
                found.append(os.path.join(directory, name))
    if not found:
        raise NoticeError("no licence file found under %s" % root)
    return found


def vcpkg_copyrights(installed_root, triplet):
    """[(port, copyright path)] for every port installed for `triplet`.

    `installed_root` is the vcpkg install root (the directory holding
    vcpkg/info/ and <triplet>/).  Every port vcpkg installs records its
    files in vcpkg/info/<port>_<version>_<triplet>.list, and a port's
    licence is installed as <triplet>/share/<port>/copyright.  A port whose
    list has no copyright entry is an error, except vcpkg's own build-helper
    ports (vcpkg-cmake, vcpkg-cmake-config, ...), which install only CMake
    scripts and put nothing into a binary.
    """
    info_dir = os.path.join(installed_root, "vcpkg", "info")
    if not os.path.isdir(info_dir):
        raise NoticeError("vcpkg install info directory is missing: %s" % info_dir)
    suffix = "_%s.list" % triplet
    copyright_line = re.compile(r"^%s/share/([^/]+)/copyright$" % re.escape(triplet))
    results = []
    for name in sorted(os.listdir(info_dir)):
        if not name.endswith(suffix):
            continue
        port = name.split("_", 1)[0]
        copyright_path = None
        with open(os.path.join(info_dir, name), "r", encoding="utf-8") as handle:
            for raw in handle:
                match = copyright_line.match(raw.strip().replace("\\", "/"))
                if match:
                    copyright_path = os.path.join(installed_root, triplet, "share",
                                                  match.group(1), "copyright")
                    port = match.group(1)
                    break
        if copyright_path is None:
            if port.startswith("vcpkg-"):
                continue
            raise NoticeError("vcpkg port %s (%s) installs no copyright file" % (port, name))
        if not os.path.isfile(copyright_path):
            raise NoticeError("vcpkg port %s lists %s but it does not exist"
                              % (port, copyright_path))
        results.append((port, copyright_path))
    if not results:
        raise NoticeError("no vcpkg ports for triplet %s under %s" % (triplet, info_dir))
    return results


def vcpkg_dll_names(installed_root, triplet):
    """Lower-cased basenames of every DLL the vcpkg ports for `triplet` install."""
    info_dir = os.path.join(installed_root, "vcpkg", "info")
    names = set()
    for name in os.listdir(info_dir) if os.path.isdir(info_dir) else []:
        if not name.endswith("_%s.list" % triplet):
            continue
        with open(os.path.join(info_dir, name), "r", encoding="utf-8") as handle:
            for raw in handle:
                entry = raw.strip().replace("\\", "/")
                if entry.lower().endswith(".dll"):
                    names.add(entry.rsplit("/", 1)[-1].lower())
    return names


# Microsoft's redistributable C/C++ runtime: its terms ask for no notice.
_MS_RUNTIME = re.compile(r"^(vcruntime|msvcp|concrt|vccorlib|vcomp|ucrtbase|api-ms-win-)"
                         r".*\.dll$", re.IGNORECASE)
_TBB_DLL = re.compile(r"^tbb[a-z0-9_]*\.dll$", re.IGNORECASE)
_TBB_NOTICE = re.compile(r"^third-party-programs-onetbb.*\.txt$", re.IGNORECASE)
_MAX_TBB_NOTICES = 12


def _dll_stem(name):
    """OpenEXR-3_2.dll -> openexr, Qt6Cored.dll -> qt6cored: name without version."""
    stem = name.lower()[:-len(".dll")]
    return re.sub(r"[-_.]?[0-9][0-9_.-]*$", "", stem) or stem


def audit_dlls(dll_dir, known_names, tbb_root=None, tbb_covered=False, skip=()):
    """Check every DLL under the output folder has a known notice source.

    A DLL is accounted for by its file name: one a vcpkg port installs, or
    one inside a --license-root install (`known_names`, lower-cased).  A
    name that differs from a known one only in its version (a DLL left over
    from an older vcpkg) is accepted with a warning.  Microsoft's runtime
    DLLs need no notice.  oneTBB's DLLs, which extlib/oidn/build.ps1 copies
    in, are covered when `tbb_covered` (the OIDN install ships oneTBB's
    licence list); otherwise `tbb_root` (TBB_ROOT) must hold them, and its
    licence files are returned to ship as oneTBB.  Anything else is an
    error.  `skip` lists folders not to search (the notice folder).
    """
    if not os.path.isdir(dll_dir):
        raise NoticeError("DLL directory is missing: %s" % dll_dir)
    known_stems = {_dll_stem(n) for n in known_names}
    skip = {os.path.normcase(os.path.abspath(p)) for p in skip}
    unknown, stale, tbb = [], [], []
    for directory, subdirs, files in os.walk(dll_dir):
        subdirs[:] = [d for d in sorted(subdirs)
                      if os.path.normcase(os.path.abspath(os.path.join(directory, d))) not in skip
                      and not d.startswith(".")]
        for name in sorted(files):
            lower = name.lower()
            if not lower.endswith(".dll") or lower in known_names or _MS_RUNTIME.match(name):
                continue
            rel = os.path.relpath(os.path.join(directory, name), dll_dir)
            if _TBB_DLL.match(name):
                tbb.append(rel)
            elif _dll_stem(name) in known_stems:
                stale.append(rel)
            else:
                unknown.append(rel)
    for rel in stale:
        sys.stderr.write("third_party_notices: warning: %s looks like a DLL left over from an "
                         "older library version; its notice is shipped, but delete it if it "
                         "is stale\n" % os.path.join(dll_dir, rel))
    if unknown:
        raise NoticeError(
            "%s holds DLLs with no known licence source: %s.  Delete a DLL that is left "
            "over; otherwise add its install with --license-root PACKAGE=DIR (the "
            "RiseExtraNoticeArgs MSBuild property)" % (dll_dir, ", ".join(unknown)))
    if not tbb or tbb_covered:
        return []
    if not tbb_root:
        raise NoticeError(
            "%s holds oneTBB DLLs (%s) but no oneTBB licence is known: the OIDN install "
            "carries no third-party-programs-oneTBB file and TBB_ROOT is not set"
            % (dll_dir, ", ".join(tbb)))
    # extlib/oidn/build.ps1 copies them from these two folders only.
    tbb_names = set()
    for sub in (os.path.join("redist", "intel64", "vc14"), "bin"):
        folder = os.path.join(tbb_root, sub)
        if os.path.isdir(folder):
            tbb_names.update(f.lower() for f in os.listdir(folder))
    missing = [rel for rel in tbb if os.path.basename(rel).lower() not in tbb_names]
    if missing:
        raise NoticeError("oneTBB DLLs %s are not in TBB_ROOT %s" % (", ".join(missing), tbb_root))
    files = license_files_under(tbb_root)
    if len(files) > _MAX_TBB_NOTICES:
        raise NoticeError("%d licence files under TBB_ROOT %s is too many to be oneTBB's"
                          % (len(files), tbb_root))
    return [("oneTBB", path, os.path.relpath(path, tbb_root).replace(os.sep, "/"))
            for path in files]


def _check_destination(dest):
    """Refuse a `dest` that exists and was not made by this script."""
    if not os.path.lexists(dest):
        return
    if not os.path.isdir(dest) or os.path.islink(dest):
        raise NoticeError("destination exists and is not a folder: %s" % dest)
    if not os.listdir(dest):
        return
    index = os.path.join(dest, INDEX_NAME)
    marked = False
    if os.path.isfile(index):
        with open(index, "r", encoding="utf-8", errors="replace") as handle:
            marked = handle.readline().startswith(INDEX_MARKER)
    if not marked:
        raise NoticeError("destination is not empty and was not made by this script: %s" % dest)


def _same_tree(a, b):
    """True if folders `a` and `b` hold the same files with the same bytes."""
    def listing(root):
        found = {}
        for directory, _, files in os.walk(root):
            for name in files:
                path = os.path.join(directory, name)
                found[os.path.relpath(path, root)] = path
        return found
    if not os.path.isdir(a) or not os.path.isdir(b):
        return False
    left, right = listing(a), listing(b)
    if set(left) != set(right):
        return False
    for rel, path in left.items():
        with open(path, "rb") as x, open(right[rel], "rb") as y:
            if x.read() != y.read():
                return False
    return True


_LOCK_WAIT_SECONDS = 300


class _FolderLock(object):
    """Exclusive lock on one destination folder, held for a whole stage().

    The Windows RISE-CLI and RISE-GUI post-build steps stage into the same
    folder and can run at the same time.  The lock is an OS file lock
    (flock / msvcrt.locking), so a run that is killed releases it.  The
    lock file lives in the temp directory, keyed by the destination, so it
    never lands in a folder that gets shipped.
    """

    def __init__(self, dest):
        key = hashlib.sha256(os.path.normcase(dest).encode("utf-8")).hexdigest()[:16]
        self.path = os.path.join(tempfile.gettempdir(), "rise-notices-%s.lock" % key)
        self.handle = None

    def __enter__(self):
        self.handle = open(self.path, "a+b")
        if os.name == "nt":
            import msvcrt
            deadline = time.time() + _LOCK_WAIT_SECONDS
            while True:
                try:
                    self.handle.seek(0)
                    msvcrt.locking(self.handle.fileno(), msvcrt.LK_NBLCK, 1)
                    return self
                except OSError:
                    if time.time() > deadline:
                        self.handle.close()
                        raise NoticeError("timed out waiting for %s" % self.path)
                    time.sleep(0.1)
        import fcntl
        while True:
            fcntl.flock(self.handle.fileno(), fcntl.LOCK_EX)
            # The holder before us deletes the file as it unlocks; if the
            # file we locked is no longer the one at the path, lock again.
            try:
                if os.stat(self.path).st_ino == os.fstat(self.handle.fileno()).st_ino:
                    return self
            except FileNotFoundError:
                pass
            self.handle.close()
            self.handle = open(self.path, "a+b")

    def __exit__(self, *exc):
        if os.name == "nt":
            import msvcrt
            self.handle.seek(0)
            msvcrt.locking(self.handle.fileno(), msvcrt.LK_UNLCK, 1)
        else:
            import fcntl
            # Delete while still holding the lock (see __enter__).
            try:
                os.remove(self.path)
            except OSError:
                pass
            fcntl.flock(self.handle.fileno(), fcntl.LOCK_UN)
        self.handle.close()
        return False


def _staging_prefix(dest):
    return ".%s.staging-" % os.path.basename(dest)


def _sweep_staging(dest):
    """Remove staging folders a killed run left beside `dest` (lock held)."""
    parent, prefix = os.path.dirname(dest), _staging_prefix(dest)
    for name in os.listdir(parent):
        if name.startswith(prefix):
            shutil.rmtree(os.path.join(parent, name), ignore_errors=True)


def _publish(staging, dest):
    """Replace `dest` with the finished `staging` folder (lock held).

    The old folder is renamed aside first and deleted afterwards, so a
    file Windows keeps open in it (an indexer, antivirus) cannot leave
    `dest` half deleted; what fails to delete is swept on the next run.
    """
    if _same_tree(staging, dest):
        return
    if os.path.lexists(dest):
        trash = tempfile.mkdtemp(prefix=_staging_prefix(dest) + "old-",
                                 dir=os.path.dirname(dest))
        old = os.path.join(trash, "old")
        for attempt in range(20):
            try:
                os.rename(dest, old)
                break
            except OSError:
                if attempt == 19:
                    shutil.rmtree(trash, ignore_errors=True)
                    raise NoticeError("could not move the old %s aside (a file in it is "
                                      "in use?)" % dest)
                time.sleep(0.25)
        for attempt in range(20):
            try:
                os.rename(staging, dest)
                break
            except OSError:
                if attempt == 19:
                    try:
                        os.rename(old, dest)  # put the previous notices back
                    except OSError:
                        raise NoticeError("could not move the new notices into %s, nor the "
                                          "old ones back (they are in %s)" % (dest, old))
                    shutil.rmtree(trash, ignore_errors=True)
                    raise NoticeError("could not move the new notices into %s" % dest)
                time.sleep(0.25)
        shutil.rmtree(trash, ignore_errors=True)
    else:
        os.rename(staging, dest)


def stage(repo_root, distribution, dest, extra=(), lock=True):
    """Copy the notices for `distribution`, plus `extra`, into `dest`.

    `extra` holds (package, source path) or (package, source path, relative
    name) tuples.  A notice lands at <dest>/<package>/<relative name>, which
    defaults to the source's file name (a vcpkg `copyright` file becomes
    copyright.txt).  `dest` is replaced as a whole.  Returns
    {package: [relative names]}.
    """
    sources = []
    for item in list(manifest_files(repo_root, distribution)) + list(extra):
        package, source = item[0], item[1]
        if len(item) > 2:
            name = item[2]
        else:
            name = os.path.basename(source)
            if name == "copyright":
                name = "copyright.txt"
        name = name.replace("\\", "/")
        if not _PACKAGE_NAME.match(package):
            raise NoticeError("bad package name %r" % package)
        if os.path.isabs(name) or ".." in name.split("/") or not name:
            raise NoticeError("bad notice name %r for %s" % (name, package))
        if not os.path.isfile(source):
            raise NoticeError("third-party notice for %s is missing: %s" % (package, source))
        sources.append((package, source, name))
    dest = os.path.abspath(dest)
    parent = os.path.dirname(dest)
    os.makedirs(parent, exist_ok=True)
    # `lock` False is for a destination private to the caller.
    with (_FolderLock(dest) if lock else _NoLock()):
        _check_destination(dest)
        _sweep_staging(dest)
        staging = tempfile.mkdtemp(prefix=_staging_prefix(dest), dir=parent)
        return _stage_into(sources, distribution, staging, dest)


class _NoLock(object):
    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False


def _stage_into(sources, distribution, staging, dest):
    try:
        staged = {}
        for package, source, name in sources:
            target = os.path.join(staging, package, *name.split("/"))
            if name in staged.get(package, []):
                # Two different files with one name in one package would
                # silently overwrite each other; identical repeats are fine.
                with open(source, "rb") as a, open(target, "rb") as b:
                    if a.read() != b.read():
                        raise NoticeError("two different %s notices are both named %s"
                                          % (package, name))
                continue
            os.makedirs(os.path.dirname(target), exist_ok=True)
            shutil.copyfile(source, target)
            staged.setdefault(package, []).append(name)
        _write_index(staging, distribution, staged)
        _publish(staging, dest)
    finally:
        if os.path.isdir(staging):
            shutil.rmtree(staging, ignore_errors=True)
    return staged


def _write_index(dest, distribution, staged):
    lines = [
        "%s (%s)" % (INDEX_MARKER, distribution),
        "",
        "This build of RISE contains or is distributed with the third-party",
        "software listed below.  Each folder holds that software's licence",
        "and copyright notices.  RISE's own licence is in the RISE folder",
        "where present, and in LICENSE.TXT of the RISE source tree.",
        "",
    ]
    for package in sorted(staged, key=str.lower):
        lines.append("%s/" % package)
        for name in staged[package]:
            lines.append("    %s" % name)
    with open(os.path.join(dest, INDEX_NAME), "w", encoding="utf-8", newline="\n") as handle:
        handle.write("\n".join(lines) + "\n")


def _parse_pair(text, flag):
    package, sep, value = text.partition("=")
    if not sep or not package or not value:
        raise NoticeError("%s expects PACKAGE=PATH, got %r" % (flag, text))
    return package, value


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--distribution", required=True, choices=DISTRIBUTIONS)
    parser.add_argument("--dest", required=True, help="folder to (re)create")
    parser.add_argument("--repo-root", default=default_repo_root())
    parser.add_argument("--vcpkg-installed",
                        help="vcpkg install root (holds vcpkg/info and <triplet>/)")
    parser.add_argument("--vcpkg-triplet")
    parser.add_argument("--license-root", action="append", default=[],
                        metavar="PACKAGE=DIR",
                        help="ship every licence file under an install prefix")
    parser.add_argument("--license-file", action="append", default=[],
                        metavar="PACKAGE=FILE", help="ship one extra licence file")
    parser.add_argument("--check-dlls", metavar="DIR",
                        help="fail if DIR (searched recursively) holds a DLL whose "
                             "notice is not shipped")
    args = parser.parse_args(argv)
    try:
        extra = []
        if bool(args.vcpkg_installed) != bool(args.vcpkg_triplet):
            raise NoticeError("--vcpkg-installed and --vcpkg-triplet go together")
        if args.vcpkg_installed:
            extra.extend(vcpkg_copyrights(args.vcpkg_installed, args.vcpkg_triplet))
        known_dlls = set()
        tbb_covered = False
        if args.vcpkg_installed:
            known_dlls |= vcpkg_dll_names(args.vcpkg_installed, args.vcpkg_triplet)
        for text in args.license_root:
            package, root = _parse_pair(text, "--license-root")
            notices = license_files_under(root)
            extra.extend((package, path, os.path.relpath(path, root).replace(os.sep, "/"))
                         for path in notices)
            tbb_covered |= any(_TBB_NOTICE.match(os.path.basename(p)) for p in notices)
            for directory, _, files in os.walk(root):
                known_dlls.update(f.lower() for f in files if f.lower().endswith(".dll"))
        if args.check_dlls:
            extra.extend(audit_dlls(args.check_dlls, known_dlls, os.environ.get("TBB_ROOT"),
                                    tbb_covered, skip=[args.dest]))
        for text in args.license_file:
            extra.append(_parse_pair(text, "--license-file"))
        staged = stage(args.repo_root, args.distribution, args.dest, extra)
    except (NoticeError, OSError) as exc:
        sys.stderr.write("third_party_notices: error: %s\n" % exc)
        return 1
    count = sum(len(names) for names in staged.values())
    print("third_party_notices: staged %d notices for %d packages into %s"
          % (count, len(staged), args.dest))
    return 0


if __name__ == "__main__":
    sys.exit(main())
