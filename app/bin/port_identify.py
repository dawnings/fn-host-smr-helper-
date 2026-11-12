"""Run one evidence-gated Host-SMR compatibility probe on a selected AHCI port."""

import fcntl
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


PCI = re.compile(r"[0-9a-f]{4}:[0-9a-f]{2}:[0-9a-f]{2}\.[0-7]")


def ports(sysfs):
    devices = list((sysfs / "class/scsi_device").glob("*"))
    result = []
    for host in sorted((sysfs / "class/scsi_host").glob("host*")):
        try:
            if (host / "proc_name").read_text().strip() != "ahci":
                continue
            parts = host.resolve(strict=True).parts
            ata = next(part for part in reversed(parts) if re.fullmatch(r"ata[0-9]+", part))
            controller = next(part for part in reversed(parts) if PCI.fullmatch(part))
            index = int((sysfs / "class/ata_port" / ata / "port_no").read_text()) - 1
            number = int(host.name[4:])
        except (OSError, StopIteration, ValueError):
            continue
        if index >= 0:
            result.append({"id": f"{controller}/{ata}", "ata": ata, "controller": controller,
                           "index": index, "host": number,
                           "occupied": any(d.name.startswith(f"{number}:") for d in devices)})
    return result


def identify(root, var, sysfs, port_id):
    if not isinstance(port_id, str):
        raise ValueError("请选择 SATA 端口。")
    compat = root / "compat"
    source = (compat / "smr_sata.c").read_bytes()
    release = os.uname().release
    loaded = sysfs / "module/smr_sata"

    with open("/run/smr-manager.lock", "a") as lock, tempfile.TemporaryDirectory(dir=var) as tmp:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise ValueError("另一个磁盘任务正在运行，请等待完成。") from error
        port = next((item for item in ports(sysfs) if item["id"] == port_id), None)
        if port is None or port["occupied"] or loaded.exists():
            raise ValueError("端口已枚举设备、端口已变化，或兼容模块正被占用。")

        backend = [str(root / "bin/smr-manager"), "--root", str(root), "--var", str(var)]
        abi = json.loads(subprocess.check_output(backend + ["--check-kprobe-abi"], timeout=10))
        fis = abi.get("fis", {})
        if not abi.get("ok") or not fis.get("ok") or fis.get("source") != "libahci":
            raise ValueError("当前内核的 AHCI 探针校验未通过。")

        def matches(module):
            vermagic = subprocess.run(["modinfo", "-F", "vermagic", str(module)],
                                      capture_output=True, text=True, timeout=5)
            return vermagic.returncode == 0 and vermagic.stdout.split()[:1] == [release]

        module = next((m for m in (var / "modules").rglob("smr_sata.ko")
                       if (m.parent / "smr_sata.c").is_file()
                       and (m.parent / "smr_sata.c").read_bytes() == source and matches(m)), None)
        if module is None:
            build = Path(tmp)
            (build / "Makefile").write_text("obj-m += smr_sata.o\n")
            (build / "smr_sata.c").write_bytes(source)
            cc = shutil.which("gcc") or shutil.which("gcc-12")
            if not cc:
                raise ValueError("缺少 gcc，无法构建 SATA 兼容模块。")
            subprocess.run(["make", "-C", f"/lib/modules/{release}/build",
                            f"M={build}", f"CC={cc}", "modules"],
                           check=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=90)
            module = build / "smr_sata.ko"
            if not matches(module):
                raise ValueError("SATA 兼容模块与当前内核版本不匹配。")

        result = subprocess.run([
            "insmod", str(module), f"controller={port['controller']}",
            f"host_no={port['host']}", f"target_port={port['index']}",
            f"rx_fis_offset={int(fis['offset'])}", "expected_proto_ok=1",
            "ahci_fis=1", "observe_only=0", "normal_retry=0",
        ], capture_output=True, text=True)
        if result.returncode:
            raise ValueError(f"兼容模块无法探测该端口：{result.stderr.strip()}")
        try:
            params = loaded / "parameters"
            evidence = (params / "evidence").read_text().strip() in {"Y", "1"}
            subprocess.run(["udevadm", "settle", "--timeout=8"], check=False)
            disks = []
            for disk in (sysfs / "class/block").glob("*"):
                if port["ata"] in disk.resolve().parts and (disk / "queue/zoned").is_file():
                    if (disk / "queue/zoned").read_text().strip() == "host-managed":
                        disks.append(disk.name)
            return {"evidence": evidence, "disks": sorted(disks), "port": port_id}
        finally:
            subprocess.run(["rmmod", "smr_sata"], check=True)
