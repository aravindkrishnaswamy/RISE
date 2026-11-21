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

# Which build file decides what a distribution compiles.  The Blender
# bridge links the make Filelist's library; the macOS release builds the
# Xcode project, whose group-relative paths this check cannot read, so it
# is checked against the Filelist that project mirrors.
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

# Source files with a third-party copyright or licence line, and the
# manifest package that carries their notice.  OslNoise is ported from
# Cycles; it may not be on every branch yet.
SOURCE_NOTICES = {
    "src/Library/Sampling/SobolDirectionNumbers.cpp": "Sobol-Joe-Kuo",
    "src/Library/Utilities/MersenneTwister.cpp": "mt19937",
    "src/Library/Utilities/MersenneTwister.h": "mt19937",
    "src/Library/Utilities/UnicodeNFCData.inc": "Unicode",
    "src/Library/Utilities/OslNoise.cpp": "Cycles",
    "src/Library/Utilities/OslNoise.h": "Cycles",
}
# Third-party code whose terms a notice cannot satisfy; see "Not covered
# by these notices" in licenses/README.md.
UNRESOLVED = {
    "src/Library/Functions/Polynomial.cpp": "OQS quartic solver (ACM TOMS 1010), no "
                                            "redistribution licence",
}


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

    def test_third_party_source_files_have_a_notice(self):
        # A source file carrying someone else's copyright or licence line
        # is ported or vendored code.  Each must map to a manifest package
        # that every distribution compiling it ships, or to an entry in
        # NOT_A_NOTICE / UNRESOLVED below.
        marker = re.compile(r"Copyright\s*(\(c\)|\(C\)|©|[0-9]{4})|Licensed under|"
                            r"SPDX-License-Identifier|copyright/permission notice")
        owner = re.compile(r"Copyright.*Aravind Krishnaswamy")
        roots = ["src/Library", "src/RISE", "src/Blender/native", "android/app/src/main/cpp",
                 "build/VS2022/RISE-GUI", "build/XCode/rise/RISE-GUI"]
        found = set()
        for root in roots:
            for directory, _, files in os.walk(os.path.join(REPO, root)):
                for name in files:
                    if not name.endswith((".c", ".cc", ".cpp", ".h", ".hpp", ".inc", ".mm",
                                          ".m", ".swift")):
                        continue
                    path = os.path.join(directory, name)
                    with open(path, "r", encoding="utf-8", errors="replace") as handle:
                        text = handle.read()
                    if any(marker.search(line) and not owner.search(line)
                           for line in text.splitlines()):
                        found.add(os.path.relpath(path, REPO).replace(os.sep, "/"))
        unknown = sorted(found - set(SOURCE_NOTICES) - set(UNRESOLVED))
        self.assertEqual(unknown, [], "third-party code with no notice mapping; add it to "
                         "licenses/third_party_notices.tsv and SOURCE_NOTICES")
        for rel, package in SOURCE_NOTICES.items():
            if rel not in found:
                continue
            for dist in tpn.DISTRIBUTIONS:
                packages = {pkg for pkg, _ in tpn.manifest_files(REPO, dist)}
                self.assertIn(package, packages, "%s (%s) not shipped for %s" % (rel, package, dist))

    def test_gui_licence_files_are_shipped(self):
        # Fonts, icons and other assets bundled into a GUI carry their
        # licence beside them; each such file must be in the manifest for
        # the distribution whose bundle contains it.
        roots = {"build/XCode/rise/RISE-GUI": "macos", "build/VS2022/RISE-GUI": "windows",
                 "android/app/src/main": "android"}
        for root, dist in roots.items():
            shipped = {os.path.normpath(os.path.join(REPO, rel))
                       for _, rel, dists in tpn.read_manifest(REPO) if dist in dists}
            for directory, _, files in os.walk(os.path.join(REPO, root)):
                for name in files:
                    if re.search(r"(^|[-_.])(licen[cs]e|copying|notice)([-_.]|$)", name, re.I):
                        path = os.path.normpath(os.path.join(directory, name))
                        self.assertIn(path, shipped, "%s is not shipped for %s" % (path, dist))

    def test_malformed_manifest_is_rejected(self):
        root = tempfile.mkdtemp()
        try:
            for bad in ("pkg\tLICENSE\n", "pkg\tLICENSE\tlinux\n", "pkg\t../x\tall\n"):
                write(os.path.join(root, tpn.MANIFEST_RELPATH), bad)
                with self.assertRaises(tpn.NoticeError, msg=bad):
                    tpn.read_manifest(root)
        finally:
            shutil.rmtree(root)

    def test_duplicate_line_is_rejected(self):
        root = tempfile.mkdtemp()
        try:
            write(os.path.join(root, tpn.MANIFEST_RELPATH),
                  "stb\tx/LICENSE\tall\nstb\tx/LICENSE\tandroid\n")
            with self.assertRaises(tpn.NoticeError):
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
        # The old folder was moved aside and deleted, not left behind.
        self.assertEqual(os.listdir(os.path.dirname(self.dest)), ["Third-Party Licenses"])

    def test_versioned_homebrew_name_is_a_valid_package(self):
        lic = os.path.join(self.tmp, "LICENSE")
        write(lic, "x")
        tpn.stage(REPO, "windows", self.dest, extra=[("openssl@3", lic)])
        self.assertTrue(os.path.isfile(os.path.join(self.dest, "openssl@3", "LICENSE")))

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

    def test_dll_audit(self):
        root = self.make_vcpkg([("qtbase", True)])
        write(os.path.join(root, "vcpkg", "info", "qtbase_1.0_x64-windows.list"),
              "x64-windows/bin/Qt6Core.dll\nx64-windows/share/qtbase/copyright\n")
        known = tpn.vcpkg_dll_names(root, "x64-windows")
        self.assertEqual(known, {"qt6core.dll"})
        bindir = os.path.join(self.tmp, "bin")
        write(os.path.join(bindir, "Qt6Core.dll"), "")
        write(os.path.join(bindir, "RISE-GUI.exe"), "")
        self.assertEqual(tpn.audit_dlls(bindir, known), [])
        # oneTBB copied in from TBB_ROOT by extlib/oidn/build.ps1.
        tbb = os.path.join(self.tmp, "tbb")
        write(os.path.join(tbb, "redist", "intel64", "vc14", "tbb12.dll"), "")
        write(os.path.join(tbb, "LICENSE.txt"), "Apache")
        write(os.path.join(bindir, "tbb12.dll"), "")
        extra = tpn.audit_dlls(bindir, known, tbb)
        self.assertEqual([(e[0], e[2]) for e in extra], [("oneTBB", "LICENSE.txt")])
        with self.assertRaises(tpn.NoticeError):
            tpn.audit_dlls(bindir, known)  # TBB_ROOT unset: unknown
        # OIDN's install ships oneTBB's licence list: no TBB_ROOT needed.
        self.assertEqual(tpn.audit_dlls(bindir, known, None, tbb_covered=True), [])
        # The MSVC runtime needs no notice.
        write(os.path.join(bindir, "vcruntime140.dll"), "")
        self.assertEqual(tpn.audit_dlls(bindir, known, None, tbb_covered=True), [])
        # A version-renamed DLL left over from an older vcpkg fails, naming
        # the library it resembles.
        write(os.path.join(bindir, "Qt6Core_5_d.dll"), "")
        with self.assertRaises(tpn.NoticeError) as ctx:
            tpn.audit_dlls(bindir, known, None, tbb_covered=True)
        self.assertIn("another version of qt6core.dll", str(ctx.exception))
        os.remove(os.path.join(bindir, "Qt6Core_5_d.dll"))
        # Dot-folders are searched too.
        write(os.path.join(bindir, ".x", "hidden.dll"), "")
        with self.assertRaises(tpn.NoticeError):
            tpn.audit_dlls(bindir, known, None, tbb_covered=True)
        shutil.rmtree(os.path.join(bindir, ".x"))
        # Subfolders are searched (Qt plugins), except the skipped one.
        write(os.path.join(bindir, "Third-Party Licenses", "x", "odd.dll"), "")
        self.assertEqual(tpn.audit_dlls(bindir, known, None, tbb_covered=True,
                                        skip=[os.path.join(bindir, "Third-Party Licenses")]), [])
        write(os.path.join(bindir, "platforms", "qmystery.dll"), "")
        with self.assertRaises(tpn.NoticeError):
            tpn.audit_dlls(bindir, known, None, tbb_covered=True,
                           skip=[os.path.join(bindir, "Third-Party Licenses")])
        os.remove(os.path.join(bindir, "platforms", "qmystery.dll"))
        shutil.rmtree(os.path.join(bindir, "Third-Party Licenses"))
        # A DLL from nowhere fails.
        write(os.path.join(bindir, "mystery.dll"), "")
        with self.assertRaises(tpn.NoticeError) as ctx:
            tpn.audit_dlls(bindir, known, tbb)
        self.assertIn("mystery.dll", str(ctx.exception))

    def test_command_line_dll_audit_uses_license_roots(self):
        script = os.path.join(TOOLS, "third_party_notices.py")
        pgl = os.path.join(self.tmp, "openpgl-install")
        write(os.path.join(pgl, "bin", "openpgl.dll"), "")
        write(os.path.join(pgl, "share", "doc", "openpgl", "LICENSE.txt"), "Apache")
        bindir = os.path.join(self.tmp, "bin")
        write(os.path.join(bindir, "openpgl.dll"), "")
        args = [sys.executable, script, "--distribution", "windows",
                "--dest", os.path.join(bindir, "Third-Party Licenses"),
                "--license-root", "openpgl=" + pgl, "--check-dlls", bindir + os.sep + "."]
        env = dict(os.environ)
        env.pop("TBB_ROOT", None)
        ok = subprocess.run(args, capture_output=True, text=True, env=env)
        self.assertEqual(ok.returncode, 0, ok.stderr)
        write(os.path.join(bindir, "stray.dll"), "")
        bad = subprocess.run(args, capture_output=True, text=True, env=env)
        self.assertNotEqual(bad.returncode, 0)
        self.assertIn("stray.dll", bad.stderr)

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
        result = subprocess.run(["/bin/bash", "-c", script],
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

    def test_loop_matches_python_on_whitespace_and_crlf(self):
        root = tempfile.mkdtemp()
        try:
            for rel in ("a/LICENSE", "b/LICENSE", "c/LICENSE"):
                write(os.path.join(root, rel), rel)
            write(os.path.join(root, tpn.MANIFEST_RELPATH),
                  "# comment\r\n"
                  "A\ta/LICENSE\tandroid, macos\r\n"
                  "B\tb/LICENSE\tall \r\n"
                  "C\tc/LICENSE\twindows\r\n")
            expected = {(p, os.path.basename(f)) for p, f in tpn.manifest_files(root, "macos")}
            self.assertEqual(expected, {("A", "LICENSE"), ("B", "LICENSE")})
            result, app = self.run_loop(root)
            try:
                self.assertEqual(result.returncode, 0, result.stderr)
                copied = {(package, name) for package in os.listdir(app)
                          for name in os.listdir(os.path.join(app, package))}
                self.assertEqual(copied, expected)
            finally:
                shutil.rmtree(app)
        finally:
            shutil.rmtree(root)

    def test_loop_accepts_exactly_what_python_accepts(self):
        lines = [
            "A\tx/LICENSE\tall", "A\tx/LICENSE\tall\r", "A\tx/LICENSE\tandroid, macos",
            " A \t x/LICENSE \t macos ", "A\tx/LICENSE\twindows",
            "A\t\tx/LICENSE\tall", "A\tx/LICENSE", "A\tx/LICENSE\tall\textra",
            "pkg name\tx/LICENSE\tall", "A\tx/LICENSE\tmacos,", "A\tx/LICENSE\tmacOS",
            "A\t../x/LICENSE\tall", "A\t/x/LICENSE\tall", "A\tx/LICENSE\t",
            "openssl@3\tx/LICENSE\tmacos", "   # indented comment", "\t# tab comment",
            "A\tx/LICENSE\tmac os", "A\tx/LICENSE\tall,macos", "Z\tx/LICENSE\tandroid",
            "A\tx/LICENSE\t all ",
        ]
        root = tempfile.mkdtemp()
        try:
            write(os.path.join(root, "x", "LICENSE"), "x")
            for line in lines:
                # A second, valid macos line keeps the "lists no notices"
                # guard out of the comparison.
                write(os.path.join(root, tpn.MANIFEST_RELPATH),
                      "Z\tx/LICENSE\tmacos\n" + line + "\n")
                try:
                    tpn.read_manifest(root)
                    python_ok = True
                except tpn.NoticeError:
                    python_ok = False
                result, app = self.run_loop(root)
                shutil.rmtree(app)
                self.assertEqual(result.returncode == 0, python_ok,
                                 "%r: python %s, bash %s" % (line, python_ok, result.stderr))
        finally:
            shutil.rmtree(root)

    def test_loop_rejects_unknown_distribution(self):
        root = tempfile.mkdtemp()
        try:
            write(os.path.join(root, tpn.MANIFEST_RELPATH),
                  "stb\textlib/stb/LICENSE.txt\tmacOS\n")
            result, app = self.run_loop(root)
            shutil.rmtree(app)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("unknown distribution", result.stderr)
        finally:
            shutil.rmtree(root)

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
            self.assertEqual(text.count('--check-dlls "$(OutDir)." $(RiseExtraNoticeArgs)'), 2,
                             project)
            import xml.dom.minidom
            xml.dom.minidom.parse(os.path.join(REPO, "build", "VS2022", project))
            self.assertEqual(text.count("openpgl=$(RiseOpenPglRoot.TrimEnd('\\'))"), 2, project)
            # The stager must be the LAST command of each post-build event:
            # a batch's exit code is its last command's, so a command after
            # it could hide a failure.
            for match in re.finditer(r"<Command>(.*?)</Command>", text, re.S):
                if "third_party_notices.py" in match.group(1):
                    last = match.group(1).strip().splitlines()[-1]
                    self.assertIn("third_party_notices.py", last, project)


class BlenderPackageTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.bridge = os.path.join(self.tmp, package_blender_addon.bridge_filename())
        write(self.bridge, "not really a library")
        self.output = os.path.join(self.tmp, "dist", "rise_renderer.zip")

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def test_zip_carries_addon_bridge_and_notices(self):
        staged = package_blender_addon.package(REPO, self.bridge, self.output, [])
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
            package_blender_addon.package(REPO, self.bridge, self.output, [])
        self.assertFalse(os.path.exists(self.output))

    def test_missing_notice_writes_no_zip(self):
        root = os.path.join(self.tmp, "repo")
        shutil.copytree(os.path.join(REPO, "src", "Blender", "addons", "rise_renderer"),
                        os.path.join(root, "src", "Blender", "addons", "rise_renderer"))
        write(os.path.join(root, tpn.MANIFEST_RELPATH), "stb\textlib/stb/LICENSE.txt\tall\n")
        with self.assertRaises(tpn.NoticeError):
            package_blender_addon.package(root, self.bridge, self.output, [])
        self.assertFalse(os.path.exists(self.output))
        self.assertFalse(os.path.exists(self.output + ".partial"))


class BlenderDependencyNoticeTest(unittest.TestCase):
    """dependency_notices on a fake library layout."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.saved = package_blender_addon.external_dependencies
        brew = os.path.join(self.tmp, "brew")
        write(os.path.join(brew, "Cellar", "openexr", "3.4", "lib", "libOpenEXR-3_4.33.dylib"), "")
        write(os.path.join(brew, "Cellar", "openexr", "3.4", "lib", "libIex-3_4.33.dylib"), "")
        write(os.path.join(brew, "opt", "openexr", "LICENSE.md"), "BSD")
        write(os.path.join(brew, "lib", "libopenpgl.0.7.1.dylib"), "")
        write(os.path.join(brew, "share", "doc", "openpgl", "LICENSE.txt"), "Apache")
        self.repo = os.path.join(self.tmp, "repo")
        oidn = os.path.join(self.repo, "extlib", "oidn", "install")
        write(os.path.join(oidn, "lib", "libOpenImageDenoise.2.dylib"), "")
        write(os.path.join(oidn, "doc", "LICENSE.txt"), "Apache")
        vdb = os.path.join(self.tmp, "blender", "openvdb")
        write(os.path.join(vdb, "lib", "libopenvdb.dylib"), "")
        self.deps = [
            os.path.join(brew, "Cellar", "openexr", "3.4", "lib", "libOpenEXR-3_4.33.dylib"),
            os.path.join(brew, "Cellar", "openexr", "3.4", "lib", "libIex-3_4.33.dylib"),
            os.path.join(brew, "lib", "libopenpgl.0.7.1.dylib"),
            os.path.join(oidn, "lib", "libOpenImageDenoise.2.dylib"),
            os.path.join(vdb, "lib", "libopenvdb.dylib"),
        ]
        package_blender_addon.external_dependencies = lambda bridge: list(self.deps)

    def tearDown(self):
        package_blender_addon.external_dependencies = self.saved
        shutil.rmtree(self.tmp)

    def test_library_without_licence_fails_until_overridden(self):
        with self.assertRaises(tpn.NoticeError) as ctx:
            package_blender_addon.dependency_notices("bridge", {}, self.repo)
        self.assertIn("openvdb", str(ctx.exception))
        override = os.path.join(self.tmp, "vdb-licence")
        write(os.path.join(override, "LICENSE"), "MPL-2.0")
        notices = package_blender_addon.dependency_notices("bridge", {"openvdb": override},
                                                          self.repo)
        self.assertEqual(sorted({n[0] for n in notices}),
                         ["OpenImageDenoise", "openexr", "openpgl", "openvdb"])

    def test_unused_override_fails(self):
        self.deps = self.deps[:4]
        with self.assertRaises(tpn.NoticeError):
            package_blender_addon.dependency_notices("bridge", {"zlib": self.tmp}, self.repo)

    def test_broad_prefix_fails(self):
        # A prefix holding many packages' licences is not one library's.
        wide = os.path.join(self.tmp, "usr-local")
        for index in range(20):
            write(os.path.join(wide, "share", "doc", "pkg%d" % index, "LICENSE"), "x")
        write(os.path.join(wide, "lib", "libthing.so.1"), "")
        self.deps = [os.path.join(wide, "lib", "libthing.so.1")]
        # A library outside Homebrew and the known installs needs an
        # explicit licence location ...
        with self.assertRaises(tpn.NoticeError) as ctx:
            package_blender_addon.dependency_notices("bridge", {}, self.repo)
        self.assertIn("--license-root thing=DIR", str(ctx.exception))
        # ... and pointing it at a whole shared prefix is refused.
        with self.assertRaises(tpn.NoticeError) as ctx:
            package_blender_addon.dependency_notices("bridge", {"thing": wide}, self.repo)
        self.assertIn("too many", str(ctx.exception))

    def test_linux_runtime_filter(self):
        runtime = package_blender_addon._LINUX_RUNTIME
        for name in ("libc.so.6", "libm.so.6", "libstdc++.so.6", "libgcc_s.so.1",
                     "ld-linux-x86-64.so.2", "libpthread.so.0"):
            self.assertTrue(runtime.match(name), name)
        # A distribution's third-party libraries in /usr/lib are not runtime.
        for name in ("libpng16.so.16", "libOpenEXR-3_1.so.30", "libtbb.so.12",
                     "libcrypto.so.3", "libcurl.so.4"):
            self.assertFalse(runtime.match(name), name)

    def test_library_name(self):
        self.assertEqual(package_blender_addon.library_name("libOpenImageDenoise.2.4.1.dylib"),
                         "OpenImageDenoise")
        self.assertEqual(package_blender_addon.library_name("/x/libtbb.so.12"), "tbb")
        self.assertEqual(package_blender_addon.library_name("libopenvdb.dylib"), "openvdb")


if __name__ == "__main__":
    unittest.main()
