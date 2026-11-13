"""Release version parsing and user-facing notes selection."""

import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "release_metadata", ROOT / "tools/release_metadata.py"
)
release = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(release)


class ReleaseTests(unittest.TestCase):
    def test_versions(self):
        self.assertEqual(release.metadata("v0.1.0"), ("0.1.0", False))
        self.assertEqual(release.metadata("v1.2.3-beta.1"), ("1.2.3-beta.1", True))
        self.assertEqual(
            release.metadata("v1.2.3+build-linux"), ("1.2.3+build-linux", False)
        )
        self.assertEqual(
            release.metadata("v1.2.3-rc.1+build.42"), ("1.2.3-rc.1+build.42", True)
        )

    def test_invalid_versions(self):
        for tag in [
            "1.2.3",
            "v1.2",
            "v01.2.3",
            "v1.2.3-01",
            "v1.2.3-",
            "v1.2.3+",
            "v1.2.3\n",
            'v1.2.3";pwd',
            "v$(pwd).2.3",
        ]:
            with self.subTest(tag=tag), self.assertRaises(ValueError):
                release.metadata(tag)

    def test_release_notes(self):
        changelog = "# Log\n\n## [Unreleased]\nfuture\n\n## [1.2.3] - 2026-11-14\n\n### Added\n- A\n\n## [1.2.2]\nold\n"
        self.assertEqual(release.release_notes(changelog, "1.2.3"), "### Added\n- A")
        self.assertIsNone(release.release_notes(changelog, "1.3.0"))
        self.assertEqual(
            release.release_notes("## [v1.2.3-rc.1]\nPreview", "1.2.3-rc.1"), "Preview"
        )

    def test_bad_notes(self):
        for notes in ["## [1.2.3]\n", "## [1.2.3]\nA\n## [1.2.3]\nB"]:
            with self.subTest(notes=notes), self.assertRaises(ValueError):
                release.release_notes(notes, "1.2.3")


if __name__ == "__main__":
    unittest.main()
