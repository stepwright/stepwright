"""Exercise the tracked-file reference guard, including intent-to-add files."""

import os
import pathlib
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
SCRATCH = pathlib.Path(sys.argv.pop()).resolve()
SCRATCH.mkdir(parents=True, exist_ok=True)


class ForbiddenReferences(unittest.TestCase):
    def test_tracked_new_and_untracked_files(self):
        with tempfile.TemporaryDirectory(dir=SCRATCH) as temp:
            root = pathlib.Path(temp)
            (root / "scripts").mkdir()
            script = root / "scripts/check-forbidden-usage.sh"
            script.write_bytes((ROOT / "scripts/check-forbidden-usage.sh").read_bytes())
            (root / "include").mkdir()
            (root / "include/ok.hpp").write_text("#pragma once\n")
            (root / "new.md").write_text("An internal link: " + "." + "claude/plans/example.md\n")
            subprocess.run(["git", "init", "--quiet", str(root)], check=True)
            subprocess.run(["git", "add", "include/ok.hpp"], cwd=root, check=True)
            def check(manifest=None):
                env = dict(os.environ)
                env.pop("STEPWRIGHT_TRACKED_FILES", None)
                if manifest is not None:
                    env["STEPWRIGHT_TRACKED_FILES"] = str(manifest)
                return subprocess.run(["bash", str(script)], cwd=root,
                                      env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            self.assertEqual(check().returncode, 0)  # ignored by the tracked-only guard
            subprocess.run(["git", "add", "-N", "new.md"], cwd=root, check=True)
            result = check()
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("internal planning reference", result.stdout)
            (root / "new.md").write_text("A public link: docs/design/engine.md\n")
            self.assertEqual(check().returncode, 0)
            # Scan a tracked symlink's text, never an untracked target's contents.
            (root / "untracked-target.md").write_text("." + "claude/plans/example.md\n")
            (root / "link.md").symlink_to("untracked-target.md")
            subprocess.run(["git", "add", "-N", "link.md"], cwd=root, check=True)
            self.assertEqual(check().returncode, 0)
            # The container uses a host-produced manifest, not an inaccessible
            # worktree pointer. Both branches must scan the actual file contents.
            manifest = root / "tracked.files"
            manifest.write_bytes(subprocess.check_output(["git", "ls-files", "-z"], cwd=root))
            self.assertEqual(check(manifest).returncode, 0)
            (root / "new.md").write_text("." + "claude/plans/example.md\n")
            self.assertNotEqual(check(manifest).returncode, 0)
            (root / "new.md").write_text("Public again\n")
            (root / ".git").rename(root / ".git.saved")
            (root / ".git").write_text("gitdir: missing-metadata\n")
            self.assertNotEqual(check().returncode, 0)  # Git errors must fail closed
            self.assertEqual(check(manifest).returncode, 0)
            self.assertNotEqual(check(root / "missing.files").returncode, 0)
            manifest.write_bytes(b"")
            self.assertNotEqual(check(manifest).returncode, 0)
            manifest.write_bytes(b"../outside\0")
            self.assertNotEqual(check(manifest).returncode, 0)


if __name__ == "__main__":
    unittest.main()
