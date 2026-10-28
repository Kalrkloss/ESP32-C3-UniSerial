#!/usr/bin/env python3
"""Prepare a GitHub Pages release site with merged firmware and manifest.

This module packages a static site (HTML, CSS, etc.) together with a firmware
binary into a single output directory suitable for publishing via GitHub Pages.
The output includes a manifest.json that describes the firmware image for the
esp-web-tools installer.
"""
import argparse
import json
import shutil
from pathlib import Path


def prepare_pages(site_dir: Path, firmware_bin: Path, output_dir: Path, version: str) -> None:
    """Package a static site and firmware binary for GitHub Pages release.

    Copies the contents of site_dir to output_dir, places the firmware binary
    at output_dir/firmware/merged-firmware.bin, and generates a release-aware
    manifest at output_dir/firmware/manifest.json.

    Args:
        site_dir: Path to the source site directory (HTML, CSS, etc.)
        firmware_bin: Path to the merged firmware binary file
        output_dir: Path to the output directory (will be created or replaced)
        version: Release version string (e.g., "v0.1.0")

    Raises:
        FileNotFoundError: If firmware_bin does not exist or site_dir is invalid.
        Propagates shutil errors if directory operations fail.
    """
    # Convert to Path objects for consistent handling
    site_dir = Path(site_dir)
    firmware_bin = Path(firmware_bin)
    output_dir = Path(output_dir)

    # Validate inputs before making any changes
    if not firmware_bin.exists():
        raise FileNotFoundError(f"Firmware binary not found: {firmware_bin}")
    if not site_dir.exists():
        raise FileNotFoundError(f"Site directory not found: {site_dir}")
    if not site_dir.is_dir():
        raise NotADirectoryError(f"Site path is not a directory: {site_dir}")

    # Replace output directory contents deterministically
    if output_dir.exists():
        shutil.rmtree(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    # Copy site files to output root
    for item in site_dir.iterdir():
        dest = output_dir / item.name
        if item.is_dir():
            shutil.copytree(item, dest)
        else:
            shutil.copy2(item, dest)

    # Create firmware directory
    firmware_dir = output_dir / "firmware"
    firmware_dir.mkdir(parents=True, exist_ok=True)

    # Copy firmware binary
    merged_firmware_path = firmware_dir / "merged-firmware.bin"
    shutil.copy2(firmware_bin, merged_firmware_path)

    # Generate manifest following ESP Web Tools schema
    # https://esphome.github.io/esp-web-tools/ - "Creating your manifest"
    manifest = {
        "name": "ESP32-C3-UniSerial",
        "version": version,
        "new_install_prompt_erase": True,
        "builds": [
            {
                "chipFamily": "ESP32-C3",
                "parts": [{"path": "merged-firmware.bin", "offset": 0}],
            }
        ],
    }

    manifest_path = firmware_dir / "manifest.json"
    with open(manifest_path, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)


def main():
    """CLI entry point for prepare_pages."""
    parser = argparse.ArgumentParser(
        description="Package a static site and firmware binary for GitHub Pages release."
    )
    parser.add_argument(
        "--site-dir",
        type=Path,
        required=True,
        help="Path to the source site directory (HTML, CSS, etc.)",
    )
    parser.add_argument(
        "--firmware-bin",
        type=Path,
        required=True,
        help="Path to the merged firmware binary file",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        required=True,
        help="Path to the output directory (will be created or replaced)",
    )
    parser.add_argument(
        "--version",
        required=True,
        help="Release version string (e.g., v0.1.0)",
    )

    args = parser.parse_args()

    try:
        prepare_pages(args.site_dir, args.firmware_bin, args.output_dir, args.version)
        print(f"✓ Packaged site and firmware to {args.output_dir}")
    except (FileNotFoundError, NotADirectoryError) as e:
        print(f"✗ Error: {e}", file=__import__("sys").stderr)
        raise


if __name__ == "__main__":
    main()
