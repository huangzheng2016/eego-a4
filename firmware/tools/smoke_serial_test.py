#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""smoke_serial_test.py — 真机串口 smoke 清单（FW6/M19）。

覆盖：启动、显示、SD、WiFi、mock 告警、确认、API 登录状态。

安全边界（强制）：本脚本**绝不刷写设备**——只做 USB Serial/JTAG 读写与局域网 HTTP
GET/POST，不调用 esptool / idf.py / 任何写 flash 的工具。设备变砖风险为零；
唯一副作用是注入一条测试告警并 ack-all 确认（测试数据，可清除）。

前提：
  * 设备已刷入本固件并上电（USB 连接，USB Serial/JTAG 115200，diag 命令通道已启动）
  * pip install pyserial（仓库 requirements.txt 已固定 pyserial==3.5）
  * WiFi 项需设备已完成首次配网；API 项需 --admin-password（或环境变量
    SMOKE_ADMIN_PASSWORD）提供管理密码

用法：
  python3 firmware/tools/smoke_serial_test.py --port /dev/cu.usbmodem1101 \
      [--baud 115200] [--admin-password PW] [--mock-url http://IP:8977/normal] \
      [--timeout 90] [--json]
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
import time
import urllib.error
import urllib.request

try:
    import serial  # pyserial
except ImportError:
    print("error: 需要 pyserial（pip install -r requirements.txt）", file=sys.stderr)
    sys.exit(2)

INJECT_EVENT = {
    "type": "smoke.serial_probe",
    "severity": "warning",
    "title": "smoke serial probe",
    "message": "smoke_serial_test.py injected this event",
    "fields": {"tool": "smoke_serial_test"},
}


class Report:
    def __init__(self) -> None:
        self.rows: list[tuple[str, str, str]] = []

    def ok(self, name: str, detail: str = "") -> None:
        self.rows.append(("PASS", name, detail))

    def warn(self, name: str, detail: str) -> None:
        self.rows.append(("WARN", name, detail))

    def fail(self, name: str, detail: str) -> None:
        self.rows.append(("FAIL", name, detail))

    def check(self, cond: bool, name: str, ok_detail: str, fail_detail: str) -> bool:
        if cond:
            self.ok(name, ok_detail)
        else:
            self.fail(name, fail_detail)
        return cond

    def counts(self) -> tuple[int, int, int]:
        p = sum(1 for r in self.rows if r[0] == "PASS")
        w = sum(1 for r in self.rows if r[0] == "WARN")
        f = sum(1 for r in self.rows if r[0] == "FAIL")
        return p, w, f

    def print(self, as_json: bool = False) -> None:
        if as_json:
            p, w, f = self.counts()
            print(json.dumps({
                "pass": p, "warn": w, "fail": f,
                "rows": [{"level": l, "check": c, "detail": d} for l, c, d in self.rows],
            }, ensure_ascii=False, indent=2))
            return
        for level, name, detail in self.rows:
            line = f"[{level}] {name}"
            if detail:
                line += f" — {detail}"
            print(line)
        p, w, f = self.counts()
        print(f"\nsmoke: pass={p} warn={w} fail={f}")


class Device:
    """USB Serial/JTAG 命令通道：发一行命令，按“静默窗口”收输出。"""

    def __init__(self, port: str, baud: int, quiet_s: float = 0.8) -> None:
        self.ser = serial.Serial()
        self.ser.port = port
        self.ser.baudrate = baud
        self.ser.timeout = 0.2
        self.ser.write_timeout = 5
        self.ser.dtr = False
        self.ser.rts = False
        self.ser.open()
        self.ser.dtr = False
        self.ser.rts = False
        self.quiet_s = quiet_s

    def close(self) -> None:
        self.ser.close()

    def drain(self, seconds: float) -> list[str]:
        """收集 seconds 内的串口输出；静默超过 quiet_s 提前返回（设备早已启动时不空等）。"""
        end = time.monotonic() + seconds
        lines: list[str] = []
        buf = bytearray()
        last_rx = time.monotonic()
        while time.monotonic() < end:
            chunk = self.ser.read(256)
            if chunk:
                buf.extend(chunk)
                last_rx = time.monotonic()
            if buf and b"\n" in buf:
                *complete, buf = buf.split(b"\n")
                for raw in complete:
                    lines.append(raw.decode("utf-8", "replace").rstrip("\r"))
            if (time.monotonic() - last_rx) > self.quiet_s:
                break
        if buf:
            lines.append(buf.decode("utf-8", "replace").rstrip("\r"))
        return lines

    def cmd(self, command: str, deadline_s: float = 15.0) -> list[str]:
        self.ser.reset_input_buffer()
        self.ser.write(command.encode() + b"\n")
        self.ser.flush()
        end = time.monotonic() + deadline_s
        lines: list[str] = []
        last_rx = time.monotonic()
        buf = bytearray()
        while time.monotonic() < end:
            chunk = self.ser.read(256)
            if chunk:
                buf.extend(chunk)
                last_rx = time.monotonic()
                continue
            if buf and b"\n" in buf:
                *complete, buf = buf.split(b"\n")
                for raw in complete:
                    lines.append(raw.decode("utf-8", "replace").rstrip("\r"))
            if lines and (time.monotonic() - last_rx) > self.quiet_s:
                break
        if buf:
            lines.append(buf.decode("utf-8", "replace").rstrip("\r"))
        return lines


# ---------------------------------------------------------------------------
# HTTP（API 登录状态；仅局域网 GET/POST，绝无刷写调用）
# ---------------------------------------------------------------------------

def http_json(url: str, payload: dict | None = None, token: str | None = None,
              timeout: float = 8.0) -> tuple[int, dict | None]:
    data = json.dumps(payload).encode() if payload is not None else None
    req = urllib.request.Request(url, data=data, method="POST" if data else "GET")
    req.add_header("Content-Type", "application/json")
    if token:
        req.add_header("Authorization", f"Bearer {token}")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            body = resp.read()
            try:
                return resp.status, json.loads(body) if body else None
            except json.JSONDecodeError:
                return resp.status, None
    except urllib.error.HTTPError as exc:
        body = exc.read()
        try:
            return exc.code, json.loads(body) if body else None
        except json.JSONDecodeError:
            return exc.code, None
    except (urllib.error.URLError, TimeoutError, OSError) as exc:
        return -1, {"error": {"message": str(exc)}}


# ---------------------------------------------------------------------------
# 各检查阶段
# ---------------------------------------------------------------------------

def phase_boot(dev: Device, rep: Report, timeout_s: float) -> None:
    print("… 等待 diag 启动横幅（可手动按一下复位键）", flush=True)
    lines = dev.drain(timeout_s)
    banner = [l for l in lines if "serial diagnostics ready" in l or "commands:" in l]
    if banner:
        rep.ok("boot", "diag 串口通道已启动")
        return
    # 兜底：直接发 version 探测（设备可能早已启动完）
    resp = dev.cmd("version", deadline_s=8)
    if any(l.startswith("version:") for l in resp):
        rep.ok("boot", "设备已启动（version 应答正常）")
    else:
        rep.fail("boot", "未看到 diag 横幅且 version 无应答；确认固件已刷入、"
                         "串口参数 115200、设备已上电")


def phase_version(dev: Device, rep: Report) -> None:
    resp = dev.cmd("version")
    ok = (any(l.startswith("version: eego-a4-alert-panel") for l in resp)
          and any(l.startswith("chip: ") for l in resp)
          and any(l.startswith("framebuffer: 552x768 logical, 52992 bytes BW") for l in resp))
    detail = next((l for l in resp if l.startswith("version:")), "")
    rep.check(ok, "version", detail or "version/chip/framebuffer 行齐全",
              "version 输出缺行（预期 eego-a4-alert-panel + chip + 552x768 logical）")


def phase_display_smoke(dev: Device, rep: Report) -> None:
    resp = dev.cmd("smoke", deadline_s=60)  # 全量刷新耗时数秒
    pass_lines = [l for l in resp if l.startswith("[PASS]")]
    fail_lines = [l for l in resp if l.startswith("[FAIL]")]
    summary = next((l for l in resp if l.startswith("smoke: pass=")), "")
    m = re.search(r"pass=(\d+) warn=(\d+) fail=(\d+)", summary)
    fail_n = int(m.group(3)) if m else -1
    rep.check(bool(m) and fail_n == 0, "selfcheck",
              summary or "内建 smoke 全过",
              f"内建 smoke 有 FAIL 行: {fail_lines[:3] or summary or '无汇总'}")
    rep.check(any("display-gray" in l for l in pass_lines), "display",
              "4 级灰条已绘制（全量刷新）",
              "display-gray 未通过（e-paper 需目视确认灰条）")
    rep.warn("display-visual", "灰条/文字需目视确认（工具无法代替人眼）"
             if any("display-gray" in l for l in pass_lines) else "显示自检未过，先查供电/排线")


def phase_sd(dev: Device, rep: Report) -> None:
    resp = dev.cmd("sd")
    line = next((l for l in resp if l.startswith("sd: ready=")), "")
    rep.check("ready=1" in line, "sd", line or "sd 无应答",
              f"SD 未就绪（{line or '无应答'}）；检查卡/挂载，配置目录 /alertpanel 无法使用")


def phase_wifi(dev: Device, rep: Report) -> str | None:
    resp = dev.cmd("net", deadline_s=20)
    head = next((l for l in resp if l.startswith("net: running=")), "")
    m = re.search(r"wifi_state=(\d+) ssid=(\S*) rssi=(-?\d+) ip=([\d.]+)", head)
    if not m:
        rep.fail("wifi", f"无法解析 net 输出（{head or '无应答'}）")
        return None
    state, ssid, _rssi, ip = m.groups()
    if state == "2":
        rep.ok("wifi", f"已连接 ssid={ssid} ip={ip}")
        return ip if ip != "0.0.0.0" else None
    rep.fail("wifi", f"wifi_state={state}（2=Connected 才继续 API 项）；先完成首次配网")
    return None


def phase_mock_alert(dev: Device, rep: Report, mock_url: str | None) -> None:
    # 1) 串口注入路径（不依赖外部 mock 服务）
    resp = dev.cmd("inject " + json.dumps(INJECT_EVENT, ensure_ascii=False))
    queued = any("inject: queued 1 event(s) -> ingested" in l for l in resp)
    rep.check(queued, "inject", "串口注入已入队 net_task",
              f"注入未受理: {next((l for l in resp if l.startswith(('inject:', 'ERR'))), '无应答')}")
    found = False
    if queued:
        end = time.monotonic() + 12
        while time.monotonic() < end and not found:
            alerts = dev.cmd("alerts", deadline_s=10)
            found = any(INJECT_EVENT["title"] in l for l in alerts)
            if not found:
                time.sleep(1.5)
    rep.check(found, "alert-visible", "快照中可见注入的告警",
              "12s 内快照未出现注入告警（net_task 摄入链路异常）")

    # 2) mock 轮询路径（可选；需设备已配置指向 mock_alert_server 的源）
    if not mock_url:
        rep.warn("mock-poll", "未给 --mock-url，跳过 http_poll 轮询联调")
        return
    type_prefix = "mock."
    end = time.monotonic() + 45
    seen = False
    while time.monotonic() < end and not seen:
        alerts = dev.cmd("alerts", deadline_s=10)
        seen = any(type_prefix in l or "Mock" in l for l in alerts)
        if not seen:
            time.sleep(3)
    rep.check(seen, "mock-poll",
              f"轮询源告警已入库（{mock_url}）",
              f"45s 内未见 mock 源告警；确认设备 sources[] 已配置 {mock_url} 且间隔 ≤15s")


def phase_ack(dev: Device, rep: Report) -> None:
    resp = dev.cmd("ack-all", deadline_s=20)
    line = next((l for l in resp if l.startswith("ack-all:")), "")
    m = re.search(r"code=(\w+) affected=(\d+)", line)
    ok = bool(m) and m.group(1) == "ok" and int(m.group(2)) >= 1
    rep.check(ok, "ack", line or "ack-all 无应答",
              f"确认异常（{line or '无应答'}）；预期 code=ok affected>=1")


def phase_api(rep: Report, ip: str | None, password: str | None, api_base: str | None) -> None:
    if not ip and not api_base:
        rep.warn("api", "无设备 IP，跳过 API 登录状态检查")
        return
    if not password:
        rep.warn("api", "未提供管理密码（--admin-password 或 SMOKE_ADMIN_PASSWORD），跳过")
        return
    base = (api_base or f"http://{ip}/api/v1").rstrip("/")

    # 负例：无令牌 → 401 且错误封装同形（M13：不区分无令牌/错令牌）
    code, body = http_json(f"{base}/status")
    same_shape = (isinstance(body, dict) and isinstance(body.get("error"), dict)
                  and body["error"].get("code") == "unauthorized")
    rep.check(code == 401 and same_shape, "api-guard",
              f"无令牌访问 status → {code} unauthorized 同形",
              f"无令牌应 401 unauthorized，实际 {code} body={body}")

    # 登录
    code, body = http_json(f"{base}/auth/login", {"password": password})
    token = body.get("token") if isinstance(body, dict) else None
    if not (code == 200 and token):
        rep.fail("api-login", f"登录失败 code={code}（确认管理密码）")
        return
    rep.ok("api-login", f"登录成功（expires_in={body.get('expires_in')}s）")

    # 登录后 status：provisioned=true 且 mode 在线
    code, body = http_json(f"{base}/status", token=token)
    sec = (body or {}).get("security", {}) if isinstance(body, dict) else {}
    mode = ((body or {}).get("device", {}) or {}).get("mode", "")
    rep.check(code == 200 and sec.get("provisioned") is True, "api-status",
              f"status 200，provisioned=true，mode={mode}",
              f"status 异常 code={code} provisioned={sec.get('provisioned')}")

    # 登出（清理会话）
    code, _ = http_json(f"{base}/auth/logout", {}, token=token)
    rep.check(code in (200, 204), "api-logout", f"登出 {code}",
              f"登出异常 code={code}")


# ---------------------------------------------------------------------------
# 主流程
# ---------------------------------------------------------------------------

def main() -> int:
    ap = argparse.ArgumentParser(description="真机串口 smoke（启动/显示/SD/WiFi/mock 告警/确认/API 登录）；绝不刷写设备")
    ap.add_argument("--port", required=True, help="串口设备（如 /dev/cu.usbmodem1101 或 COM3）")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--admin-password", default=os.environ.get("SMOKE_ADMIN_PASSWORD"),
                    help="管理密码（默认读环境变量 SMOKE_ADMIN_PASSWORD）")
    ap.add_argument("--api-base", default=None,
                    help="管理 API 基址（默认 http://<设备IP>/api/v1；台架可用 http://127.0.0.1:端口/api/v1）")
    ap.add_argument("--mock-url", default=None,
                    help="mock_alert_server 场景 URL（如 http://192.168.1.10:8977/normal）")
    ap.add_argument("--timeout", type=float, default=90.0, help="启动横幅等待秒数（默认 90）")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()

    rep = Report()
    dev = Device(args.port, args.baud)
    try:
        phase_boot(dev, rep, args.timeout)
        phase_version(dev, rep)
        phase_display_smoke(dev, rep)
        phase_sd(dev, rep)
        ip = phase_wifi(dev, rep)
        phase_mock_alert(dev, rep, args.mock_url)
        phase_ack(dev, rep)
        phase_api(rep, ip, args.admin_password, args.api_base)
    except serial.SerialException as exc:
        rep.fail("serial", f"串口异常: {exc}")
    finally:
        dev.close()

    rep.print(args.json)
    _, _, f = rep.counts()
    return 1 if f else 0


if __name__ == "__main__":
    sys.exit(main())
