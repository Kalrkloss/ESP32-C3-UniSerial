#!/usr/bin/env python3
"""Verify USB Serial/JTAG to UART loopback delivery under host bursts."""

import argparse
import hashlib
import random
import sys
import threading
import time

import serial


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port", help="USB Serial/JTAG port, for example COM13")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--bytes", dest="byte_count", type=int, default=65536)
    args = parser.parse_args()
    if args.baud <= 0:
        parser.error("--baud must be positive")
    if args.byte_count <= 0:
        parser.error("--bytes must be positive")
    return args


def prime_loopback(port):
    marker = bytes(range(64))
    received = bytearray()
    sent = 0
    deadline = time.monotonic() + 5.0
    quiet_deadline = None

    while sent < len(marker):
        written = port.write(marker[sent:])
        if written <= 0:
            raise TimeoutError("serial preflight write made no progress")
        sent += written

    while time.monotonic() < deadline:
        chunk = port.read(4096)
        if chunk:
            received.extend(chunk)
            if marker in received:
                quiet_deadline = time.monotonic() + 0.25
        elif quiet_deadline is not None and time.monotonic() >= quiet_deadline:
            return

    if marker not in received:
        raise TimeoutError("serial preflight loopback did not echo its marker")


def main():
    args = parse_args()
    payload_rng = random.Random(0xC0FFEE)
    payload = bytes(payload_rng.getrandbits(8) for _ in range(args.byte_count))
    expected_checksum = hashlib.sha256(payload).hexdigest()
    timeout = max(15.0, args.byte_count * 10 / args.baud * 3 + 5.0)
    received = bytearray()
    read_error = []
    stop_reader = threading.Event()

    try:
        port = serial.Serial(
            port=None,
            baudrate=args.baud,
            timeout=0.1,
            write_timeout=timeout,
        )
        port.rts = False
        port.dtr = False
        port.port = args.port
        port.open()
    except (serial.SerialException, OSError) as exc:
        print(f"Could not open {args.port}: {exc}", file=sys.stderr)
        return 2

    def read_echo():
        deadline = time.monotonic() + timeout
        quiet_deadline = None
        try:
            while not stop_reader.is_set():
                if len(received) >= len(payload):
                    if quiet_deadline is None:
                        quiet_deadline = time.monotonic() + 0.25
                    if time.monotonic() >= quiet_deadline:
                        break
                    chunk = port.read(4096)
                    if chunk:
                        received.extend(chunk)
                        quiet_deadline = time.monotonic() + 0.25
                    continue

                if time.monotonic() >= deadline:
                    break
                chunk = port.read(min(4096, len(payload) - len(received)))
                if chunk:
                    received.extend(chunk)
        except (serial.SerialException, OSError) as exc:
            read_error.append(exc)

    try:
        port.reset_input_buffer()
        prime_loopback(port)
        port.reset_input_buffer()
        reader = threading.Thread(target=read_echo, daemon=True)
        reader.start()

        sent = 0
        while sent < len(payload):
            written = port.write(payload[sent : sent + 4096])
            if written <= 0:
                raise TimeoutError("serial write made no progress")
            sent += written

        reader.join(timeout + 1.0)
        if reader.is_alive():
            stop_reader.set()
            reader.join()
            raise TimeoutError("timed out waiting for the loopback echo")
    except (serial.SerialException, OSError, TimeoutError) as exc:
        stop_reader.set()
        print(f"Burst transfer failed: {exc}", file=sys.stderr)
        return 2
    finally:
        port.close()

    actual = hashlib.sha256(received).hexdigest()
    print(f"Sent: {len(payload)} bytes; received: {len(received)} bytes")
    print(f"SHA-256 expected: {expected_checksum}")
    print(f"SHA-256 received: {actual}")

    failed = False
    if len(received) != len(payload):
        print(
            f"Byte-count mismatch: expected {len(payload)}, received {len(received)}",
            file=sys.stderr,
        )
        failed = True
    if received != payload:
        mismatch = next(
            (
                index
                for index, (expected_byte, received_byte) in enumerate(
                    zip(payload, received)
                )
                if expected_byte != received_byte
            ),
            min(len(payload), len(received)),
        )
        print(f"Order/content mismatch at byte {mismatch}", file=sys.stderr)
        failed = True
    if actual != expected_checksum:
        print("Checksum mismatch", file=sys.stderr)
        failed = True
    if read_error:
        print(f"Serial read failed: {read_error[0]}", file=sys.stderr)
        failed = True
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
