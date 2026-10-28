"""Tests for the project documentation/installer site.

These tests parse ``site/index.html`` with the standard-library HTML parser
and verify the content required by the firmware release plan: English
content, the esp-web-tools installer button, the required documentation
sections, relative-only local links, and the browser/erase warnings users
need before flashing.
"""
import os
import posixpath
import re
import unittest
from html.parser import HTMLParser

SITE_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "site")
INDEX_PATH = os.path.join(SITE_DIR, "index.html")


class _LinkCollector(HTMLParser):
    """Collects href/src attribute values and ids from the parsed document."""

    def __init__(self):
        super().__init__()
        self.local_refs = []
        self.ids = set()
        self.has_install_button = False
        self.html_lang = None

    def handle_starttag(self, tag, attrs):
        attr_dict = dict(attrs)
        if tag == "html" and "lang" in attr_dict:
            self.html_lang = attr_dict["lang"]
        if tag == "esp-web-install-button":
            self.has_install_button = True
            if attr_dict.get("manifest"):
                self.local_refs.append(("manifest", attr_dict["manifest"]))
        if "id" in attr_dict:
            self.ids.add(attr_dict["id"])
        for key in ("href", "src"):
            if key in attr_dict and attr_dict[key]:
                self.local_refs.append((key, attr_dict[key]))


def _read_site_html():
    with open(INDEX_PATH, "r", encoding="utf-8") as handle:
        return handle.read()


def _is_external_or_fragment(value):
    """True for values that are not local site-relative files."""
    if value.startswith("#"):
        return True
    if "://" in value:
        return True
    if value.startswith("//"):
        return True
    if value.startswith("mailto:"):
        return True
    return False


def _escapes_site_root(value):
    """True if a local-relative reference normalizes to above the site root.

    Pages publishing copies only the ``site/`` directory contents to the
    published root, so a reference such as ``../README.md`` or
    ``sub/../../x`` must be treated as broken even though it does not start
    with a leading ``/``.
    """
    path_part = value.split("#", 1)[0].split("?", 1)[0]
    if not path_part:
        return False
    normalized = posixpath.normpath(path_part)
    return normalized == ".." or normalized.startswith("../")


class SiteContentTests(unittest.TestCase):
    def setUp(self):
        if not os.path.isfile(INDEX_PATH):
            self.fail(f"{INDEX_PATH} does not exist yet")
        self.html = _read_site_html()
        parser = _LinkCollector()
        parser.feed(self.html)
        self.parser = parser

    def test_declares_english_content(self):
        self.assertEqual(self.parser.html_lang, "en")

    def test_has_install_button_with_manifest(self):
        self.assertTrue(
            self.parser.has_install_button,
            "expected an <esp-web-install-button> element",
        )
        manifest_refs = [v for k, v in self.parser.local_refs if k == "manifest"]
        self.assertTrue(manifest_refs, "esp-web-install-button must have a manifest attribute")
        self.assertEqual(manifest_refs[0], "firmware/manifest.json")

    def test_has_required_sections(self):
        required_ids = {
            "hardware",
            "setup",
            "configuration",
            "transport",
            "led",
            "security",
            "recovery",
        }
        missing = required_ids - self.parser.ids
        self.assertFalse(missing, f"missing required section anchors: {missing}")

    def test_local_links_are_relative(self):
        for kind, value in self.parser.local_refs:
            if _is_external_or_fragment(value):
                continue
            self.assertFalse(
                value.startswith("/"),
                f"local {kind} must be relative, found root-relative value: {value}",
            )
            self.assertFalse(
                _escapes_site_root(value),
                f"local {kind} must stay within the published site root "
                f"(Task 3 copies only site/ contents to Pages), found a "
                f"path that traverses above it: {value}",
            )

    def test_states_https_and_web_serial_requirement(self):
        lowered = self.html.lower()
        self.assertIn("https", lowered)
        self.assertIn("web serial", lowered)

    def test_warns_erase_clears_saved_settings(self):
        lowered = self.html.lower()
        self.assertIn("erase", lowered)
        # The warning must connect erasing to losing saved Wi-Fi/UART settings.
        erase_warning = re.search(
            r"eras\w*[^.]*?(wi-?fi|uart)[^.]*?(settings|password|credentials|configuration)",
            lowered,
            re.DOTALL,
        )
        self.assertIsNotNone(
            erase_warning,
            "expected a warning that erasing clears saved Wi-Fi/UART settings",
        )

    def test_loads_pinned_esp_web_tools_module(self):
        self.assertIn(
            "https://unpkg.com/esp-web-tools@10.4.0/dist/web/install-button.js?module",
            self.html,
        )


if __name__ == "__main__":
    unittest.main()
