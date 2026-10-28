Original project: [aliengreen/esp32_uart_bridge](https://github.com/aliengreen/esp32_uart_bridge).

This is an independent, substantial redesign of the original project. It is maintained as a separate repository because the planned architecture and feature set differ significantly from the upstream project. The original GPL-3.0 license and attribution are retained.

# ESP32-C3-UniSerial

| Supported Target | ESP32-C3 |
| ---------------- | -------- |

ESP32-C3-UniSerial is an ESP-IDF project that bridges USB Serial/JTAG and a hardware UART. The current firmware uses independent transfer tasks to reduce bridge latency.
Data is transferred in both directions between the computer and the target MCU.

## Current configuration features

- With no saved Wi-Fi network, the device starts an open setup network named `UniSerial-xxxxxx`; open `http://192.168.4.1` for the captive configuration page.
- The page scans for Wi-Fi networks, saves the selected network, and configures the setup AP channel. In station mode, the router controls the channel. If the saved network cannot be reached, the device falls back to the setup AP.
- UART baud rate, buffer size, data bits, parity, stop bits, and RX/TX signal inversion are stored in NVS and applied after restart.
- The optional Telnet UART bridge is disabled by default. When enabled, it replaces USB Serial/JTAG data forwarding; USB remains available for flashing and system messages.
- The ESP32-C3 USB Serial/JTAG hardware ignores the host's CDC line-coding baud value; the host terminal cannot change the UART baud rate.

After a successful Wi-Fi connection, the same page is available at the device's DHCP address; find it in the router's client list. The 4 MB flash layout uses one large application and NVS; OTA updates are not enabled.

## Telnet UART bridge

Enable Telnet from the configuration page; changes take effect after the device restarts. The default port is `23`. Configure any port from `1` to `65535`, except `80`, which is reserved for the configuration page. Connect to the device's setup-AP or Wi-Fi address with a Telnet client, for example:

```shell
telnet 192.168.4.1 23
```

Authentication is optional. Leave the password empty for no login; otherwise set both a username and password. A saved password is shown as `******`; submitting that unchanged mask preserves it, while clearing the field removes it and disables authentication. Only one Telnet client can connect at a time. UART output received without an authenticated client is discarded rather than replayed later.

Telnet and configuration-page traffic are unencrypted, and credentials are stored in unencrypted NVS. The setup AP is open and the configuration page has no login. Use both services only on trusted local networks; neither is suitable for untrusted environments.

> NOTE: Firmware tested on ESP32-C3. But it may work in other ESP32 family.

See diagram below:

```
 COMPUTER            ESP32-C3             MCU
┌────────┐     ┌────────┬────────┐     ┌────────┐
│        │     │        │        │     │        │
│        │     │        │        │     │        │
│        │     │        │        │     │        │
│        │     │        │        │     │        │
│  USB   │◄───►│  CDC   │  UART  │◄───►│  UART  │
│        │     │        │        │     │        │
│        │     │        │        │     │        │
│        │     │        │        │     │        │
│        │     │        │        │     │        │
└────────┘     └────────┴────────┘     └────────┘
```


### Hardware Required

The example can be run on ESP32-C3 based development board connected to a computer with a single USB cable for flashing and bridging. The target MCU interface should have 3.3V logic. 

### Setup the Hardware

Connect the target MCU serial interface to the ESP32-C3 as follows.


-----------------------------------------------------------------------------------------
| ESP32-C3 chip         | Kconfig Option     | ESP32-C3 Pin | Target MCU UART Pin |
| ----------------------|--------------------|----------------------|--------------------|
| Transmit Data (TxD)   | UBRIDGE_UART_TXD | GPIO21             | RxD               |
| Receive Data (RxD)    | UBRIDGE_UART_RXD | GPIO20         | TxD               |
| Ground                | n/a                | GND                  | GND               |

See connection diagram below:

```
 ESP32-C3           MCU
┌────────┐       ┌────────┐
│        │       │        │
│        │       │        │
│ GPIO21 ├──────►│  RxD   │
│        │ 3.3v  │        │
│        │ logic │        │
│ GPIO20 │◄──────┤  TxD   │
│        │       │        │
│        │       │        │
└────────┘       └────────┘
```

### RX burst regression test

For a loopback test, connect ESP32-C3 GPIO21 (UART TX) to GPIO20 (UART RX), then run:

```shell
python -m pip install pyserial
python tools/test_usb_serial_jtag_burst.py COM13 --baud 115200 --bytes 65536
```

Replace `COM13` with the board's USB Serial/JTAG port and set `--baud` to the UART baud configured on the device. The test disables DTR/RTS, removes startup output with a short loopback preflight, and verifies byte count, order, and SHA-256. Use `--bytes 65537` to include a final partial USB packet.

### Status LED hardware limitation

The SuperMini status LED intentionally uses GPIO8. On this hardware, GPIO8 is also wired as the external-target BOOT0 control. While status LED activity is enabled, GPIO8 must not be used for external target boot control.


## How to Compile the Project

The project is built and tested with [ESP-IDF](https://github.com/espressif/esp-idf) v6.1. Please read the ESP-IDF documentation for setting up the environment.

The USB Serial/JTAG driver is tracked as a submodule at `components/esp_driver_usb_serial_jtag` from [Kalrkloss/esp32-usb-serial-jtag-rxfix](https://github.com/Kalrkloss/esp32-usb-serial-jtag-rxfix), pinned to `58639fe0d87cdb4c080a7a7b0f25f7e5323cbaa7`. Initialize it after cloning with `git submodule update --init --recursive`.

- `idf.py menuconfig` can be used to change the default configuration. The project-specific settings are in the "ESP32 Uart Bridge Configuration" sub-menu.
- `idf.py build` will build the project binaries.
- `idf.py -p PORT flash` will flash the ESP32-C3. The `PORT` is the serial port created by an ESP32-C3 USB-CDC profile connected to the USB D+/D- pins of the ESP32-C3. This serial connection has to be established to upload firmware into the ESP32-C3 and it also used to act as bridge. 

The initial flashing can be done by using other methods, see [this guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/get-started/establish-serial-connection.html).

### Configure the project

Use the command below to configure project using Kconfig menu as showed in the table above.
The default Kconfig values can be changed such as: `UBRIDGE_UART_BAUD_RATE`, `UBRIDGE_UART_PARITY`,  `UBRIDGE_UART_PORT_NUM` (Refer to Kconfig file).

```
idf.py menuconfig
```

> NOTE: This repository defaults to `115200` baud and UART port `1` on the ESP32-C3.
> Runtime UART settings are saved in NVS by the web UI and applied after restart. The ESP32-C3 USB Serial/JTAG controller ignores the host's CDC line-coding baud value.
>
> WARNING: The parity setting by default selected `none`. If you are going to flash STM32 using the ST serial bootloader you MUST select `even` parity. 

### Build and Flash

Build the project and flash it to the board:

```
idf.py set-target esp32c3
idf.py build
idf.py -p PORT flash
```
