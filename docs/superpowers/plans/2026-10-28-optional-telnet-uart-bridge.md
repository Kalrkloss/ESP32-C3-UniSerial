# Optional Telnet UART Bridge Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an optional Telnet-to-UART bridge configured through the web page, mutually exclusive with USB Serial/JTAG bridge data transfer.

**Architecture:** Persist a Telnet enable flag, port, and optional credentials in `app_config_t`/NVS. Add a dedicated socket bridge module that handles one Telnet client and UART events; `bridge_task` starts either the existing USB data path or Telnet, never both. Keep ESP-IDF/lwIP sockets and the existing Wi-Fi portal; add no third-party dependency.

**Tech Stack:** ESP-IDF v6.1, ESP32-C3, C, FreeRTOS UART event queue, lwIP sockets, Python 3/pyserial hardware test.

**Spec:** `docs/superpowers/specs/2026-10-28-telnet-bridge-design.md`

## Global Constraints

- Target ESP32-C3 with ESP-IDF v6.1.
- Telnet defaults to TCP port 23; valid ports are 1–65535 except 80, reserved for the configuration HTTP server.
- At most one Telnet client is active; a second client is rejected.
- An empty Telnet password disables login; a non-empty password requires a non-empty login name.
- Display a configured password as `******`; saving that unchanged marker preserves it, and an empty field clears it while retaining the inactive login name.
- Telnet credentials/data and the existing HTTP/NVS configuration remain unencrypted; document trusted-network-only use.
- Telnet mode skips USB Serial/JTAG bridge driver installation and both USB bridge tasks. USB flashing and system messages remain available.
- Apply configuration changes after the existing save-and-restart flow; do not silently fall back to USB if Telnet startup fails.
- Consume Telnet IAC commands rather than forwarding them to UART; preserve literal `0xFF` payload bytes.
- Do not add a third-party component, TLS, SSH, runtime mode switching, or multi-client broadcast.

## Review Focus

- Empty, masked, replaced, and cleared passwords: no-login, preserve, replace, or clear exactly as specified; exercise in Task 1's configuration test and post/restart check.
- Port boundary and HTTP conflict values: reject 0 and 80, accept 23 and a custom port; exercise in Task 1's validation and configuration checks.
- Fragmented IAC sequences and literal `0xFF`: consume negotiation and preserve payload byte-for-byte; exercise in Task 2's protocol test.
- No client, a second client, and client disconnect/reconnect: drain UART without stale replay and accept a later client; exercise with the Task 2 socket test and a hardware no-client check.
- Telnet startup failure or Telnet mode selected: never start USB forwarding; test both data modes and inspect the explicit error branch in Task 2.

---

### Task 1: Persist and expose Telnet settings

**Files:**
- Modify: `main/app_config.h`
- Modify: `main/app_config.c`
- Modify: `main/wifi_portal.c`
- Create: `tools/test_telnet_bridge.py`

**Interfaces:**
- Consumes: existing `app_config_t`, `app_config_load()`, `app_config_save()`, `app_config_is_valid()`, and the `/save` restart flow.
- Produces: `app_config_t.telnet_enabled`, `telnet_port`, `telnet_username`, and `telnet_password`; defaults are disabled, port 23, and empty credentials. The test script's `--check-config` mode verifies the rendered web form.

- [ ] **Step 1: Write a failing configuration-page test**

Create `tools/test_telnet_bridge.py` with a `--check-config` mode that fetches the page and asserts it contains the Telnet checkbox, default port 23, username/password fields, and the port-80 warning. Run against the current setup AP before adding the fields.

- [ ] **Step 2: Verify the configuration test fails for the missing fields**

Run:

```powershell
python tools/test_telnet_bridge.py --host 192.168.4.1 --check-config
```

Expected: FAIL because the current page does not yet expose the Telnet fields.

- [ ] **Step 3: Add typed settings, NVS persistence, and validation**

Add `APP_TELNET_USERNAME_MAX` = 32, `APP_TELNET_PASSWORD_MAX` = 64, and a default port constant of 23. Add a `bool`, `uint16_t`, and fixed-size username/password arrays to `app_config_t`. Extend defaults, validation, NVS load, and NVS save using keys `telnet_enabled`, `telnet_port`, `telnet_user`, and `telnet_pass`; store the enable flag as U8 and port as U32. Reject port 0, port 80, out-of-range values, overlong strings, and a non-empty password with an empty username; allow an inactive username with an empty password.

- [ ] **Step 4: Add and validate configuration-page controls**

Add form fields named `telnet_enabled`, `telnet_port`, `telnet_username`, and `telnet_password`. Absence of the checkbox means disabled. Add a numeric port input (1–65535, with port 80 rejected), username field, and password field. Render `******` only when a stored password exists. On POST, preserve the current password only when it is configured and the submitted value is exactly `******`; clear it on an empty value; otherwise save the replacement. Keep the username when the password is cleared. Return the existing 400 response for invalid combinations and do not save or restart on invalid input.

- [ ] **Step 5: Run the configuration-page test and verify persistence**

Run `python tools/test_telnet_bridge.py --host 192.168.4.1 --check-config`; expected: PASS. Then use the page to save disabled/port-23 defaults, set Telnet enabled with a custom port and credentials, and restart. Verify the values persist, an unchanged mask preserves the password, replacing the password changes it, clearing it disables authentication while retaining the username, and invalid ports/credential combinations return 400 without restarting.

- [ ] **Step 6: Build the settings changes**

Run the ESP-IDF v6.1 build command from the repository environment:

```powershell
. C:\Espressif\tools\Microsoft.v6.1\PowerShell_profile.ps1
$env:PYTHONUTF8 = '1'
Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue
idf.py -B build-idf6.1 build
```

Expected: build succeeds; Telnet defaults do not change the existing USB bridge mode.

### Task 2: Implement the Telnet socket bridge and integration test

**Files:**
- Create: `main/telnet_bridge.h`
- Create: `main/telnet_bridge.c`
- Modify: `main/CMakeLists.txt`
- Modify: `main/esp32_uart_bridge.c`
- Modify: `tools/test_telnet_bridge.py`

**Interfaces:**
- Consumes: `const app_config_t *`, the UART port, and the UART event `QueueHandle_t`.
- Produces: `esp_err_t telnet_bridge_start(const app_config_t *config, uart_port_t uart_port, QueueHandle_t uart_queue)`. It binds the configured port, reports socket/bind/listen/task-creation errors, and never enables USB forwarding. `bridge_task` selects exactly one data path.

- [ ] **Step 1: Extend the test script with failing Telnet integration checks**

Extend the script using Python sockets and pySerial. Add `--host`, `--port`, optional `--username`, and optional `--usb-port` arguments; prompt for the password rather than putting it on the command line. Test fragmented Telnet negotiation, valid login, three invalid login attempts, deterministic byte-exact loopback including `0xFF`, rejection of a second client, and reconnect. When `--usb-port` is supplied, send a distinct USB marker while Telnet is connected and fail if it appears in the Telnet echo.

- [ ] **Step 2: Run the integration test before the server exists**

Run:

```powershell
python tools/test_telnet_bridge.py --host 192.168.4.1 --port 23
```

Expected: FAIL because the Telnet endpoint is not yet implemented.

- [ ] **Step 3: Implement the listener, authentication, and Telnet protocol**

Implement `telnet_bridge_start()` and a listener/session task in `main/telnet_bridge.c`. Bind to `INADDR_ANY` on `config->telnet_port`. Accept one client and reject additional connections while it is active. If `config->telnet_password` is empty, begin data forwarding immediately; otherwise prompt for username/password and disconnect after three failed attempts. Implement an incremental IAC state machine that responds DONT/WONT to unsupported options, strips control sequences, and decodes/encodes doubled `0xFF`.

- [ ] **Step 4: Bridge socket and UART data without blocking UART indefinitely**

Write client payload to `uart_port`; consume `UART_DATA` events from `uart_queue` and send their bytes to the active socket. Handle short reads/writes, socket errors, disconnects, and event queue errors explicitly. Drain and discard UART input while no client is connected; close a failed client and return to accept state. Do not retain output for a later client.

- [ ] **Step 5: Select exactly one bridge data path**

Keep UART initialization common. When `telnet_enabled` is false, install the USB Serial/JTAG driver and create the existing USB-to-UART and UART-to-USB tasks. When true, do not install the USB bridge driver or create those tasks; after `wifi_portal_start()` initializes networking, call `telnet_bridge_start(&bridge_config, UBRIDGE_UART_PORT_NUM, uart_queue)`. Log a startup error without starting USB as fallback.

- [ ] **Step 6: Build the integrated firmware**

Run the ESP-IDF v6.1 build command from Task 1. Expected: build succeeds with the Telnet component registered and no new dependency.

- [ ] **Step 7: Run Telnet and USB mode integration checks**

With Telnet disabled, run `python tools/test_usb_serial_jtag_burst.py COM13 --baud 115200 --bytes 65536`; expected: exact echo and checksum. With Telnet enabled, run `python tools/test_telnet_bridge.py --host 192.168.4.1 --port 23 --usb-port COM13`; expected: exact Telnet loopback and no USB-to-UART echo. Repeat on a custom port, with configured credentials (for example `--username test-user`), and on the connected Wi-Fi interface if available. In authenticated mode, the script prompts for the password, tests valid and invalid credentials, and checks that the third invalid attempt disconnects. With a target producing UART output but no Telnet client, verify the output is drained and not replayed after a client connects. Inspect the listener-start failure branch to confirm it logs the error and cannot start USB tasks.

### Task 3: Document and complete the hardware acceptance matrix

**Files:**
- Modify: `README.md`
- Validate: `tools/test_telnet_bridge.py`
- Validate: `tools/test_usb_serial_jtag_burst.py`

**Interfaces:**
- Consumes: the completed configuration page and exclusive data paths.
- Produces: user instructions for enabling Telnet, selecting its port, configuring optional credentials, and understanding its plaintext security limitations.

- [ ] **Step 1: Document Telnet operation and security**

Document default port 23, the valid configurable port range (including port 80 reservation), how to connect from a Telnet client, optional login behavior, the restart requirement, USB/Telnet exclusivity, and the unencrypted trusted-network-only warning.

- [ ] **Step 2: Run the complete acceptance matrix**

On the ESP32-C3, verify: default port 23; custom-port persistence; rejection of ports 0 and 80; no-auth and authenticated Telnet; password preserve/replace/clear behavior; IAC and literal `0xFF` byte preservation; one-client limit and reconnect; USB remains unchanged when Telnet is disabled; and USB bridge payloads are absent when Telnet is enabled. Build once more with ESP-IDF v6.1 and inspect the final diff for unrelated files.

- [ ] **Step 3: Commit the feature**

Stage only the Telnet implementation, its tests, and directly related documentation. Keep `Features.txt`, generated `build-idf6.1/`, and unrelated local artifacts unstaged. Commit with the required Copilot co-author trailer.
