#!/usr/bin/env python3
"""Inspect USB topology and kernel-reported zoned disks on Linux."""

import argparse
import json
import re
from pathlib import Path

ROOT_HUB = re.compile(r"usb[0-9]+")
USB_DEVICE = re.compile(r"[0-9]+-[0-9]+(?:\.[0-9]+)*")


def attr(path, name):
    try:
        return (path / name).read_text(errors="replace").strip() or None
    except OSError:
        return None


def scan(sysfs):
    usb_bus = sysfs / "bus/usb/devices"
    block_class = sysfs / "class/block"
    if not usb_bus.is_dir() or not block_class.is_dir():
        raise ValueError("需要 Linux 上已挂载的 sysfs（默认 /sys）。")

    controllers = {}
    devices = {}
    for entry in sorted(usb_bus.iterdir()):
        port = entry.name
        if ROOT_HUB.fullmatch(port):
            controller = entry.resolve().parent
            record = controllers.setdefault(
                controller, {"path": str(controller), "root_hubs": []}
            )
            record["root_hubs"].append({"name": port, "speed_mbps": attr(entry, "speed")})
        elif USB_DEVICE.fullmatch(port):
            vendor = attr(entry, "idVendor")
            product = attr(entry, "idProduct")
            if not vendor or not product:
                continue
            drivers = {
                (interface / "driver").resolve().name
                for interface in usb_bus.glob(f"{port}:*")
                if (interface / "driver").is_symlink()
            }
            devices[entry.resolve()] = {
                "port": port,
                "vid_pid": f"{vendor}:{product}",
                "product": attr(entry, "product"),
                "device_class": attr(entry, "bDeviceClass"),
                "drivers": sorted(drivers),
                "disks": [],
            }

    zoned_disks = []
    for entry in sorted(block_class.iterdir()):
        if (entry / "partition").exists():
            continue
        zoned = attr(entry, "queue/zoned")
        disk = {
            "name": entry.name,
            "model": attr(entry, "device/model"),
            "zoned": zoned,
            "usb_port": None,
        }
        for parent in entry.resolve().parents:
            device = devices.get(parent)
            if device is not None:
                disk["usb_port"] = device["port"]
                device["disks"].append(disk)
                break
        if zoned in {"host-managed", "host-aware"}:
            zoned_disks.append(disk)

    for device in devices.values():
        drivers = set(device["drivers"])
        if any(disk["zoned"] == "host-managed" for disk in device["disks"]):
            status = "host-managed"
        elif device["disks"]:
            status = "block-device-present"
        elif drivers & {"uas", "usb-storage"}:
            status = "storage-driver-without-block-device"
        else:
            status = "usb-device-present"
        device["status"] = status

    return {
        "usb_controllers": list(controllers.values()),
        "usb_devices": list(devices.values()),
        "zoned_disks": zoned_disks,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sysfs", type=Path, default=Path("/sys"))
    args = parser.parse_args()
    try:
        result = scan(args.sysfs)
    except (OSError, ValueError) as error:
        parser.exit(1, f"{error}\n")
    print(json.dumps(result, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
