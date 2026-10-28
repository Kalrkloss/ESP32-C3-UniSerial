#!/usr/bin/env python3
"""Verify that bridge paths feed the connection/data LED indicator."""

import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def source(path):
    file_path = ROOT / path
    return file_path.read_text(encoding="utf-8") if file_path.exists() else ""


class StatusLedTests(unittest.TestCase):
    def test_led_uses_gpio8_active_low_and_usb_host_presence(self):
        led_source = source(Path("main") / "bridge_status_led.c")
        self.assertTrue("GPIO_NUM_8" in led_source, "LED pin should be GPIO8")
        self.assertTrue(
            "usb_serial_jtag_is_connected()" in led_source,
            "USB mode should use the USB host connection state",
        )
        self.assertTrue(
            "should_be_on ? 0 : 1" in led_source,
            "GPIO8 output should respect the active-low LED",
        )

    def test_usb_data_directions_report_activity(self):
        bridge_source = source(Path("main") / "esp32_uart_bridge.c")
        self.assertGreaterEqual(
            bridge_source.count("bridge_status_led_activity()"), 2
        )

    def test_telnet_connection_and_data_events_report_status(self):
        telnet_source = source(Path("main") / "telnet_bridge.c")
        self.assertTrue(
            "bridge_status_led_set_telnet_connected(true)" in telnet_source,
            "Telnet accept should report a connected client",
        )
        self.assertTrue(
            "bridge_status_led_set_telnet_connected(false)" in telnet_source,
            "Telnet close should report a disconnected client",
        )
        self.assertGreaterEqual(
            telnet_source.count("bridge_status_led_activity()"), 2
        )

    def test_led_activity_uses_a_timed_background_task(self):
        led_source = source(Path("main") / "bridge_status_led.c")
        self.assertTrue("20000" in led_source, "LED quiet interval should be 20 ms")
        self.assertTrue(
            "STATUS_LED_TASK_PERIOD_MS 5" in led_source,
            "LED task should sample the short activity window every 5 ms",
        )
        self.assertTrue("vTaskDelay" in led_source, "LED timing must be asynchronous")

    def test_sustained_activity_flashes_instead_of_staying_off(self):
        led_source = source(Path("main") / "bridge_status_led.c")
        self.assertTrue(
            "STATUS_LED_ACTIVITY_CYCLE_US 100000" in led_source,
            "Sustained activity should use a 10 Hz flash cycle",
        )
        self.assertTrue(
            "(now_us - activity_started_us) % STATUS_LED_ACTIVITY_CYCLE_US"
            in led_source,
            "LED state should alternate during sustained activity",
        )
        self.assertTrue(
            "connected && (!activity_active || activity_led_on)" in led_source,
            "A connected LED should flash during activity and stay on otherwise",
        )


if __name__ == "__main__":
    unittest.main()
