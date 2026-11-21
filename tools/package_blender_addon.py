#!/usr/bin/env python3
"""Package the RISE Blender add-on, its native bridge and its notices as a zip.

The zip installs through Blender's Preferences > Add-ons > Install from
Disk.  Layout:

  rise_renderer/                   the add-on (test_*.py left out)
  rise_renderer/native/            the bridge library (bridge.py looks here)
  rise_renderer/third_party_licenses/
                                   the notices licenses/third_party_notices.tsv
                                   lists for "blender", with an index

Fails, writing no zip, if the bridge library or any listed notice is
missing.  The bridge links its image / denoising / guiding / OpenVDB
libraries dynamically from the build machine (Homebrew on macOS); those
libraries are not put in the zip, so their notices are not either.  The
script lists them so a packager knows the zip expects them on the target
machine.

  make -C src/Blender/native
  python3 tools/package_blender_addon.py            # -> dist/rise_renderer-<ver>-<os>.zip
"""

import argparse
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


def external_dependencies(bridge):
    """Non-system shared libraries the bridge loads (best effort, for the log)."""
    try:
        if sys.platform == "darwin":
            out = subprocess.run(["otool", "-L", bridge], capture_output=True,
                                 text=True, check=True).stdout.splitlines()[1:]
            deps = [line.split()[0] for line in out if line.strip()]
            # A dylib's first entry is its own install name.
            own = os.path.basename(bridge)
            return [d for d in deps
                    if os.path.basename(d) != own
                    and not d.startswith(("/usr/lib/", "/System/"))]
        if sys.platform.startswith("linux"):
            out = subprocess.run(["ldd", bridge], capture_output=True,
                                 text=True, check=True).stdout.splitlines()
            deps = [line.split("=>")[-1].split("(")[0].strip() for line in out]
            return [d for d in deps if d and not d.startswith(("/lib", "/usr/lib", "linux-vdso"))]
    except (OSError, subprocess.CalledProcessError):
        return None
    return None


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


def package(repo_root, bridge, output):
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
            if name.endswith(".py") and not name.startswith("test_") and os.path.isfile(path):
                shutil.copyfile(path, os.path.join(addon_dst, name))
        native_dst = os.path.join(addon_dst, "native")
        os.makedirs(native_dst)
        shutil.copyfile(bridge, os.path.join(native_dst, bridge_filename()))
        os.chmod(os.path.join(native_dst, bridge_filename()), 0o755)
        staged = third_party_notices.stage(
            repo_root, "blender", os.path.join(addon_dst, NOTICES_DIR))

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
    args = parser.parse_args(argv)
    try:
        output = args.output
        if not output:
            version = addon_version(os.path.join(repo_root, "src", "Blender", "addons", ADDON_NAME))
            system = {"darwin": "macos", "win32": "windows"}.get(sys.platform, sys.platform)
            output = os.path.join(repo_root, "dist", "%s-%s-%s-%s.zip"
                                  % (ADDON_NAME, version, system, platform.machine().lower()))
        staged = package(repo_root, os.path.abspath(args.bridge), os.path.abspath(output))
    except (third_party_notices.NoticeError, OSError) as exc:
        sys.stderr.write("package_blender_addon: error: %s\n" % exc)
        return 1
    print("package_blender_addon: wrote %s (%d notice packages)" % (output, len(staged)))
    deps = external_dependencies(args.bridge)
    if deps:
        print("package_blender_addon: note: the bridge loads these libraries from the build "
              "machine; they are not in the zip and must be present where it is installed:")
        for dep in deps:
            print("    " + dep)
    return 0


if __name__ == "__main__":
    sys.exit(main())
