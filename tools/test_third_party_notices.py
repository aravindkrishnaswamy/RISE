#!/usr/bin/env python3
"""Tests for the third-party notice packaging (licenses/README.md).

  python3 tools/test_third_party_notices.py

Covers the manifest, which every packager reads; the stager used by the
Windows post-build and the Blender add-on zip; the macOS release script's
manifest loop; and the coverage rule that every extlib/ directory a build
compiles has a notice for that build.  The Android Gradle task is exercised
by building the app (it fails the build when a notice is missing).
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import unittest
import zipfile

TOOLS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS)
import package_blender_addon  # noqa: E402
import third_party_notices as tpn  # noqa: E402

REPO = tpn.default_repo_root()

# Notices every distribution must carry: code compiled into the library
# on every platform.
CORE_PACKAGES = {"stb", "cgltf", "mt19937", "Sobol-Joe-Kuo", "Cycles"}
CYCLES_FILES = {"Cycles-NOTICE.txt", "Apache-2.0.txt", "BSD-3-Clause.txt"}

# Which build file decides what a distribution compiles.  The macOS
# release and the Blender bridge both compile the library's Filelist
# sources (the Xcode project mirrors that list).
BUILD_FILES = {
    "android": ["build/cmake/rise-android/rise_sources.cmake",
                "build/cmake/rise-android/extlib_png.cmake"],
    "windows": ["build/VS2022/Library/Library.vcxproj"],
    "macos": ["build/make/rise/Filelist"],
    "blender": ["build/make/rise/Filelist"],
}
# extlib/ directories whose notices come from an install tree, not the
# manifest (the packagers look them up at packaging time).
EXTERNAL_EXTLIB = {"oidn"}


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(text)


class ManifestTest(unittest.TestCase):
    def test_every_listed_file_exists(self):
        for package, relpath, _ in tpn.read_manifest(REPO):
            self.assertTrue(os.path.isfile(os.path.join(REPO, relpath)),
                            "%s notice missing: %s" % (package, relpath))

    def test_core_notices_reach_every_distribution(self):
        for dist in tpn.DISTRIBUTIONS:
            files = tpn.manifest_files(REPO, dist)
            packages = {package for package, _ in files}
            self.assertTrue(CORE_PACKAGES <= packages, "%s lacks %s"
                            % (dist, sorted(CORE_PACKAGES - packages)))
            cycles = {os.path.basename(p) for package, p in files if package == "Cycles"}
            self.assertEqual(cycles, CYCLES_FILES, dist)

    def test_compiled_extlib_code_has_a_notice(self):
        pattern = re.compile(r"extlib[\\/]+([A-Za-z0-9_-]+)[\\/]")
        for dist, build_files in BUILD_FILES.items():
            packages = {package for package, _ in tpn.manifest_files(REPO, dist)}
            for build_file in build_files:
                with open(os.path.join(REPO, build_file), "r", encoding="utf-8") as handle:
                    text = handle.read()
                # Only source files count: include paths name install trees.
                compiled = set()
                for line in text.splitlines():
                    if re.search(r"\.(c|cc|cpp)\b", line) or "_DIR}/" in line:
                        compiled.update(pattern.findall(line))
                compiled.update(re.findall(r"\$\{EXTLIB_DIR\}/([A-Za-z0-9_-]+)", text))
                for name in sorted(compiled - EXTERNAL_EXTLIB):
                    self.assertIn(name, packages,
                                  "%s compiles extlib/%s (%s) but licenses/third_party_notices.tsv "
                                  "has no %s notice for it" % (dist, name, build_file, dist))

    def test_malformed_manifest_is_rejected(self):
        root = tempfile.mkdtemp()
        try:
            for bad in ("pkg\tLICENSE\n", "pkg\tLICENSE\tlinux\n", "pkg\t../x\tall\n"):
                write(os.path.join(root, tpn.MANIFEST_RELPATH), bad)
                with self.assertRaises(tpn.NoticeError, msg=bad):
                    tpn.read_manifest(root)
        finally:
            shutil.rmtree(root)


class StageTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.dest = os.path.join(self.tmp, "out", "Third-Party Licenses")

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def test_stage_real_manifest(self):
        staged = tpn.stage(REPO, "windows", self.dest)
        self.assertEqual(set(staged["Cycles"]), CYCLES_FILES)
        for package, names in staged.items():
            for name in names:
                self.assertTrue(os.path.isfile(os.path.join(self.dest, package, name)))
        with open(os.path.join(self.dest, tpn.INDEX_NAME), encoding="utf-8") as handle:
            self.assertTrue(handle.readline().startswith(tpn.INDEX_MARKER))
        # No staging folder is left beside the result.
        self.assertEqual(os.listdir(os.path.dirname(self.dest)), ["Third-Party Licenses"])

    def test_missing_notice_fails_and_keeps_previous_folder(self):
        tpn.stage(REPO, "blender", self.dest)
        before = sorted(os.listdir(self.dest))
        with self.assertRaises(tpn.NoticeError):
            tpn.stage(REPO, "blender", self.dest,
                      extra=[("Ghost", os.path.join(self.tmp, "no-such-LICENSE"))])
        self.assertEqual(sorted(os.listdir(self.dest)), before)

    def test_refuses_foreign_folder(self):
        write(os.path.join(self.dest, "precious.txt"), "keep me")
        with self.assertRaises(tpn.NoticeError):
            tpn.stage(REPO, "windows", self.dest)
        self.assertTrue(os.path.isfile(os.path.join(self.dest, "precious.txt")))

    def test_restage_drops_stale_files(self):
        tpn.stage(REPO, "windows", self.dest)
        write(os.path.join(self.dest, "stale", "LICENSE"), "old")
        tpn.stage(REPO, "windows", self.dest)
        self.assertFalse(os.path.exists(os.path.join(self.dest, "stale")))

    def test_conflicting_names_in_one_package_fail(self):
        a, b = os.path.join(self.tmp, "a", "LICENSE"), os.path.join(self.tmp, "b", "LICENSE")
        write(a, "one")
        write(b, "two")
        with self.assertRaises(tpn.NoticeError):
            tpn.stage(REPO, "windows", self.dest, extra=[("X", a), ("X", b)])

    def test_concurrent_stages_into_one_folder(self):
        # RISE-CLI and RISE-GUI post-build steps share $(OutDir).
        errors = []

        def run():
            try:
                tpn.stage(REPO, "windows", self.dest)
            except Exception as exc:  # pragma: no cover - reported below
                errors.append(exc)
        threads = [threading.Thread(target=run) for _ in range(6)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        self.assertEqual(errors, [])
        reference = os.path.join(self.tmp, "reference")
        tpn.stage(REPO, "windows", reference)
        self.assertTrue(tpn._same_tree(reference, self.dest))
        self.assertEqual(os.listdir(os.path.dirname(self.dest)), ["Third-Party Licenses"])


class ExternalSourcesTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def make_vcpkg(self, ports, triplet="x64-windows"):
        root = os.path.join(self.tmp, "vcpkg_installed", triplet)
        for port, has_copyright in ports:
            lines = ["%s/" % triplet, "%s/include/%s.h" % (triplet, port)]
            if has_copyright:
                lines.append("%s/share/%s/copyright" % (triplet, port))
                write(os.path.join(root, triplet, "share", port, "copyright"), port + " licence")
            write(os.path.join(root, "vcpkg", "info", "%s_1.0_%s.list" % (port, triplet)),
                  "\n".join(lines) + "\n")
        # A port of another triplet is ignored.
        write(os.path.join(root, "vcpkg", "info", "zlib_1.3_x64-linux.list"), "x64-linux/\n")
        return root

    def test_vcpkg_ports(self):
        root = self.make_vcpkg([("qtbase", True), ("ffmpeg", True), ("vcpkg-cmake", False)])
        found = dict(tpn.vcpkg_copyrights(root, "x64-windows"))
        self.assertEqual(set(found), {"qtbase", "ffmpeg"})
        dest = os.path.join(self.tmp, "dest")
        tpn.stage(REPO, "windows", dest, extra=list(found.items()))
        self.assertTrue(os.path.isfile(os.path.join(dest, "qtbase", "copyright.txt")))

    def test_vcpkg_port_without_copyright_fails(self):
        root = self.make_vcpkg([("qtbase", True), ("mystery", False)])
        with self.assertRaises(tpn.NoticeError):
            tpn.vcpkg_copyrights(root, "x64-windows")

    def test_vcpkg_missing_tree_fails(self):
        with self.assertRaises(tpn.NoticeError):
            tpn.vcpkg_copyrights(os.path.join(self.tmp, "none"), "x64-windows")

    def test_license_root(self):
        root = os.path.join(self.tmp, "oidn")
        write(os.path.join(root, "share", "doc", "OpenImageDenoise", "LICENSE.txt"), "apache")
        write(os.path.join(root, "share", "doc", "OpenImageDenoise",
                           "third-party-programs-oneTBB.txt"), "tbb")
        write(os.path.join(root, "lib", "libOpenImageDenoise.so"), "binary")
        found = tpn.license_files_under(root)
        self.assertEqual(sorted(os.path.basename(p) for p in found),
                         ["LICENSE.txt", "third-party-programs-oneTBB.txt"])
        empty = os.path.join(self.tmp, "empty")
        os.makedirs(empty)
        with self.assertRaises(tpn.NoticeError):
            tpn.license_files_under(empty)

    def test_command_line_fails_closed(self):
        script = os.path.join(TOOLS, "third_party_notices.py")
        dest = os.path.join(self.tmp, "cli")
        ok = subprocess.run([sys.executable, script, "--distribution", "windows",
                             "--dest", dest], capture_output=True, text=True)
        self.assertEqual(ok.returncode, 0, ok.stderr)
        bad = subprocess.run([sys.executable, script, "--distribution", "windows",
                              "--dest", dest, "--license-root",
                              "openpgl=" + os.path.join(self.tmp, "no-openpgl")],
                             capture_output=True, text=True)
        self.assertNotEqual(bad.returncode, 0)
        self.assertIn("openpgl", bad.stderr)


class MacOSReleaseLoopTest(unittest.TestCase):
    """Runs the manifest loop of scripts/create_macos_release.sh."""

    def extract_loop(self):
        with open(os.path.join(REPO, "scripts", "create_macos_release.sh"), encoding="utf-8") as h:
            text = h.read()
        start = text.index('NOTICE_MANIFEST="${SOURCE_ROOT}')
        end = text.index("\n", text.index("lists no notices for macos"))
        return text[start:end]

    def run_loop(self, source_root):
        app = tempfile.mkdtemp()
        script = (
            'set -euo pipefail\n'
            'die() { echo "error: $*" >&2; exit 1; }\n'
            'copy_static_license() {\n'
            '  [ -f "$1" ] || die "in-tree third-party notice is missing: $1"\n'
            '  mkdir -p "$APP/$2"; cp "$1" "$APP/$2/"\n'
            '}\n' + self.extract_loop() + "\n")
        result = subprocess.run(["bash", "-c", script],
                                env=dict(os.environ, SOURCE_ROOT=source_root, APP=app),
                                capture_output=True, text=True)
        return result, app

    def test_loop_copies_macos_entries(self):
        result, app = self.run_loop(REPO)
        try:
            self.assertEqual(result.returncode, 0, result.stderr)
            expected = {(package, os.path.basename(path))
                        for package, path in tpn.manifest_files(REPO, "macos")}
            copied = {(package, name) for package in os.listdir(app)
                      for name in os.listdir(os.path.join(app, package))}
            self.assertEqual(copied, expected)
        finally:
            shutil.rmtree(app)

    def test_loop_fails_on_missing_notice(self):
        root = tempfile.mkdtemp()
        try:
            write(os.path.join(root, tpn.MANIFEST_RELPATH),
                  "# comment\tline\nstb\textlib/stb/LICENSE.txt\tall\n")
            result, app = self.run_loop(root)
            shutil.rmtree(app)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("missing", result.stderr)
        finally:
            shutil.rmtree(root)


class WindowsProjectTest(unittest.TestCase):
    def test_both_executables_stage_notices_in_every_configuration(self):
        for project in ("RISE-CLI/RISE-CLI.vcxproj", "RISE-GUI/RISE-GUI.vcxproj"):
            with open(os.path.join(REPO, "build", "VS2022", project), encoding="utf-8") as h:
                text = h.read()
            self.assertEqual(text.count("third_party_notices.py"
                                        '" --distribution windows'), 2, project)
            self.assertEqual(text.count("--vcpkg-installed"), 2, project)
            self.assertEqual(text.count("open-image-denoise="), 2, project)
            self.assertEqual(text.count("openpgl=$(RiseOpenPglRoot)"), 2, project)


class BlenderPackageTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.bridge = os.path.join(self.tmp, package_blender_addon.bridge_filename())
        write(self.bridge, "not really a library")
        self.output = os.path.join(self.tmp, "dist", "rise_renderer.zip")

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def test_zip_carries_addon_bridge_and_notices(self):
        staged = package_blender_addon.package(REPO, self.bridge, self.output)
        with zipfile.ZipFile(self.output) as archive:
            names = set(archive.namelist())
        prefix = "rise_renderer/third_party_licenses/"
        for package, notices in staged.items():
            for notice in notices:
                self.assertIn(prefix + package + "/" + notice, names)
        self.assertTrue(CORE_PACKAGES <= set(staged))
        self.assertIn("rise_renderer/__init__.py", names)
        self.assertIn("rise_renderer/bridge.py", names)
        self.assertIn("rise_renderer/native/" + package_blender_addon.bridge_filename(), names)
        self.assertFalse([n for n in names if os.path.basename(n).startswith("test_")])

    def test_missing_bridge_writes_no_zip(self):
        os.remove(self.bridge)
        with self.assertRaises(tpn.NoticeError):
            package_blender_addon.package(REPO, self.bridge, self.output)
        self.assertFalse(os.path.exists(self.output))

    def test_missing_notice_writes_no_zip(self):
        root = os.path.join(self.tmp, "repo")
        shutil.copytree(os.path.join(REPO, "src", "Blender", "addons", "rise_renderer"),
                        os.path.join(root, "src", "Blender", "addons", "rise_renderer"))
        write(os.path.join(root, tpn.MANIFEST_RELPATH), "stb\textlib/stb/LICENSE.txt\tall\n")
        with self.assertRaises(tpn.NoticeError):
            package_blender_addon.package(root, self.bridge, self.output)
        self.assertFalse(os.path.exists(self.output))
        self.assertFalse(os.path.exists(self.output + ".partial"))


if __name__ == "__main__":
    unittest.main()
