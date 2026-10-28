# Optional Telnet UART Bridge Design

## Goal

Allow the ESP32-C3 bridge to expose its hardware UART over Telnet when
explicitly enabled in the configuration page. Telnet and USB Serial/JTAG
bridge data paths are mutually exclusive: Telnet mode must not install or run
the USB bridge driver/tasks. USB remains available for firmware flashing and
system messages.

## Agreed requirements

- Add a persistent Telnet enable setting to the existing web configuration
  page. Configuration changes take effect after the existing save-and-restart
  flow.
- Telnet listens on TCP port 23 and is reachable on the active setup AP or
  connected Wi-Fi interface.
- Permit one Telnet client at a time. Reject additional clients while one is
  connected; accept new connections after disconnect.
- Bridge bytes in both directions between the connected Telnet client and the
  configured UART.
- With Telnet disabled, preserve the current USB Serial/JTAG-to-UART behavior.
- When Telnet is enabled, do not install the USB Serial/JTAG bridge driver or
  create either USB bridge task. Do not silently fall back to USB if Telnet
  startup fails.
- The configuration page accepts a Telnet login name and password. If no
  password is configured, Telnet requires no login. If a password is set, a
  non-empty login name is required.
- When a password is configured, display `******` in its field. Saving that
  unchanged mask preserves the stored password. Clearing the field removes
  the password and disables Telnet authentication; retain the login name as an
  inactive value.
- Cap authentication at three failed attempts per connection, then disconnect.
- Handle Telnet IAC negotiation sufficiently to prevent negotiation bytes from
  being forwarded as UART payload, and preserve literal `0xFF` payload bytes.
- Plaintext is accepted for this use: Telnet credentials/data are unencrypted;
  configuration uses HTTP and credentials are stored in the existing NVS
  partition without encryption. This is intended only for trusted local
  networks.

## Current context

- Target: ESP32-C3 with ESP-IDF v6.1.
- The existing firmware initializes UART1 and creates USB-to-UART and
  UART-to-USB tasks unconditionally in `main/esp32_uart_bridge.c`.
- Runtime UART and Wi-Fi settings are loaded and saved by `main/app_config.c`
  and rendered/handled by `main/wifi_portal.c`.
- The configuration page currently applies settings by saving to NVS and
  restarting the device.
- Wi-Fi operation uses either the setup AP or station mode; after station
  connection failures it falls back to the setup AP.

## Architecture

### Configuration and startup

Extend `app_config_t` and NVS persistence with a Telnet enable flag, login
name, and password. Add matching controls to the existing configuration page.
Validate lengths and reject a non-empty password with an empty login name.
Treat an empty password as authentication disabled, even if an inactive login
name remains stored.

The bridge initializes the UART in either mode. If Telnet is disabled, retain
the existing USB Serial/JTAG driver installation and two USB bridge tasks. If
Telnet is enabled, skip USB bridge installation and task creation, then start
the Telnet listener alongside the existing Wi-Fi portal. Bind to all local
interfaces so the service is available on whichever AP or station interface
is active. A listener/task startup error is logged and leaves the USB data
path disabled; it must not change the selected mode implicitly.

### Telnet session and byte flow

Use the existing ESP-IDF/lwIP socket APIs; do not add a third-party component.
The listener owns the listening socket and accepts one client. A connected
session has one receive path that writes client payload to UART and one UART
receive path that writes bytes to the socket. Handle short reads/writes and
disconnects explicitly. Close a failed client and return to accept state.

Implement minimal Telnet IAC handling: negotiate unsupported options off,
consume Telnet control sequences instead of forwarding them to UART, and
encode/decode doubled IAC bytes so literal `0xFF` data remains byte-exact.
When authentication is enabled, prompt for login and password before enabling
the data paths. Disconnect after three failed attempts.

Drain and discard UART input while no Telnet client is connected so an absent
or slow client cannot indefinitely block UART reads. Report UART overflow and
socket errors through existing ESP logging. Do not buffer historical UART
output for a later client.

### Configuration-page password behavior

The stored password is never returned to the browser. When configured, render
the password input with the literal mask `******`. On save, an unchanged mask
means preserve the current password; an empty field means clear it; any other
value replaces it. Reserve the exact value `******` as the unchanged marker.
The page must communicate these semantics next to the password field.

## Scope and non-goals

- No TLS, SSH, web terminal, multi-client broadcast, or runtime mode switching
  without restart.
- Do not remove USB firmware flashing or system logging in Telnet mode.
- Do not change UART framing, pins, Wi-Fi setup/fallback behavior, or the
  existing USB RX backpressure fix.
- Do not add a third-party Telnet dependency.

## Validation and acceptance

1. Build with the project's ESP-IDF v6.1 environment.
2. With Telnet disabled, verify the existing USB Serial/JTAG loopback and
   burst-regression behavior remains unchanged.
3. With Telnet enabled and the device restarted, verify UART loopback through a
   standard Telnet client on the setup AP and on the connected Wi-Fi interface
   when each mode is active. Verify USB bridge payload forwarding is absent.
4. Verify no-password mode starts forwarding without a login prompt; verify
   configured credentials allow access, wrong credentials are rejected, and
   the third failed attempt closes the session.
5. Verify the password mask preserves the stored password, clearing the field
   disables authentication while retaining the login name, and a replacement
   password takes effect after restart.
6. Verify Telnet negotiation bytes do not reach UART, literal `0xFF` bytes
   round-trip, a second simultaneous client is rejected, and a disconnected
   client can reconnect.
7. Verify socket errors and listener startup failures are logged and never
   cause a USB bridge fallback.

## Risks

- Telnet and the current HTTP configuration endpoint expose credentials and
  UART traffic in plaintext; users must restrict access to trusted networks.
- A slow or absent Telnet client cannot preserve unbounded UART output. Output
  received with no client is intentionally discarded, and socket/UART buffer
  limits still constrain slow-client behavior.
- Telnet clients vary in option negotiation; the IAC handling must be tested
  with a standard terminal client as well as a byte-exact loopback test.
