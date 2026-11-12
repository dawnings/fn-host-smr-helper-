"""Read and trigger reclaim for registered, verified dm-zoned mappings."""

import fcntl
import json
import re
import subprocess


NAME = re.compile(r"SMR_[A-Za-z0-9_.+-]+")
STATUS = re.compile(
    r"(?P<name>SMR_[A-Za-z0-9_.+-]+): 0 \d+ zoned (?P<zones>\d+) zones "
    r"(?:(?P<cache>\d+/\d+) cache )?(?P<random>\d+/\d+) random "
    r"(?P<sequential>\d+/\d+) sequential"
)


def command(*args):
    result = subprocess.run(args, text=True, capture_output=True, timeout=12)
    if result.returncode:
        raise RuntimeError(result.stderr.strip() or result.stdout.strip() or args[0] + " failed")
    return result.stdout


def mappings(var):
    try:
        records = json.loads((var / "config.json").read_text())["disks"]
    except FileNotFoundError:
        return {}
    registered = {r["dm"]: r["serial"] for r in records
                  if isinstance(r.get("dm"), str) and NAME.fullmatch(r["dm"])
                  and isinstance(r.get("serial"), str) and r["serial"]}
    if not registered:
        return {}
    tree = json.loads(command("lsblk", "-J", "-o", "NAME,TYPE,SERIAL,ZONED"))["blockdevices"]
    live = {child["name"]: disk["serial"] for disk in tree
            if disk["type"] == "disk" and disk["zoned"] == "host-managed"
            for child in disk.get("children", []) if child["type"] == "dmz"}
    result = {}
    for line in command("dmsetup", "status", "--noflush", "--target", "zoned").splitlines():
        match = STATUS.fullmatch(line.strip())
        if not match:
            continue
        status = match.groupdict()
        name = status["name"]
        serial = registered.get(name)
        if serial and live.get(name) == serial:
            result[serial] = status
    return result


def reclaim(var, serial):
    if not isinstance(serial, str) or not serial:
        raise ValueError("请选择已登记的硬盘。")
    with open("/run/smr-manager.lock", "a") as guard:
        try:
            fcntl.flock(guard, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise ValueError("有磁盘操作正在运行，请稍后再试。") from error
        status = mappings(var).get(serial)
        if not status:
            raise ValueError("该硬盘没有可验证的活动 dm-zoned 映射。")
        command("dmsetup", "message", status["name"], "0", "reclaim")
        return status["name"]
