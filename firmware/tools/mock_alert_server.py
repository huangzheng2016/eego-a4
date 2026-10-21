#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""mock_alert_server.py — 主机侧 mock 告警源（FW6/M19），供设备 http_poll 轮询联调。

纯标准库。按 alert_core AlertRecord/AlertEvent 契约（M11 §4）提供 8 组场景：

  /normal     正常：单条 warning 事件（恒等 field_map 即可解析）
  /batch      批量：{"data":{"alerts":[...]}} 5 条事件（需 array_path="data.alerts"）
  /dup        重复：每次轮询同一 fingerprint 事件（type/title 不变，message 轮换）
              → 设备 occurrences 递增
  /clear      清除：前 K 次轮询给 1 条活动事件，之后给空数组
              → 设备 clear_after_misses（默认 3）达标后自动清除
  /ttl        TTL：携带 "ttl_s":5 的事件 → 设备到期自动 expired
  /oversize   超大：单事件 >16KB（message 17000 字符）→ 设备按预算跳过并计数
  /badlen     错误 Content-Length：声明长度 > 实际 body 后主动断连
              → 设备应记传输错误（consecutive_failures），不解析
  /slow       延迟：--delay-ms（默认 3000）后返回正常事件
              → 设大于设备 timeout_ms 可验证超时路径

辅助端点：/_log（请求日志 JSON）、/_reset（清计数器）、/（场景索引 + 建议 SourceConfig）。

安全边界：默认只绑 127.0.0.1；设备联调时 --host 0.0.0.0 暴露到局域网。
本服务只发 mock 数据，不接收/存储任何凭据；请求日志只记方法/路径/计数。

用法：
  python3 firmware/tools/mock_alert_server.py --host 0.0.0.0 --port 8977
  # 浏览器/设备管理页把 sources[] 指向 http://<主机IP>:8977/normal 等端点。
"""

from __future__ import annotations

import argparse
import json
import socket
import sys
import threading
import time
from collections import defaultdict
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

SCENARIOS = ("normal", "batch", "dup", "clear", "ttl", "oversize", "badlen", "slow")

_lock = threading.Lock()
_poll_count: dict[str, int] = defaultdict(int)   # path -> 轮询次数
_req_log: list[dict] = []                       # 最近 200 条请求


def _event(type_id: str, severity: str, title: str, message: str,
           fields: dict | None = None, ttl_s: int | None = None,
           state: str | None = None) -> dict:
    ev: dict = {"type": type_id, "severity": severity, "title": title,
                "message": message, "fields": fields or {}}
    if ttl_s is not None:
        ev["ttl_s"] = ttl_s
    if state is not None:
        ev["state"] = state
    return ev


def _normal_body() -> dict:
    return _event("mock.disk_almost_full", "warning", "Mock 磁盘将满",
                  "联调正常路径：/alertpanel 用量 87%", {"host": "mock-host", "used_pct": 87})


def _batch_body() -> dict:
    alerts = [
        _event("mock.cpu_high", "critical", "Mock CPU 过高", "cpu 95% 持续 5m",
               {"host": "mock-host", "cpu_pct": 95}),
        _event("mock.mem_high", "warning", "Mock 内存偏高", "mem 82%",
               {"host": "mock-host", "mem_pct": 82}),
        _event("mock.backup_done", "info", "Mock 备份完成", "夜间备份成功",
               {"host": "mock-host"}),
        _event("mock.cert_expiring", "warning", "Mock 证书将过期", "TLS 证书 7 天后到期",
               {"host": "mock-host", "days_left": 7}),
        _event("mock.service_down", "critical", "Mock 服务离线", "nginx 未响应探测",
               {"host": "mock-host", "service": "nginx"}),
    ]
    return {"data": {"alerts": alerts}}


def _dup_body(n: int) -> dict:
    # type/title 不变 → 内置 dedup（source.id+type+title）fingerprint 稳定；
    # message 带轮询序号 → occurrences 递增可在设备 `alerts` 输出确认。
    return _event("mock.heartbeat_dup", "info", "Mock 重复心跳",
                  f"第 {n} 次轮询同一指纹事件", {"host": "mock-host", "poll": n})


def _clear_body(poll_n: int, clear_after: int) -> list:
    if poll_n <= clear_after:
        return [_event("mock.clearable", "warning", "Mock 可清除告警",
                       "前 %d 轮出现，之后缺席 → clear_after_misses 自动清除"
                       % clear_after, {"host": "mock-host"})]
    return []  # 缺席 → 设备 miss 记账


def _ttl_body() -> dict:
    return _event("mock.ttl_probe", "info", "Mock TTL 探针",
                  "ttl_s=5，设备应在到期后自动 expired", {"host": "mock-host"}, ttl_s=5)


def _oversize_body() -> dict:
    # 单事件 JSON > 16KB（kMaxEventJsonBytes）→ 设备跳过并 events_skipped 计数
    return _event("mock.oversize", "warning", "Mock 超大事件",
                  "x" * 17000, {"host": "mock-host"})


class Handler(BaseHTTPRequestHandler):
    server_version = "MockAlert/1.0"
    protocol_version = "HTTP/1.1"

    # -- 场景配置（由 CLI 注入）--
    delay_ms = 3000
    clear_after = 3

    def log_message(self, fmt, *args):  # 静默默认 stderr 日志，走 _req_log
        pass

    # ------------------------------------------------------------------
    def _record(self, path: str) -> int:
        with _lock:
            _poll_count[path] += 1
            _req_log.append({
                "t": time.strftime("%H:%M:%S"),
                "client": self.client_address[0],
                "method": self.command,
                "path": path,
                "poll": _poll_count[path],
            })
            if len(_req_log) > 200:
                del _req_log[: len(_req_log) - 200]
            return _poll_count[path]

    def _send_json(self, payload, status: int = 200) -> None:
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    # ------------------------------------------------------------------
    def do_GET(self):  # noqa: N802
        path = self.path.split("?", 1)[0].strip("/")
        n = self._record(path)

        if path == "":
            return self._index()
        if path == "_log":
            with _lock:
                return self._send_json({"poll_count": dict(_poll_count), "requests": _req_log})
        if path == "_reset":
            with _lock:
                _poll_count.clear()
                _req_log.clear()
            return self._send_json({"ok": True})
        if path == "normal":
            return self._send_json(_normal_body())
        if path == "batch":
            return self._send_json(_batch_body())
        if path == "dup":
            return self._send_json(_dup_body(n))
        if path == "clear":
            return self._send_json(_clear_body(n, self.clear_after))
        if path == "ttl":
            return self._send_json(_ttl_body())
        if path == "oversize":
            return self._send_json(_oversize_body())
        if path == "slow":
            time.sleep(self.delay_ms / 1000.0)
            return self._send_json(_normal_body())
        if path == "badlen":
            return self._badlen()
        self._send_json({"error": "unknown scenario", "scenarios": list(SCENARIOS)}, 404)

    # ------------------------------------------------------------------
    def _badlen(self) -> None:
        # 错误 Content-Length：声明比实际多 64 字节，写完 body 直接断连。
        # 设备 http_pool 应在读 body 时发现短读 → 传输错误，不进 JSON 解析。
        body = json.dumps(_normal_body(), ensure_ascii=False).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body) + 64))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)
        self.wfile.flush()
        self.connection.close()

    def _suggested_source(self, scenario: str, array_path: str = "") -> dict:
        host = self.headers.get("Host", "127.0.0.1")
        return {
            "id": f"mock-{scenario}", "kind": "http_poll", "enabled": True,
            "url": f"http://{host}/{scenario}", "interval_ms": 10000,
            "clear_after_misses": 3, "array_path": array_path,
            "field_map": {}, "severity_map": {}, "dedup_key": [],
            "default_type": "generic.any", "timeout_ms": 20000,
            "max_body_bytes": 65536,
        }

    def _index(self) -> None:
        payload = {
            "service": "mock_alert_server",
            "scenarios": {
                "normal": "单条 warning 事件（恒等映射）",
                "batch": "5 条事件，array_path=data.alerts",
                "dup": "同一 fingerprint 重复（occurrences 递增）",
                "clear": "前 K 轮在场后缺席（clear_after_misses 自动清除）",
                "ttl": "ttl_s=5（到期 expired）",
                "oversize": "单事件 >16KB（预算跳过计数）",
                "badlen": "错误 Content-Length（传输错误）",
                "slow": f"延迟 {self.delay_ms}ms 响应（可验超时）",
            },
            "suggested_sources": {
                "normal": self._suggested_source("normal"),
                "batch": self._suggested_source("batch", "data.alerts"),
                "dup": self._suggested_source("dup"),
                "clear": self._suggested_source("clear"),
                "ttl": self._suggested_source("ttl"),
                "oversize": self._suggested_source("oversize"),
                "slow": self._suggested_source("slow"),
            },
            "control": {"_log": "请求日志", "_reset": "清计数器"},
        }
        self._send_json(payload)


def main() -> int:
    ap = argparse.ArgumentParser(description="mock 告警源（8 场景 + 请求日志），供 http_poll 联调")
    ap.add_argument("--host", default="127.0.0.1",
                    help="监听地址（设备联调用 0.0.0.0；默认仅本机）")
    ap.add_argument("--port", type=int, default=8977)
    ap.add_argument("--delay-ms", type=int, default=3000, help="/slow 延迟（默认 3000）")
    ap.add_argument("--clear-after", type=int, default=3,
                    help="/clear 前 K 轮给活动事件（默认 3，对齐设备 clear_after_misses）")
    args = ap.parse_args()

    Handler.delay_ms = args.delay_ms
    Handler.clear_after = args.clear_after

    srv = ThreadingHTTPServer((args.host, args.port), Handler)
    lan_ip = "127.0.0.1"
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("192.0.2.1", 80))  # TEST-NET，仅取本机路由源地址
        lan_ip = s.getsockname()[0]
        s.close()
    except OSError:
        pass
    print(f"mock_alert_server: http://{args.host}:{args.port}/  (场景索引)")
    print(f"设备可用地址示例: http://{lan_ip}:{args.port}/normal")
    print(f"  /slow 延迟 {args.delay_ms}ms；/clear 前 {args.clear_after} 轮在场；/_log 看轮询记录")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        srv.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
