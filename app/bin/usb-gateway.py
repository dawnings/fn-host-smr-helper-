#!/usr/bin/env python3
"""Serve the USB scan and forward the existing UI/API to the SATA backend."""

import argparse
import hmac
import http.client
from http.server import BaseHTTPRequestHandler
import json
import os
from pathlib import Path
import signal
import socket
import socketserver
import stat
import subprocess
import time
from threading import RLock
from urllib.parse import urlsplit

from scan import scan
from native_registry import rebuild
from zoned_status import mappings, reclaim
from port_identify import identify, ports


HOP_HEADERS = {
    "connection", "keep-alive", "proxy-authenticate", "proxy-authorization",
    "te", "trailer", "transfer-encoding", "upgrade", "proxy-connection",
}
ZONED_ROUTES = {"/api/zoned-status", "/app/smr-manager/api/zoned-status"}
REGISTRY_ROUTES = {"/api/native-registry", "/app/smr-manager/api/native-registry"}
ASSOCIATION_ROUTES = {"/api/association-enable", "/app/smr-manager/api/association-enable"}
PORT_ROUTES = {"/api/port-identify", "/app/smr-manager/api/port-identify"}
USB_ROUTES = {"/api/usb-scan", "/app/smr-manager/api/usb-scan"}


class UnixHTTPConnection(http.client.HTTPConnection):
    def __init__(self, path, timeout=30):
        super().__init__("localhost", timeout=timeout)
        self.path = path

    def connect(self):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(self.timeout)
        self.sock.connect(str(self.path))


class Gateway(socketserver.ThreadingMixIn, socketserver.UnixStreamServer):
    daemon_threads = True

    def __init__(self, gateway, upstream, sysfs, root, var, backend):
        self.upstream = upstream
        self.sysfs = sysfs
        self.root = root
        self.var = var
        self.backend = backend
        super().__init__(str(gateway), Handler)


class Handler(BaseHTTPRequestHandler):
    def setup(self):
        self.request.settimeout(30)
        super().setup()

    def address_string(self):
        return "fnOS gateway"

    def log_request(self, code="-", size="-"):
        pass

    def do_GET(self):
        self.dispatch()

    def do_HEAD(self):
        self.dispatch()

    def do_POST(self):
        self.dispatch()

    def dispatch(self):
        path = urlsplit(self.path).path
        if path in ASSOCIATION_ROUTES:
            self.enable_association(path)
            return
        if path in PORT_ROUTES | REGISTRY_ROUTES | ZONED_ROUTES:
            self.port_identify(path)
            return
        if path in USB_ROUTES:
            if self.command not in {"GET", "HEAD"}:
                self.send_error(405)
                return
            try:
                result = scan(self.server.sysfs)
                status = 200
            except (OSError, ValueError) as error:
                result = {"error": str(error)}
                status = 503
            body = json.dumps(result, ensure_ascii=False).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            if self.command == "GET":
                self.wfile.write(body)
            return
        with self.server.backend.lock:
            self.forward()

    def enable_association(self, path):
        if self.command != "POST":
            self.send_error(405)
            return
        if (self.headers.get("Transfer-Encoding") or
                self.headers.get_all("Content-Length", []) != ["0"]):
            self.send_error(400)
            return
        try:
            with self.server.backend.lock:
                state = self.backend_state(path)
                token = state.get("csrf")
                supplied = self.headers.get("X-CSRF-Token", "")
                if not isinstance(token, str) or not token or not hmac.compare_digest(
                        token.encode(), supplied.encode()):
                    raise PermissionError("Invalid CSRF token")
                if state.get("association_supported") is False:
                    raise ValueError("当前飞牛版本不支持硬盘归属。")
                if (state.get("job") or {}).get("status") == "running":
                    raise ValueError("有磁盘任务正在运行，请等待完成。")
                marker = self.server.var / "association.enabled"
                if marker.exists():
                    raise ValueError("硬盘归属已开启，请刷新服务状态。")
                self.start_association_job(path, supplied)
                deadline = time.monotonic() + 20
                while True:
                    job = self.backend_state(path).get("job") or {}
                    if job.get("status") == "failed":
                        raise ValueError(job.get("output") or "硬盘归属开启失败。")
                    if job.get("status") == "done" and marker.is_file():
                        break
                    if time.monotonic() >= deadline:
                        raise ValueError("未能确认硬盘归属已开启，未安排重启。请刷新服务状态。")
                    time.sleep(0.25)
                result = subprocess.run([
                    "systemd-run", "--unit=smr-manager-restart-trim", "--collect",
                    "--on-active=3s", "/usr/bin/systemctl", "restart", "trim_main.service",
                ], check=False, capture_output=True, text=True, timeout=5)
                if result.returncode:
                    print(result.stderr.strip(), flush=True)
                    raise ValueError("硬盘归属已开启，但未能安排重启 trim_main。")
            status, data = 202, {"restart_queued": True}
        except PermissionError:
            self.send_error(403)
            return
        except (OSError, http.client.HTTPException, subprocess.SubprocessError, ValueError) as error:
            status, data = 409, {"error": str(error)}
        body = json.dumps(data, ensure_ascii=False).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def start_association_job(self, path, token):
        headers = {key: value for key, value in self.headers.items()
                   if key.lower() not in HOP_HEADERS | {"content-length", "x-csrf-token", "content-type"}}
        headers.update({"X-CSRF-Token": token, "Content-Type": "application/json"})
        body = json.dumps({"action": "service", "feature": "association", "enabled": True})
        connection = UnixHTTPConnection(self.server.upstream)
        try:
            connection.request("POST", path.rsplit("/", 1)[0] + "/action", body, headers)
            response = connection.getresponse()
            response.read()
            if not 200 <= response.status < 300:
                raise ValueError(f"后端拒绝开启硬盘归属（HTTP {response.status}）。")
        finally:
            connection.close()

    def port_identify(self, path):
        if self.command not in {"GET", "POST"}:
            self.send_error(405)
            return
        try:
            with self.server.backend.lock:
                state = self.backend_state(path)
            token = state.get("csrf")
        except PermissionError:
            self.send_error(403)
            return
        except (OSError, ValueError, http.client.HTTPException):
            self.send_error(502, "SATA backend unavailable")
            return
        if not isinstance(token, str) or not token:
            self.send_error(403)
            return

        status = 200
        try:
            if self.command == "GET":
                if path in ZONED_ROUTES:
                    data = {"disks": mappings(self.server.var)}
                elif path in REGISTRY_ROUTES:
                    data = {"records": rebuild(self.server.var)}
                else:
                    data = {"ports": ports(self.server.sysfs)}
            else:
                supplied = self.headers.get("X-CSRF-Token", "")
                if not hmac.compare_digest(token.encode(), supplied.encode()):
                    self.send_error(403)
                    return
                lengths = self.headers.get_all("Content-Length", [])
                if self.headers.get("Transfer-Encoding") or len(lengths) != 1:
                    raise ValueError("请求长度无效。")
                length = int(lengths[0])
                if not 0 < length <= (65536 if path in REGISTRY_ROUTES else 1024):
                    raise ValueError("请求长度无效。")
                payload = json.loads(self.rfile.read(length))
                if not isinstance(payload, dict):
                    raise ValueError("请选择 SATA 端口。")
                if path in ZONED_ROUTES:
                    data = {"mapping": reclaim(self.server.var, payload.get("serial"))}
                elif path in REGISTRY_ROUTES:
                    expected = payload.get("records")
                    if not isinstance(expected, list):
                        raise ValueError("请先预览待重建的登记。")
                    with self.server.backend.lock:
                        state = self.backend_state(path)
                        fresh_token = state.get("csrf")
                        if not isinstance(fresh_token, str) or not hmac.compare_digest(
                                fresh_token.encode(), supplied.encode()):
                            self.send_error(403)
                            return
                        if (state.get("job") or {}).get("status") == "running":
                            raise ValueError("有磁盘任务正在运行，请等待完成。")
                        records = rebuild(self.server.var, expected)
                        try:
                            self.server.backend.reload()
                            saved = (self.backend_state(path).get("config") or {}).get("disks", [])
                            if not all(record in saved for record in records):
                                raise ValueError("后端尚未读到全部登记")
                        except (RuntimeError, OSError, ValueError, http.client.HTTPException) as error:
                            raise ValueError(f"登记已保存，但后端重载验证失败：{error}") from error
                        data = {"records": records}
                else:
                    data = identify(self.server.root, self.server.var,
                                    self.server.sysfs, payload.get("port"))
        except (OSError, ValueError, subprocess.SubprocessError) as error:
            print(f"Port identification: {error}", flush=True)
            status, data = 409, {"error": str(error)}
        body = json.dumps(data, ensure_ascii=False).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def backend_state(self, path):
        connection = UnixHTTPConnection(self.server.upstream)
        try:
            headers = {key: value for key, value in self.headers.items()
                       if key.lower() not in HOP_HEADERS | {"content-length", "x-csrf-token"}}
            connection.request("GET", path.rsplit("/", 1)[0] + "/state", headers=headers)
            response = connection.getresponse()
            if response.status != 200:
                raise PermissionError("Original backend rejected the request")
            return json.loads(response.read())
        finally:
            connection.close()

    def forward(self):
        if self.headers.get("Transfer-Encoding") or len(self.headers.get_all("Content-Length", [])) > 1:
            self.send_error(400)
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            self.send_error(400)
            return
        if not 0 <= length <= 65536:
            self.send_error(413)
            return

        body = self.rfile.read(length)
        if len(body) != length:
            self.send_error(400)
            return

        if self.command == "POST" and urlsplit(self.path).path in {
                "/api/action", "/app/smr-manager/api/action"}:
            try:
                action = json.loads(body)
            except (ValueError, UnicodeError):
                action = None
            if (isinstance(action, dict) and action.get("action") == "service" and
                    action.get("feature") == "association" and action.get("enabled") is True):
                self.send_error(409, "Use the confirmed association enable action")
                return

        connection = UnixHTTPConnection(self.server.upstream)
        try:
            try:
                request_skip = HOP_HEADERS | {
                    name.strip().lower()
                    for name in self.headers.get("Connection", "").split(",")
                }
                connection.putrequest(self.command, self.path, skip_host=True, skip_accept_encoding=True)
                for key, value in self.headers.items():
                    if key.lower() not in request_skip:
                        connection.putheader(key, value)
                connection.endheaders(body)
                response = connection.getresponse()
                data = response.read()
            except (OSError, http.client.HTTPException):
                self.send_error(502, "SATA backend unavailable")
                return

            response_skip = HOP_HEADERS | {"content-length", "server", "date"} | {
                name.strip().lower()
                for name in response.getheader("Connection", "").split(",")
            }
            self.send_response(response.status)
            for key, value in response.getheaders():
                if key.lower() not in response_skip:
                    self.send_header(key, value)
            if self.command == "HEAD":
                reported = response.getheader("Content-Length")
                if reported is not None:
                    self.send_header("Content-Length", reported)
            elif response.status not in {204, 304}:
                self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            if self.command != "HEAD":
                self.wfile.write(data)
        finally:
            connection.close()


def wait_for_upstream(child, upstream, stopping):
    deadline = time.monotonic() + 20
    while not stopping() and child.poll() is None and time.monotonic() < deadline:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as probe:
            probe.settimeout(0.2)
            try:
                probe.connect(str(upstream))
                return True
            except OSError:
                time.sleep(0.1)
    if stopping():
        return False
    raise RuntimeError("SATA backend did not open its socket")


class Backend:
    def __init__(self, command, upstream, stopping):
        self.command = command
        self.upstream = upstream
        self.stopping = stopping
        self.lock = RLock()
        self.child = None

    def start(self):
        self.child = subprocess.Popen(self.command)
        return wait_for_upstream(self.child, self.upstream, self.stopping)

    def stop(self):
        if self.child is not None and self.child.poll() is None:
            self.child.terminate()
            try:
                self.child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.child.kill()
                self.child.wait()

    def reload(self):
        self.stop()
        self.upstream.unlink(missing_ok=True)
        if not self.start():
            raise RuntimeError("Application is stopping")

    def running(self):
        with self.lock:
            return self.child is not None and self.child.poll() is None


def queue_startup_identify(command):
    # Keep identification independent of the web process, including on app stop.
    try:
        result = subprocess.run([
            "systemd-run", "--unit=smr-manager-startup-identify",
            "--collect", "--no-block",
            "--property=After=smr-manager-identify.service",
            "--property=TimeoutStartSec=300",
            *command, "--task", "identify",
        ], timeout=5, check=False)
        if result.returncode:
            print(f"Could not queue startup identification (exit {result.returncode})",
                  flush=True)
    except (OSError, subprocess.TimeoutExpired) as error:
        print(f"Could not queue startup identification: {error}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--var", type=Path, required=True)
    parser.add_argument("--sysfs", type=Path, default=Path("/sys"))
    args = parser.parse_args()
    upstream = args.root / "app.sock"
    gateway = args.root / "usb-gateway.sock"
    stopping = False

    def stop(*_):
        nonlocal stopping
        stopping = True

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    command = [
        str(args.root / "bin/smr-manager"), "--root", str(args.root), "--var", str(args.var)
    ]
    backend = Backend(command, upstream, lambda: stopping)
    try:
        if not backend.start():
            return
        with Gateway(gateway, upstream, args.sysfs, args.root, args.var, backend) as server:
            try:
                gateway.chmod(stat.S_IMODE(upstream.stat().st_mode))
                if not stopping and os.environ.get("SMR_SKIP_STARTUP_IDENTIFY") != "1":
                    queue_startup_identify(command)
                server.timeout = 0.25
                while not stopping and backend.running():
                    server.handle_request()
                if not stopping:
                    raise RuntimeError(f"SATA backend exited with status {backend.child.returncode}")
            finally:
                gateway.unlink(missing_ok=True)
    finally:
        with backend.lock:
            backend.stop()


if __name__ == "__main__":
    main()
