#!/usr/bin/env python3
"""Build-contract REDs for the ARC-owned fire Metal buffers (macOS only)."""
import copy
import json
from pathlib import Path
import shlex
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]
NAMES = {"FireProduction" + family + "Mac.mm"
         for family in ("Advection", "Projection", "Force", "Compute")}


def entries(project):
    objects = project["objects"]
    return {key: value for key, value in objects.items()
            if value.get("isa") == "PBXBuildFile"
            and Path(objects[value["fileRef"]].get("path", "")).name in NAMES}


def validate(project):
    found = entries(project)
    counts = dict.fromkeys(NAMES, 0)
    for value in found.values():
        name = Path(project["objects"][value["fileRef"]]["path"]).name
        counts[name] += 1
        flags = shlex.split(value.get("settings", {}).get("COMPILER_FLAGS", ""))
        ownership = [flag for flag in flags if flag in ("-fobjc-arc", "-fno-objc-arc")]
        if not ownership or ownership[-1] != "-fobjc-arc":
            raise ValueError("ARC missing or overridden: " + name)
    if any(count != 2 for count in counts.values()):
        raise ValueError("both Xcode target entries required for every Metal family")


class ARCContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        project = ROOT / "build/XCode/rise/rise.xcodeproj/project.pbxproj"
        cls.project = json.loads(subprocess.check_output(
            ["plutil", "-convert", "json", "-o", "-", str(project)]))

    def test_current_targets(self):
        validate(self.project)

    def test_each_target_missing_arc_and_override_are_red(self):
        for key in entries(self.project):
            for override in (False, True):
                with self.subTest(entry=key, override=override):
                    mutant = copy.deepcopy(self.project)
                    settings = mutant["objects"][key]["settings"]
                    flags = shlex.split(settings["COMPILER_FLAGS"])
                    if override:
                        flags.append("-fno-objc-arc")
                    else:
                        flags.remove("-fobjc-arc")
                    settings["COMPILER_FLAGS"] = shlex.join(flags)
                    with self.assertRaises(ValueError):
                        validate(mutant)

    def test_actual_translation_unit_guards_refuse_non_arc(self):
        for name in sorted(NAMES):
            source = (ROOT / "src/Library/Utilities" / name).read_text()
            prefix = source.split("#import <Foundation/Foundation.h>", 1)[0]
            self.assertIn("#if !__has_feature(objc_arc)", prefix)
            for enabled in (False, True):
                with self.subTest(source=name, arc=enabled):
                    result = subprocess.run(
                        ["xcrun", "clang++", "-x", "objective-c++", "-E",
                         "-fobjc-arc" if enabled else "-fno-objc-arc", "-"],
                        input=prefix, text=True, capture_output=True)
                    if enabled:
                        self.assertEqual(result.returncode, 0, result.stderr)
                    else:
                        self.assertNotEqual(result.returncode, 0)
                        self.assertIn("Fire production Metal resource lifetime requires ARC",
                                      result.stderr)


if __name__ == "__main__":
    unittest.main()
