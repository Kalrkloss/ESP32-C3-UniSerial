#!/usr/bin/env python3
"""Verify the settings-save response offers a delayed return to the portal."""

import ast
import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "main" / "wifi_portal.c"


def save_response_html():
    source = SOURCE.read_text(encoding="utf-8")
    handler = re.search(
        r"static esp_err_t save_handler\(.*?\n}\n\nstatic esp_err_t redirect_to_portal",
        source,
        re.DOTALL,
    )
    if handler is None:
        raise AssertionError("save_handler was not found")

    response = re.search(
        r"httpd_resp_sendstr\(\s*req,\s*((?:\"(?:\\.|[^\"\\])*\"\s*)+)\);",
        handler.group(0),
        re.DOTALL,
    )
    if response is None:
        raise AssertionError("save response HTML was not found")

    literals = re.findall(r'"(?:\\.|[^"\\])*"', response.group(1))
    return "".join(ast.literal_eval(literal) for literal in literals)


def portal_stylesheet():
    source = SOURCE.read_text(encoding="utf-8")
    handler = re.search(
        r"static esp_err_t index_handler\(.*?static esp_err_t save_handler\(",
        source,
        re.DOTALL,
    )
    if handler is None:
        raise AssertionError("index_handler was not found")

    response = re.search(
        r"httpd_resp_sendstr_chunk\(\s*req,\s*((?:\"(?:\\.|[^\"\\])*\"\s*)+)\);",
        handler.group(0),
        re.DOTALL,
    )
    if response is None:
        raise AssertionError("configuration page HTML was not found")

    literals = re.findall(r'"(?:\\.|[^"\\])*"', response.group(1))
    html = "".join(ast.literal_eval(literal) for literal in literals)
    stylesheet = re.search(r"<style>(.*?)</style>", html, re.DOTALL)
    if stylesheet is None:
        raise AssertionError("configuration page stylesheet was not found")
    return stylesheet.group(1)


class SaveRedirectTests(unittest.TestCase):
    def test_save_response_returns_to_portal_after_eight_seconds(self):
        html = save_response_html()
        self.assertRegex(
            html,
            r'<meta\s+http-equiv="refresh"\s+content="8;\s*url=/">',
        )

    def test_save_response_has_a_manual_portal_link(self):
        html = save_response_html()
        self.assertRegex(html, r'<a\s+href="/">[^<]+</a>')


class PortalSpacingTests(unittest.TestCase):
    def test_inline_non_checkbox_controls_have_a_horizontal_gap(self):
        css = portal_stylesheet()
        self.assertRegex(
            css,
            r"label input:not\(\[type=checkbox\]\),label select"
            r"\{margin-left:\.35rem\}",
        )


class WifiPasswordBehaviorTests(unittest.TestCase):
    def test_portal_shows_saved_password_mask_and_removes_clear_checkbox(self):
        source = SOURCE.read_text(encoding="utf-8")
        self.assertIn('name=\\"wifi_password\\"', source)
        self.assertRegex(
            source,
            re.compile(r'name=\\"wifi_password\\".*?value=\\"', re.DOTALL),
        )
        self.assertRegex(
            source,
            re.compile(
                r'if \(err == ESP_OK && s_config\.wifi_password\[0\] != \'\\0\'\)'
                r'\s*\{\s*err = httpd_resp_sendstr_chunk\(req,\s*"\*\*\*\*\*\*"\);',
                re.DOTALL,
            ),
        )
        self.assertNotIn("wifi_password_clear", source)
        self.assertNotIn("Clear saved Wi-Fi password", source)

    def test_save_handler_clears_blank_and_preserves_unchanged_mask(self):
        source = SOURCE.read_text(encoding="utf-8")
        handler = re.search(
            r"static esp_err_t save_handler\(.*?\n}\n\nstatic esp_err_t redirect_to_portal",
            source,
            re.DOTALL,
        )
        if handler is None:
            raise AssertionError("save_handler was not found")
        body = handler.group(0)
        self.assertNotIn(
            "wifi_password_clear",
            body,
        )
        self.assertRegex(
            body,
            r'get_form_value\(body,\s*"wifi_password",\s*wifi_password,\s*'
            r'APP_WIFI_PASSWORD_MAX \+ 1\)',
        )
        self.assertRegex(
            body,
            re.compile(
                r'if \(wifi_password\[0\] == \'\\0\'\)\s*\{\s*'
                r'next_config\.wifi_password\[0\] = \'\\0\';\s*'
                r'\} else if \(strcmp\(wifi_password, "\*\*\*\*\*\*"\) != 0 \|\|\s*'
                r'next_config\.wifi_password\[0\] == \'\\0\'\)\s*\{\s*'
                r'memcpy\(next_config\.wifi_password, wifi_password, '
                r'strlen\(wifi_password\) \+ 1\);',
                re.DOTALL,
            ),
        )


if __name__ == "__main__":
    unittest.main()
