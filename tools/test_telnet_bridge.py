#!/usr/bin/env python3
"""Verify the Telnet configuration page and UART bridge on a live device."""

import argparse
import getpass
import re
import socket
import sys
import time
from urllib.error import URLError
from urllib.request import urlopen

IAC = 255
TELNET_VERBS = {251, 252, 253, 254}


class TelnetSession:
    def __init__(self, connection):
        self.connection = connection
        self.wire = bytearray()
        self.data = bytearray()

    def send_raw(self, payload):
        self.connection.sendall(payload)

    def send_data(self, payload):
        self.send_raw(payload.replace(b"\xff", b"\xff\xff"))

    def _decode(self, payload):
        self.wire.extend(payload)
        while self.wire:
            if self.wire[0] != IAC:
                self.data.append(self.wire.pop(0))
                continue
            if len(self.wire) < 2:
                return
            command = self.wire[1]
            if command == IAC:
                self.data.append(IAC)
                del self.wire[:2]
            elif command in TELNET_VERBS:
                if len(self.wire) < 3:
                    return
                del self.wire[:3]
            elif command == 250:
                end = self.wire.find(b"\xff\xf0", 2)
                if end < 0:
                    return
                del self.wire[: end + 2]
            else:
                del self.wire[:2]

    def _recv(self, timeout):
        self.connection.settimeout(timeout)
        payload = self.connection.recv(4096)
        if not payload:
            raise ConnectionError("Telnet connection closed")
        self._decode(payload)

    def read_until(self, marker, timeout=5):
        deadline = time.monotonic() + timeout
        while True:
            position = self.data.find(marker)
            if position >= 0:
                end = position + len(marker)
                result = bytes(self.data[:end])
                del self.data[:end]
                return result
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError(f"timed out waiting for {marker!r}")
            self._recv(remaining)

    def read_exactly(self, count, timeout=10):
        deadline = time.monotonic() + timeout
        while len(self.data) < count:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError(
                    f"received {len(self.data)} of {count} expected bytes"
                )
            self._recv(remaining)
        result = bytes(self.data[:count])
        del self.data[:count]
        return result

    def read_wire_exactly(self, count, timeout=3):
        if self.wire:
            raise AssertionError("unexpected partial Telnet control sequence")
        received = bytearray()
        deadline = time.monotonic() + timeout
        while len(received) < count:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("timed out waiting for Telnet negotiation")
            self.connection.settimeout(remaining)
            payload = self.connection.recv(count - len(received))
            if not payload:
                raise ConnectionError("Telnet connection closed")
            received.extend(payload)
        return bytes(received)

    def wait_for_data(self, timeout):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                self._recv(min(0.1, deadline - time.monotonic()))
            except socket.timeout:
                continue
            except ConnectionError:
                break
        result = bytes(self.data)
        self.data.clear()
        return result

    def close(self):
        self.connection.close()


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True, help="Device hostname or IP")
    parser.add_argument("--port", type=int, default=23, help="Telnet TCP port")
    parser.add_argument("--username", help="Telnet login name, if authentication is enabled")
    parser.add_argument("--usb-port", help="USB Serial/JTAG port to check for isolation")
    parser.add_argument("--check-config", action="store_true")
    args = parser.parse_args()
    if not 1 <= args.port <= 65535 or args.port == 80:
        parser.error("--port must be 1-65535 and cannot be 80")
    return args


def check_config(host):
    with urlopen(f"http://{host}/", timeout=10) as response:
        page = response.read().decode("utf-8")

    required = {
        "Telnet enable checkbox": r'<input[^>]*type="checkbox"[^>]*name="telnet_enabled"',
        "default Telnet port 23": r'<input[^>]*name="telnet_port"[^>]*value="23"',
        "Telnet username field": r'<input[^>]*name="telnet_username"',
        "Telnet password field": r'<input[^>]*name="telnet_password"',
        "port 80 warning": r"(?i)port.{0,40}80|80.{0,40}port",
    }
    missing = [
        label
        for label, pattern in required.items()
        if re.search(pattern, page) is None
    ]
    if missing:
        raise AssertionError(
            "configuration page is missing: " + ", ".join(missing)
        )


def connect_telnet(host, port, username, password):
    connection = socket.create_connection((host, port), timeout=5)
    session = TelnetSession(connection)
    if username is not None:
        session.read_until(b"Username: ")
        session.send_data(username.encode("utf-8") + b"\r\n")
        session.read_until(b"Password: ")
        session.send_data(password.encode("utf-8") + b"\r\n")
        session.read_until(b"Login successful\r\n")
    return session


def check_failed_logins(host, port, username, password):
    session = connect_telnet(host, port, None, None)
    try:
        session.read_until(b"Username: ")
        for attempt in range(3):
            session.send_data((username + "-wrong").encode("utf-8") + b"\r\n")
            session.read_until(b"Password: ")
            session.send_data((password + "-wrong").encode("utf-8") + b"\r\n")
            if attempt < 2:
                session.read_until(b"Username: ")
        session.read_until(b"Login incorrect\r\n")
        session.connection.settimeout(3)
        if session.connection.recv(1) != b"":
            raise AssertionError("server did not close after three failed logins")
    except ConnectionResetError:
        pass
    finally:
        session.close()


def send_loopback(session):
    payload = bytes(range(256)) + bytes(
        (index * 73 + 19) & 0xFF for index in range(4096)
    )
    session.send_data(payload)
    received = session.read_exactly(len(payload))
    if received != payload:
        mismatch = next(
            index
            for index, (expected, actual) in enumerate(zip(payload, received))
            if expected != actual
        )
        raise AssertionError(f"loopback payload differs at byte {mismatch}")


def check_usb_isolation(usb_port, session):
    try:
        import serial
    except ImportError as exc:
        raise RuntimeError("pySerial is required when --usb-port is used") from exc

    marker = b"USB-MUST-NOT-BRIDGE-7D31"
    port = serial.Serial(port=None, baudrate=115200, timeout=0.1, write_timeout=2)
    port.rts = False
    port.dtr = False
    port.port = usb_port
    try:
        port.open()
        port.reset_input_buffer()
        port.write(marker)
        port.flush()
        deadline = time.monotonic() + 0.5
        usb_data = bytearray()
        while time.monotonic() < deadline:
            usb_data.extend(port.read(256))
        if marker in usb_data:
            raise AssertionError("USB data was forwarded to UART while Telnet was active")
    finally:
        port.close()

    if marker in session.wait_for_data(0.5):
        raise AssertionError("USB marker was echoed through the Telnet session")


def check_second_client(host, port):
    try:
        connection = socket.create_connection((host, port), timeout=3)
    except ConnectionRefusedError:
        return
    connection.settimeout(3)
    try:
        if connection.recv(1) != b"":
            raise AssertionError("additional Telnet client was not rejected")
    except ConnectionResetError:
        pass
    finally:
        connection.close()


def run_integration(args, password):
    if args.username is not None:
        check_failed_logins(args.host, args.port, args.username, password)

    session = connect_telnet(args.host, args.port, args.username, password)
    try:
        session.send_raw(b"\xff")
        time.sleep(0.05)
        session.send_raw(b"\xfd\x2a")
        if session.read_wire_exactly(3) != b"\xff\xfc\x2a":
            raise AssertionError("server did not reject unsupported Telnet option")
        send_loopback(session)
        if args.usb_port:
            check_usb_isolation(args.usb_port, session)
        check_second_client(args.host, args.port)
        send_loopback(session)
    finally:
        session.close()

    reconnected = connect_telnet(args.host, args.port, args.username, password)
    try:
        send_loopback(reconnected)
    finally:
        reconnected.close()


def main():
    args = parse_args()
    try:
        if args.check_config:
            check_config(args.host)
            print("Telnet configuration fields are present.")
            return 0
        password = (
            getpass.getpass("Telnet password: ")
            if args.username is not None
            else None
        )
        run_integration(args, password)
    except (AssertionError, ConnectionError, OSError, TimeoutError, URLError, RuntimeError) as exc:
        print(f"Telnet bridge check failed: {exc}", file=sys.stderr)
        return 1
    print("Telnet negotiation, authentication, loopback, and reconnect checks passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
