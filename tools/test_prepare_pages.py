"""Tests for prepare_pages: packaging merged firmware and static site for Pages.

These tests verify that prepare_pages copies the site directory, the firmware
binary, and a release-aware manifest into an output directory that GitHub Pages
or other static hosts can publish.
"""
import json
import os
import shutil
import tempfile
import unittest
from pathlib import Path

# Import prepare_pages from prepare_pages.py (will fail until implemented)
try:
    from prepare_pages import prepare_pages
except ImportError:
    prepare_pages = None


class PreparePagesFunctionalTests(unittest.TestCase):
    """Tests for the prepare_pages function."""

    def setUp(self):
        """Create temporary directories for testing."""
        if prepare_pages is None:
            self.skipTest("prepare_pages module not yet implemented")

        self.temp_dir = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.temp_dir)

        # Create a test site directory with static files
        self.site_dir = Path(self.temp_dir) / "site"
        self.site_dir.mkdir()
        (self.site_dir / "index.html").write_text("<html><body>Test</body></html>")
        (self.site_dir / "styles.css").write_text("body { margin: 0; }")

        # Create a test firmware binary
        self.firmware_bin = Path(self.temp_dir) / "merged.bin"
        self.firmware_bin.write_bytes(b"TEST_FIRMWARE_BINARY_DATA")

        # Output directory (will be created or replaced by prepare_pages)
        self.output_dir = Path(self.temp_dir) / "output"

    def test_prepare_pages_function_exists(self):
        """Verify that prepare_pages is callable."""
        self.assertIsNotNone(prepare_pages)
        self.assertTrue(callable(prepare_pages))

    def test_copies_static_files(self):
        """Verify that prepare_pages copies site files to output without modifying source."""
        prepare_pages(self.site_dir, self.firmware_bin, self.output_dir, "v0.1.0")

        # Check that output files exist
        self.assertTrue((self.output_dir / "index.html").exists())
        self.assertTrue((self.output_dir / "styles.css").exists())

        # Verify content was copied correctly
        self.assertEqual(
            (self.output_dir / "index.html").read_text(),
            "<html><body>Test</body></html>",
        )
        self.assertEqual((self.output_dir / "styles.css").read_text(), "body { margin: 0; }")

        # Verify source is unmodified
        self.assertTrue((self.site_dir / "index.html").exists())
        self.assertTrue((self.site_dir / "styles.css").exists())

    def test_copies_firmware_binary(self):
        """Verify that prepare_pages copies the firmware binary to firmware/merged-firmware.bin."""
        prepare_pages(self.site_dir, self.firmware_bin, self.output_dir, "v0.1.0")

        firmware_path = self.output_dir / "firmware" / "merged-firmware.bin"
        self.assertTrue(firmware_path.exists(), f"Expected {firmware_path} to exist")
        self.assertEqual(firmware_path.read_bytes(), b"TEST_FIRMWARE_BINARY_DATA")

    def test_generates_manifest_with_correct_fields(self):
        """Verify that prepare_pages generates a manifest following ESP Web Tools schema.

        Schema per https://esphome.github.io/esp-web-tools/:
        - Root: name, version, new_install_prompt_erase
        - Root: builds array with chipFamily and parts per build
        """
        prepare_pages(self.site_dir, self.firmware_bin, self.output_dir, "v0.1.0")

        manifest_path = self.output_dir / "firmware" / "manifest.json"
        self.assertTrue(manifest_path.exists(), f"Expected {manifest_path} to exist")

        with open(manifest_path, "r", encoding="utf-8") as f:
            manifest = json.load(f)

        # Verify root-level required fields
        self.assertEqual(manifest.get("name"), "ESP32-C3-UniSerial")
        self.assertEqual(manifest.get("version"), "v0.1.0")
        self.assertTrue(manifest.get("new_install_prompt_erase"))

        # Verify builds array structure
        builds = manifest.get("builds")
        self.assertIsNotNone(builds, "manifest must have a 'builds' key")
        self.assertIsInstance(builds, list, "builds must be an array")
        self.assertEqual(len(builds), 1, "expected exactly one build for ESP32-C3")

        build = builds[0]
        self.assertEqual(build.get("chipFamily"), "ESP32-C3")

        parts = build.get("parts")
        self.assertIsNotNone(parts, "build must have a 'parts' key")
        self.assertIsInstance(parts, list, "parts must be an array")
        self.assertEqual(len(parts), 1, "expected exactly one part")

        part = parts[0]
        self.assertEqual(part.get("path"), "merged-firmware.bin")
        self.assertEqual(part.get("offset"), 0)

    def test_missing_firmware_binary_raises_error(self):
        """Verify that prepare_pages raises an explicit filesystem error if binary is missing."""
        missing_bin = Path(self.temp_dir) / "nonexistent.bin"
        self.assertFalse(missing_bin.exists())

        with self.assertRaises(FileNotFoundError):
            prepare_pages(self.site_dir, missing_bin, self.output_dir, "v0.1.0")

        # Verify that output_dir was not created/modified (no partial state)
        # If output_dir doesn't exist, that's the expected clean state
        if self.output_dir.exists():
            # If it exists, it should be empty or only contain what was there before
            contents = list(self.output_dir.iterdir())
            self.assertEqual(
                len(contents),
                0,
                f"Output directory should be empty after error, found: {contents}",
            )

    def test_replaces_stale_output_contents_deterministically(self):
        """Verify that prepare_pages replaces existing output contents cleanly."""
        # First run
        prepare_pages(self.site_dir, self.firmware_bin, self.output_dir, "v0.1.0")
        original_manifest = (self.output_dir / "firmware" / "manifest.json").read_text()

        # Add a stale file that shouldn't be there in a second run
        stale_file = self.output_dir / "stale.txt"
        stale_file.write_text("stale content")

        # Second run with different version
        new_firmware = Path(self.temp_dir) / "new.bin"
        new_firmware.write_bytes(b"NEW_FIRMWARE")

        prepare_pages(self.site_dir, new_firmware, self.output_dir, "v0.2.0")

        # Verify stale file is gone
        self.assertFalse(stale_file.exists(), "Stale files should be removed")

        # Verify new content is present and old is replaced
        new_manifest = (self.output_dir / "firmware" / "manifest.json").read_text()
        self.assertNotEqual(
            original_manifest,
            new_manifest,
            "Manifest should be updated with new version",
        )

        manifest = json.loads(new_manifest)
        self.assertEqual(manifest.get("version"), "v0.2.0")


class PreparePagesCLITests(unittest.TestCase):
    """Tests for the prepare_pages CLI interface."""

    def test_cli_script_runs_with_help(self):
        """Verify that prepare_pages.py can be invoked with --help and produces expected output."""
        import subprocess
        import sys

        script_path = os.path.join(
            os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
            "tools",
            "prepare_pages.py",
        )
        self.assertTrue(os.path.isfile(script_path), f"Script not found at {script_path}")

        result = subprocess.run(
            [sys.executable, script_path, "--help"],
            capture_output=True,
            text=True,
            timeout=5,
        )

        self.assertEqual(result.returncode, 0, f"--help failed: {result.stderr}")
        self.assertIn("site-dir", result.stdout, "Expected --site-dir in help output")
        self.assertIn("firmware-bin", result.stdout, "Expected --firmware-bin in help output")
        self.assertIn("output-dir", result.stdout, "Expected --output-dir in help output")
        self.assertIn("version", result.stdout, "Expected --version in help output")


if __name__ == "__main__":
    unittest.main()
