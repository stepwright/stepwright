"""Release-note extraction must fail closed before GitHub publication."""

import importlib.util
import pathlib
import unittest

SPEC = importlib.util.spec_from_file_location(
    "release_notes", pathlib.Path(__file__).resolve().parents[1] / "scripts/release_notes.py")
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class ReleaseNotes(unittest.TestCase):
    def test_exact_section(self):
        text = "## [Unreleased]\nnew\n## [0.1.0] - 2026-10-25\n### Added\n- first\n## [0.0.1] - 2026-01-01\nold\n"
        self.assertEqual(MODULE.extract(text, "0.1.0"), "### Added\n- first\n")

    def test_refuse_unreleased_missing_empty_duplicate_and_malformed(self):
        for text, version in [("## [Unreleased]\nnew", "0.1.0"),
                              ("## [0.1.0] - 2026-10-25\n", "0.1.0"),
                              ("## [0.1.0] - 2026-10-25\na\n## [0.1.0] - 2026-10-25\nb", "0.1.0"),
                              ("## [0.1.0]\nno date", "0.1.0"), ("", "Unreleased")]:
            with self.subTest(text=text, version=version), self.assertRaises(ValueError):
                MODULE.extract(text, version)


if __name__ == "__main__":
    unittest.main()
