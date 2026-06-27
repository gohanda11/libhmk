#!/usr/bin/env python3
"""Read PMW3610 diagnostic information over libhmk's Raw HID interface."""

import hid
import struct
import sys

VID = 0xAB50
PID = 0xAB61
RAW_HID_USAGE_PAGE = 0xFFAB
RAW_HID_USAGE = 0xAB
COMMAND_POINTING_DEVICE_INFO = 18


def find_raw_hid_device():
    for info in hid.enumerate(VID, PID):
        if info["usage_page"] == RAW_HID_USAGE_PAGE and info["usage"] == RAW_HID_USAGE:
            return info
    return None


def main():
    dev_info = find_raw_hid_device()
    if dev_info is None:
        print("Error: libhmk Raw HID interface not found.")
        print(f"  VID=0x{VID:04X}, PID=0x{PID:04X}")
        print("  Make sure the keyboard is connected and the firmware supports Raw HID.")
        sys.exit(1)

    path = dev_info["path"]
    dev = hid.device()
    try:
        dev.open_path(path)
        request = bytes([COMMAND_POINTING_DEVICE_INFO]) + bytes(63)
        dev.write(request)
        response = dev.read(64, timeout_ms=1000)
        if not response:
            print("Error: no response from device.")
            sys.exit(1)
    finally:
        dev.close()

    cmd_id, product_id, observation, motion, irq_low, init_ok = struct.unpack_from(
        "<BBBBBB", bytes(response)
    )

    print("PMW3610 diagnostic information:")
    print(f"  command_id   = {cmd_id} (expected {COMMAND_POINTING_DEVICE_INFO})")
    print(f"  product_id   = 0x{product_id:02X} (expected 0x3E)")
    print(f"  observation  = 0x{observation:02X} (expected 0x0F)")
    print(f"  motion       = 0x{motion:02X}")
    print(f"  irq_low      = {irq_low} (1 = motion pin active)")
    print(f"  init_ok      = {init_ok} (1 = init reported success)")

    if init_ok and product_id == 0x3E and (observation & 0x0F) == 0x0F:
        print("Status: SPI communication looks OK.")
    else:
        print("Status: SPI communication or sensor initialization failed.")


if __name__ == "__main__":
    main()
