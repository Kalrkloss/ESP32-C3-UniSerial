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
STYLES_PATH = os.path.join(SITE_DIR, "styles.css")


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

    def test_hardware_section_has_all_three_connection_diagrams(self):
        hardware_match = re.search(
            r'<section id="hardware">(.*?)</section>', self.html, re.DOTALL
        )
        self.assertIsNotNone(hardware_match, "expected the hardware section")
        hardware = hardware_match.group(1)
        figures = re.findall(r"<figure\b.*?</figure>", hardware, re.DOTALL)
        self.assertEqual(len(figures), 3)
        figure_text = "\n".join(figures).lower()
        for expected in (
            "usb host connection",
            "telnet via an external wi-fi access point",
            "telnet via the esp32-c3 setup access point",
            "target (e.g. microcontroller)",
            "gpio21",
            "gpio20",
            "ground to ground",
        ):
            with self.subTest(expected=expected):
                self.assertIn(expected, figure_text)
        self.assertNotIn("arduino target", figure_text)

    def test_connection_diagrams_have_accessible_descriptions_and_responsive_layout(self):
        hardware_match = re.search(
            r'<section id="hardware">(.*?)</section>', self.html, re.DOTALL
        )
        self.assertIsNotNone(hardware_match, "expected the hardware section")
        hardware = hardware_match.group(1)
        self.assertEqual(len(re.findall(r'<svg\b[^>]*role="img"', hardware)), 3)
        self.assertEqual(len(re.findall(r"<title\b", hardware)), 3)
        self.assertEqual(len(re.findall(r"<desc\b", hardware)), 3)
        self.assertEqual(len(re.findall(r"<figcaption\b", hardware)), 3)

        with open(STYLES_PATH, "r", encoding="utf-8") as handle:
            styles = handle.read()
        self.assertRegex(
            styles,
            r"\.connection-diagram-grid\s*\{[^}]*"
            r"grid-template-columns:\s*repeat\(auto-fit",
        )
        self.assertRegex(
            styles,
            re.compile(
                r"@media \(max-width: 640px\).*?\.connection-diagram svg\s*\{"
                r"[^}]*min-width:\s*40rem",
                re.DOTALL,
            ),
        )

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

    def test_warns_every_install_clears_settings_regardless_of_erase_choice(self):
        """The merged image spans NVS, so every install clears saved settings.

        The site must not promise that declining the erase prompt preserves
        saved Wi-Fi/UART settings: a normal (non-erase) update still writes
        the merged image at offset 0, which overlaps NVS and clears it
        regardless of the installer's erase choice.
        """
        lowered = self.html.lower()
        # The old, false preservation claim must not appear anywhere.
        self.assertNotRegex(
            lowered,
            r"normal (firmware )?update (does not require eras\w*|preserves|does not erase)",
            "site must not claim a normal update preserves saved settings",
        )
        self.assertNotIn(
            "preserves your saved settings",
            lowered,
            "site must not claim saved settings survive a normal update",
        )
        # The warning must instead say every install clears settings,
        # regardless of whether the user declines/accepts the erase prompt.
        clears_every_install = re.search(
            r"every (web flasher )?install\w*[^.]*?clears?[^.]*?(wi-?fi|uart)[^.]*?settings",
            lowered,
            re.DOTALL,
        )
        self.assertIsNotNone(
            clears_every_install,
            "expected a warning that every Web Flasher install clears saved "
            "Wi-Fi/UART settings",
        )
        self.assertIn(
            "regardless",
            lowered,
            "expected the warning to say this happens regardless of the erase choice",
        )
        self.assertTrue(
            "save" in lowered or "reconfigur" in lowered,
            "expected guidance to save/reconfigure settings before/after installing",
        )

    def test_documents_usb_flashing_only_no_system_messages_during_telnet(self):
        """USB is flashing-only; release firmware logs are not exposed over USB.

        Release configuration disables the USB secondary console and
        reassigns UART0 pins, so firmware log/system-message output is never
        exposed over USB in this release, Telnet or not.
        """
        lowered = self.html.lower()
        self.assertNotIn(
            "usb remains available for flashing and system messages",
            lowered,
            "site must not claim USB carries system messages",
        )
        self.assertIn("usb remains available for flashing only", lowered)
        self.assertIn("not exposed over usb", lowered)

    def test_loads_pinned_esp_web_tools_module(self):
        self.assertIn(
            "https://unpkg.com/esp-web-tools@10.4.0/dist/web/install-button.js?module",
            self.html,
        )


if __name__ == "__main__":
    unittest.main()
