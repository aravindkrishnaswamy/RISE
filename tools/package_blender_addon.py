#!/usr/bin/env python3
"""Package the RISE Blender add-on, its native bridge and its notices as a zip.

The zip installs through Blender's Preferences > Add-ons > Install from
Disk.  Layout:

  rise_renderer/                   the add-on (test_*.py left out)
  rise_renderer/native/            the bridge library (bridge.py looks here)
  rise_renderer/third_party_licenses/
                                   the notices licenses/third_party_notices.tsv
                                   lists for "blender", with an index

Fails, writing no zip, if the bridge library or any notice is missing.

The bridge links its image, denoising, guiding and OpenVDB libraries
dynamically from the build machine (Homebrew, extlib/oidn/install, a
Blender library tree) and compiles some of their header code inline.
Those libraries are not put in the zip, but their notices are: each
dependency is resolved to its install prefix and that prefix's licence
files are shipped.  A dependency with no licence file found fails the
packaging; point at one with --license-root NAME=DIR, where NAME is the
library's name as the error reports it.  The zip runs only where those
libraries sit at the paths the bridge was linked against.

  make -C src/Blender/native
  python3 tools/package_blender_addon.py            # -> dist/rise_renderer-<ver>-<os>.zip
"""

import argparse
import glob
import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import third_party_notices  # noqa: E402

ADDON_NAME = "rise_renderer"
NOTICES_DIR = "third_party_licenses"


def bridge_filename():
    if sys.platform == "darwin":
        return "rise_blender_bridge.dylib"
    if os.name == "nt":
        return "rise_blender_bridge.dll"
    return "rise_blender_bridge.so"


def addon_version(addon_dir):
    with open(os.path.join(addon_dir, "__init__.py"), "r", encoding="utf-8") as handle:
        match = re.search(r'"version"\s*:\s*\(([^)]*)\)', handle.read())
    if not match:
        raise third_party_notices.NoticeError("no bl_info version in %s/__init__.py" % addon_dir)
    return ".".join(part.strip() for part in match.group(1).split(","))


_MAX_NOTICES_PER_LIBRARY = 12
# The C / C++ runtime every Linux program loads from the system; anything
# else in /usr/lib (a distribution's libpng, OpenEXR, tbb) is third-party
# code whose notice the zip must carry.
_LINUX_RUNTIME = re.compile(
    r"^(libc|libm|libmvec|libdl|librt|libpthread|libutil|libresolv|libstdc\+\+|libgcc_s|"
    r"libgomp|libatomic|"
    r"ld-linux[^/]*|linux-vdso)\.so")
_HOMEBREW_CELLAR = re.compile(r"^(?P<prefix>.*)/Cellar/(?P<formula>[^/]+)/[^/]+/")


def _macos_dependencies(bridge):
    """Physical paths of the non-system dylibs the bridge links."""
    out = subprocess.run(["otool", "-L", bridge], capture_output=True,
                         text=True, check=True).stdout.splitlines()[1:]
    names = [line.split()[0] for line in out if line.strip()]
    load = subprocess.run(["otool", "-l", bridge], capture_output=True,
                          text=True, check=True).stdout.splitlines()
    rpaths, want = [], False
    for line in load:
        line = line.strip()
        if line == "cmd LC_RPATH":
            want = True
        elif want and line.startswith("path "):
            rpaths.append(line.split()[1])
            want = False
    here = os.path.dirname(os.path.abspath(bridge))
    rpaths = [r.replace("@loader_path", here).replace("@executable_path", here) for r in rpaths]
    own = os.path.basename(bridge)
    found = []
    for name in names:
        if os.path.basename(name) == own or name.startswith(("/usr/lib/", "/System/")):
            continue
        if name.startswith("@rpath/"):
            candidates = [os.path.join(r, name[len("@rpath/"):]) for r in rpaths]
        elif name.startswith("@loader_path/"):
            candidates = [os.path.join(here, name[len("@loader_path/"):])]
        else:
            candidates = [name]
        resolved = next((c for c in candidates if os.path.exists(c)), None)
        if resolved is None:
            raise third_party_notices.NoticeError(
                "cannot resolve the bridge's dependency %s" % name)
        found.append(os.path.realpath(resolved))
    return found


def _linux_dependencies(bridge):
    out = subprocess.run(["ldd", bridge], capture_output=True,
                         text=True, check=True).stdout.splitlines()
    found = []
    for line in out:
        if "=>" not in line:
            continue
        path = line.split("=>", 1)[1].split("(")[0].strip()
        if not path or path == "not found":
            raise third_party_notices.NoticeError("bridge dependency unresolved: %s" % line.strip())
        if not _LINUX_RUNTIME.match(os.path.basename(path)):
            found.append(os.path.realpath(path))
    return found


def external_dependencies(bridge):
    """Physical paths of the non-system shared libraries the bridge links."""
    try:
        if sys.platform == "darwin":
            return _macos_dependencies(bridge)
        if sys.platform.startswith("linux"):
            return _linux_dependencies(bridge)
    except (OSError, subprocess.CalledProcessError) as exc:
        raise third_party_notices.NoticeError("cannot list the bridge's dependencies: %s" % exc)
    raise third_party_notices.NoticeError(
        "listing a bridge's dependencies is not supported on %s" % sys.platform)


def library_name(path):
    """libOpenImageDenoise.2.4.1.dylib -> OpenImageDenoise, libtbb.so.12 -> tbb."""
    name = os.path.basename(path)
    if name.startswith("lib"):
        name = name[3:]
    name = re.split(r"\.(?:dylib|so|dll)\b", name)[0]
    return re.sub(r"[.-][0-9][0-9._-]*$", "", name) or name


def _known_roots(repo_root):
    """Install prefixes whose licence location is known, as {root: package}."""
    return {os.path.realpath(os.path.join(repo_root, "extlib", "oidn", "install")):
            "OpenImageDenoise"}


def dependency_notices(bridge, overrides, repo_root=None):
    """[(package, licence path, relative name)] for the bridge's dependencies.

    A Homebrew library maps to its formula, as in the macOS release
    script; libraries in extlib/oidn/install to OIDN.  Any other library
    needs --license-root NAME=DIR (`overrides`): a shared prefix such as
    /usr or /usr/local holds many packages' licences, and nothing tells
    which are this library's.
    """
    repo_root = repo_root or third_party_notices.default_repo_root()
    known = _known_roots(repo_root)
    groups = {}
    for path in external_dependencies(bridge):
        match = _HOMEBREW_CELLAR.match(path)
        prefix = os.path.dirname(os.path.dirname(path))
        root = None
        if match:
            package = match.group("formula")
            root = os.path.join(match.group("prefix"), "opt", package)
        elif os.path.isdir(os.path.join(prefix, "Cellar")):
            # A file directly in a Homebrew prefix's lib/ that is not a
            # symlink into the Cellar (openpgl): find the formula that owns
            # it, else the documentation folder named after it.
            owners = sorted(glob.glob(os.path.join(prefix, "Cellar", "*", "*", "lib",
                                                   os.path.basename(path))))
            if owners:
                package = owners[0][len(prefix) + len("/Cellar/"):].split("/")[0]
                root = os.path.join(prefix, "opt", package)
            else:
                package = library_name(path)
                root = os.path.join(prefix, "share", "doc", package)
        elif os.path.realpath(prefix) in known:
            package = known[os.path.realpath(prefix)]
            root = prefix
        else:
            package = library_name(path)
        if package in overrides:
            root = overrides[package]
        if root is None:
            raise third_party_notices.NoticeError(
                "do not know where the licence of %s (%s) is; pass --license-root %s=DIR"
                % (package, path, package))
        groups.setdefault(package, (path, root))
    notices = []
    for package, (path, root) in sorted(groups.items()):
        try:
            files = third_party_notices.license_files_under(root)
        except third_party_notices.NoticeError:
            raise third_party_notices.NoticeError(
                "no licence file found for %s (%s) under %s; pass --license-root %s=DIR"
                % (package, path, root, package))
        if len(files) > _MAX_NOTICES_PER_LIBRARY:
            raise third_party_notices.NoticeError(
                "%d licence files under %s for %s is too many to be one library's; "
                "pass --license-root %s=DIR" % (len(files), root, package, package))
        notices.extend((package, f, os.path.relpath(f, root).replace(os.sep, "/")) for f in files)
    unused = sorted(set(overrides) - set(groups))
    if unused:
        raise third_party_notices.NoticeError(
            "--license-root given for %s, which the bridge does not link" % ", ".join(unused))
    return notices


def _add_tree(archive, root, arc_root):
    for directory, subdirs, files in os.walk(root):
        subdirs.sort()
        for name in sorted(files):
            path = os.path.join(directory, name)
            arcname = os.path.join(arc_root, os.path.relpath(path, root)).replace(os.sep, "/")
            info = zipfile.ZipInfo(arcname, date_time=(2026, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            mode = 0o755 if os.access(path, os.X_OK) else 0o644
            info.external_attr = (0o100000 | mode) << 16
            with open(path, "rb") as handle:
                archive.writestr(info, handle.read())


def package(repo_root, bridge, output, dependency_licenses=None):
    addon_src = os.path.join(repo_root, "src", "Blender", "addons", ADDON_NAME)
    if not os.path.isfile(os.path.join(addon_src, "__init__.py")):
        raise third_party_notices.NoticeError("add-on source is missing: %s" % addon_src)
    if not os.path.isfile(bridge):
        raise third_party_notices.NoticeError(
            "bridge library is missing: %s (build it with make -C src/Blender/native)" % bridge)
    if os.path.basename(bridge) != bridge_filename():
        raise third_party_notices.NoticeError(
            "bridge %s is not named %s, so bridge.py would not find it"
            % (bridge, bridge_filename()))

    staging = tempfile.mkdtemp(prefix="rise_addon_")
    try:
        addon_dst = os.path.join(staging, ADDON_NAME)
        os.makedirs(addon_dst)
        for name in sorted(os.listdir(addon_src)):
            path = os.path.join(addon_src, name)
            if name == "__pycache__" or name.startswith("test_") or name.startswith("."):
                continue
            if not (name.endswith(".py") and os.path.isfile(path)):
                # A subpackage or data file would need copying too; refuse
                # rather than ship an add-on without it.
                raise third_party_notices.NoticeError(
                    "the packager copies only top-level .py files; extend it for %s" % path)
            shutil.copyfile(path, os.path.join(addon_dst, name))
        native_dst = os.path.join(addon_dst, "native")
        os.makedirs(native_dst)
        shutil.copyfile(bridge, os.path.join(native_dst, bridge_filename()))
        os.chmod(os.path.join(native_dst, bridge_filename()), 0o755)
        # None: work them out from the bridge.  Tests pass a list.
        if dependency_licenses is None:
            dependency_licenses = dependency_notices(bridge, {}, repo_root)
        # The staging folder is private to this run: no lock needed.
        staged = third_party_notices.stage(
            repo_root, "blender", os.path.join(addon_dst, NOTICES_DIR), dependency_licenses,
            lock=False)

        os.makedirs(os.path.dirname(os.path.abspath(output)), exist_ok=True)
        partial = output + ".partial"
        with zipfile.ZipFile(partial, "w") as archive:
            _add_tree(archive, addon_dst, ADDON_NAME)
        # Post-condition: the zip really carries every staged notice.
        with zipfile.ZipFile(partial) as archive:
            names = set(archive.namelist())
        expected = ["%s/%s/%s/%s" % (ADDON_NAME, NOTICES_DIR, package_name, notice)
                    for package_name, notices in staged.items() for notice in notices]
        expected.append("%s/%s/%s" % (ADDON_NAME, NOTICES_DIR, third_party_notices.INDEX_NAME))
        expected.append("%s/native/%s" % (ADDON_NAME, bridge_filename()))
        missing = [name for name in expected if name not in names]
        if missing:
            raise third_party_notices.NoticeError("zip is missing %s" % ", ".join(missing))
        os.replace(partial, output)
    finally:
        shutil.rmtree(staging, ignore_errors=True)
        if os.path.exists(output + ".partial"):
            os.remove(output + ".partial")
    return staged


def main(argv=None):
    repo_root = third_party_notices.default_repo_root()
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--bridge",
                        default=os.path.join(repo_root, "src", "Blender", "native", bridge_filename()))
    parser.add_argument("--output")
    parser.add_argument("--license-root", action="append", default=[], metavar="NAME=DIR",
                        help="where to find the licence of a linked library")
    args = parser.parse_args(argv)
    try:
        overrides = dict(third_party_notices._parse_pair(text, "--license-root")
                         for text in args.license_root)
        bridge = os.path.abspath(args.bridge)
        output = args.output
        if not output:
            version = addon_version(os.path.join(repo_root, "src", "Blender", "addons", ADDON_NAME))
            system = {"darwin": "macos", "win32": "windows"}.get(sys.platform, sys.platform)
            output = os.path.join(repo_root, "dist", "%s-%s-%s-%s.zip"
                                  % (ADDON_NAME, version, system, platform.machine().lower()))
        if not os.path.isfile(bridge):
            raise third_party_notices.NoticeError(
                "bridge library is missing: %s (build it with make -C src/Blender/native)" % bridge)
        dep_paths = external_dependencies(bridge)
        deps = dependency_notices(bridge, overrides, repo_root)
        staged = package(repo_root, bridge, os.path.abspath(output), deps)
    except (third_party_notices.NoticeError, OSError) as exc:
        sys.stderr.write("package_blender_addon: error: %s\n" % exc)
        return 1
    print("package_blender_addon: wrote %s (%d notice packages)" % (output, len(staged)))
    print("package_blender_addon: the bridge loads its libraries from these paths, which the "
          "target machine must have; their notices are in the zip:")
    for path in dep_paths:
        print("    " + path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
