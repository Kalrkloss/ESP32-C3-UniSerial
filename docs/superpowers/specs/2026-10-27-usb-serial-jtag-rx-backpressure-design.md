# USB Serial/JTAG RX Backpressure Design

## Goal

Prevent loss of host-to-UART data when USB Serial/JTAG input arrives faster
than the bridge can drain its software RX buffer. Preserve the existing
`usb_serial_jtag_read_bytes()` API and normal interactive behavior.

## Current context

- Target: ESP32-C3 Super Mini, 4 MB flash.
- Framework: migrate the project from ESP-IDF 5.4.1 to the latest stable
  release, ESP-IDF v6.1.
- USB path: native USB Serial/JTAG to UART1, with UART pins GPIO20 RX and
  GPIO21 TX.
- The project currently uses the ESP-IDF USB Serial/JTAG driver and configures
  the RX ring buffer from the web-configured UART buffer size.
- The observed driver issue is that the RX ISR reads the hardware FIFO before
  checking whether the software ring buffer accepted the packet.

## Architecture

Install ESP-IDF v6.1 side-by-side with the existing v5.4.1 installation. Migrate
and build the firmware against v6.1 before introducing the driver fix so
migration errors and driver changes can be isolated.

Create a small public repository,
`Kalrkloss/esp32-usb-serial-jtag-rxfix`, containing the ESP-IDF
`esp_driver_usb_serial_jtag` component based on the v6.1 source, including its
original license notices and component metadata. Add that repository as a
Git submodule at `components/esp_driver_usb_serial_jtag`. ESP-IDF's component
search order gives project components precedence over built-in IDF components,
so this path overrides the SDK driver without modifying the installed SDK.
Pin the submodule to a reviewed commit.

## RX loss-prevention invariant

After a host USB packet is accepted, every byte must remain owned by exactly
one of:

1. the USB Serial/JTAG hardware RX FIFO;
2. the driver's software RX ring buffer; or
3. a persistent driver-owned pending-packet buffer.

The ISR must never discard a packet when the ring buffer is full. Before
removing a packet from the hardware FIFO, the driver must reserve enough
software capacity for it, or copy it to persistent pending storage. When no
software capacity is available, RX servicing must pause so the host is
backpressured (or its packet is retained in the pending slot). Resume only
after `usb_serial_jtag_read_bytes()` has freed sufficient capacity. Use
ISR-safe operations and explicit synchronization; do not rely on an unchecked
ring-buffer send or a larger ring buffer as the correctness mechanism.

## Scope and compatibility

- Keep the public driver API and the bridge task interfaces unchanged.
- Keep the upstream driver component layout and required CMake/Kconfig/linker
  metadata needed by ESP-IDF v6.1.
- Document the upstream ESP-IDF version and source revision in the component
  repository.
- Do not modify the global ESP-IDF installation.
- Do not change UART framing, Wi-Fi behavior, flash layout, OTA policy, or the
  setup portal as part of this fix.

## Validation and acceptance

1. Build the unmodified bridge after migrating it to ESP-IDF v6.1.
2. Build again with the submodule override selected; confirm the build log
   resolves `esp_driver_usb_serial_jtag` from the project component directory.
3. On the ESP32-C3, send deterministic numbered data substantially larger than
   the configured RX ring buffer while UART output is slower than USB input.
   Compare exact byte count and checksum at the UART receiver over repeated
   runs, including with the Wi-Fi portal active.
4. Repeat with the minimum and maximum configured buffer sizes and verify
   short interactive input still works.
5. Confirm that pausing/resuming RX does not busy-spin, deadlock, or lose a
   packet at the full-to-available transition.

## Risks

- ESP-IDF 6.x migration may require changes unrelated to the RX driver; keep
  those changes separate from the driver fix where practical.
- The USB host retry/backpressure behavior must be verified on hardware, not
  inferred from a successful build.
- The component override must stay aligned with the pinned ESP-IDF component
  source and preserve Espressif's licensing notices.
