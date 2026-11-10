import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


class CiNativeRunnerTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="glamdb ci ")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        (self.root / "scripts").mkdir()
        (self.root / "bin").mkdir()
        self.runner = self.root / "scripts/ci-native.sh"
        if (ROOT / "scripts/ci-native.sh").exists():
            shutil.copyfile(ROOT / "scripts/ci-native.sh", self.runner)

    def run_script(self, *arguments):
        return subprocess.run(
            ["bash", str(self.runner), *arguments], cwd=self.root,
            env={**os.environ, "PATH": str(self.root / "bin") + os.pathsep + os.environ["PATH"]},
            text=True, capture_output=True, timeout=15,
        )

    def test_invalid_profile_or_stage_has_no_build_side_effects(self):
        for arguments in (("bogus", "test"), ("normal", "bogus"), ("asan",), ("normal", "test", "extra")):
            with self.subTest(arguments=arguments):
                result = self.run_script(*arguments)
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertFalse((self.root / "build").exists())

    def test_build_failure_keeps_exit_code_and_diagnostic_log(self):
        cmake = self.root / "bin/cmake"
        cmake.write_text('#!/bin/sh\nprintf "%s\\n" "compiler failure details" >&2\nexit 17\n')
        cmake.chmod(0o755)
        result = self.run_script("normal", "build")
        self.assertEqual(result.returncode, 17, result.stderr)
        self.assertIn("compiler failure details", result.stdout + result.stderr)
        self.assertIn("compiler failure details", (self.root / "artifacts/native-normal/build.log").read_text())


if __name__ == "__main__":
    unittest.main()
