"""Recover missing fnOS native registrations from active storage topology."""

import fcntl
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile


SERIAL = re.compile(r"[A-Za-z0-9_-]+")
VG = re.compile(r"trim_[0-9a-f]{8}(?:_[0-9a-f]{4}){3}_[0-9a-f]{12}")
UUID = re.compile(r"[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}")


def candidates(tree, by_id):
    result = []
    for disk in tree:
        serial, name = disk.get("serial"), disk.get("name")
        if (disk.get("type") != "disk" or disk.get("zoned") != "host-managed"
                or not isinstance(serial, str) or not SERIAL.fullmatch(serial)
                or not isinstance(name, str)):
            continue
        ids = [p for p in by_id.glob(f"ata-*_{serial}")
               if p.is_symlink() and p.resolve().name == name]
        if len(ids) != 1:
            continue
        for dm in disk.get("children", []):
            if (dm.get("name"), dm.get("type"), dm.get("fstype")) != (
                    f"SMR_{serial}", "dmz", "LVM2_member"):
                continue
            for lv in dm.get("children", []):
                vg, suffix = (lv.get("name") or "").rsplit("-", 1) if "-" in (lv.get("name") or "") else ("", "")
                uuid = lv.get("uuid") or ""
                if (suffix == "0" and VG.fullmatch(vg) and UUID.fullmatch(uuid)
                        and lv.get("type") == "lvm" and lv.get("fstype") == "btrfs"):
                    result.append({"serial": serial, "id": str(ids[0]), "dm": dm["name"],
                                   "vg": vg, "uuid": uuid, "mode": "native"})
    return [d for d in result if all(sum(x[key] == d[key] for x in result) == 1
                                      for key in ("serial", "dm", "vg", "uuid"))]


def missing_records(config):
    tree = json.loads(subprocess.check_output([
        "lsblk", "-J", "-o", "NAME,TYPE,FSTYPE,UUID,SERIAL,ZONED",
    ], timeout=10))["blockdevices"]
    current = config["disks"]
    return [record for record in candidates(tree, Path("/dev/disk/by-id"))
            if not any(any(d.get(key) == record[key] for key in ("serial", "id", "dm", "vg", "uuid"))
                       for d in current)]


def rebuild(var, expected=None):
    config_path = var / "config.json"
    with open("/run/smr-manager.lock", "a") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise ValueError("有磁盘任务正在运行，请稍后再试。") from error
        if config_path.is_symlink():
            raise ValueError("配置文件路径无效。")
        try:
            config = json.loads(config_path.read_text())
            existed = True
        except FileNotFoundError:
            config = {"schema_version": 1, "services": {}, "controllers": {}, "disks": []}
            existed = False
        if config.get("schema_version") != 1 or not isinstance(config.get("disks"), list):
            raise ValueError("应用配置版本不支持重建登记。")
        records = missing_records(config)
        if expected is None:
            return records
        if expected != records or not records:
            raise ValueError("磁盘状态或登记已变化，请重新预览。")
        config["disks"].extend(records)
        with tempfile.NamedTemporaryFile(mode="w", prefix=".registry-", dir=var, delete=False) as f:
            temporary = Path(f.name)
            try:
                if existed:
                    subprocess.run(["cp", "--attributes-only", "--preserve=all",
                                    str(config_path), str(temporary)], check=True)
                json.dump(config, f, indent=2, ensure_ascii=False)
                f.write("\n")
                f.flush()
                os.fsync(f.fileno())
                temporary.replace(config_path)
            finally:
                temporary.unlink(missing_ok=True)
        return records
