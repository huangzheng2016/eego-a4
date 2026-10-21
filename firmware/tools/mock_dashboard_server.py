#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generic, credential-free HTTP mock for EEGO dashboard presets and examples."""

from __future__ import annotations

import argparse
import binascii
import hashlib
import json
import math
import socket
import struct
import sys
import threading
import time
import urllib.request
import zlib
from collections import defaultdict
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

_lock = threading.Lock()
_poll_count = defaultdict(int)
_request_log = []


def now_ms():
    return time.time_ns() // 1_000_000


def metric(metric_name, value, unit="", timestamp_ms=0, **labels):
    return {"metric": metric_name, "value": value, "unit": unit, "timestamp_ms": timestamp_ms,
            "labels": {key: str(label_value) for key, label_value in labels.items()}}


def event(event_id, severity, title, message, timestamp_ms, state="active"):
    return {"id": event_id, "rule_id": "", "timestamp_ms": timestamp_ms, "severity": severity,
            "state": state, "title": title, "message": message, "linked_panel_id": ""}


def series(name, values, unit="", labels=None, start_ms=None, step_ms=300000):
    start = now_ms() - step_ms * (len(values) - 1) if start_ms is None else start_ms
    return [metric(name, value, unit, start + index * step_ms, **(labels or {}))
            for index, value in enumerate(values)]


def metrics_payload():
    points = [42, 48, 45, 61, 58, 67, 73, 69, 81, 76, 88, 84,
              79, 83, 91, 86, 78, 82, 74, 69, 72, 77, 85, 83]
    records = series("series.value", points, "units")
    records += series("metric.value", [72, 83], "units")
    records += series("metric.secondary", [41, 46], "units")
    records += series("metric.tertiary", [18, 22], "units")
    records += series("progress.percent", [62, 68], "percent")
    records += series("comparison.current", points[-12:], "units")
    records += series("comparison.previous", [55, 59, 52, 63, 60, 66, 61, 68, 64, 70, 67, 72], "units")
    records += series("table.value", [23, 87, 45, 91, 66, 34, 78, 54], "units",
                      start_ms=now_ms() - 7 * 60000, step_ms=60000)
    names = ["alpha", "beta", "gamma", "delta"]
    for index, record in enumerate(records):
        if record["metric"] == "table.value":
            record["labels"] = {"name": names[index % 4],
                                "status": ("ok", "degraded", "ok", "down")[index % 4]}
    records += [metric("status.value", value, "state", now_ms(), name=name,
                       status=status)
                for name, value, status in (("alpha", 0, "ok"), ("beta", 1, "degraded"),
                                            ("gamma", 0, "ok"), ("delta", 2, "down"),
                                            ("edge", 0, "ok"), ("backup", 1, "degraded"))]
    heat = [round(20 + 70 * (0.5 + 0.5 * math.sin(index * 0.63)), 1) for index in range(32)]
    records += series("heatmap.value", heat, "units", start_ms=now_ms() - 31 * 60000, step_ms=60000)
    records += series("timeline.value", [0, 0, 1, 0, 2, 0, 0, 1, 0, 0, 0, 2], "state",
                      start_ms=now_ms() - 11 * 300000)
    return {"metrics": records, "events": [], "text": "Generic metrics updated"}


def token_payload():
    used = [52000 + index * 1350 for index in range(24)]
    records = series("token.used", used, "tokens", {"account": "demo-account"}, step_ms=900000)
    remaining = max(0, 100 - used[-1] / 1000)
    records += series("token.remaining", [remaining + 2, remaining], "percent",
                      {"account": "demo-account"})
    records += series("token.input", [31000, 32450], "tokens", {"account": "demo-account"})
    records += series("token.output", [19000, 20300], "tokens", {"account": "demo-account"})
    return {"metrics": records, "events": [], "text": "TOKEN mock data is not a provider bill"}


def weather_payload():
    temperature = [round(21 + 4 * math.sin(index / 3.2) + index * 0.08, 1) for index in range(24)]
    records = series("weather.temperature", temperature, "C", {"city": "Shanghai"}, step_ms=900000)
    records += series("weather.humidity", [66, 64], "percent", {"city": "Shanghai"})
    records += series("weather.wind_speed", [12.4, 11.8], "km/h", {"city": "Shanghai"})
    records += series("weather.pressure", [1014, 1013], "hPa", {"city": "Shanghai"})
    records += series("weather.aqi", [42, 39], "AQI", {"city": "Shanghai"})
    events = [event("weather-advisory", "info", "Weather advisory", "Mock breeze from the east", now_ms())]
    return {"metrics": records, "events": events, "text": "Shanghai: mild with a light breeze"}


def server_payload():
    cpu = [38, 42, 47, 44, 51, 58, 54, 63, 68, 61, 72, 77,
           69, 74, 82, 79, 85, 88, 81, 76, 83, 89, 84, 86]
    memory = [55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66,
              67, 66, 68, 69, 70, 71, 72, 73, 72, 74, 73, 72]
    records = series("server.cpu", cpu, "percent", {"server": "srv-demo", "name": "srv-demo"})
    records += series("server.memory", memory, "percent", {"server": "srv-demo", "name": "srv-demo"})
    records += series("server.disk", [61, 62], "percent", {"server": "srv-demo", "name": "srv-demo"})
    records += [metric("server.health", value, "state", now_ms(), name=name, status=status)
                for name, value, status in (("srv-demo", 1, "degraded"), ("srv-backup", 0, "ok"),
                                            ("srv-edge", 0, "ok"), ("srv-db", 2, "down"))]
    events = [event("server-cpu-warning", "warning", "High CPU", "srv-demo CPU is above 80%", now_ms()),
              event("server-backup-clear", "info", "Backup completed", "Nightly backup completed",
                    now_ms() - 600000, "cleared")]
    return {"metrics": records, "events": events, "text": "srv-demo: CPU high, memory stable"}


def security_payload():
    records = series("security.alerts.active", [2, 3], "events")
    records += series("security.blocked", [18, 21, 19, 25, 28, 24, 31, 29, 35, 32, 38, 36], "events")
    records += series("security.failed_logins", [4, 7], "events")
    current = now_ms()
    events = [
        event("sec-001", "critical", "Repeated authentication failures", "18 failures from one LAN client", current),
        event("sec-002", "warning", "Unusual outbound destination", "A new destination was blocked by policy", current - 60000),
        event("sec-003", "warning", "Port scan detected", "TCP scan from 198.51.100.20 was blocked", current - 120000),
        event("sec-004", "info", "Certificate rotation", "The mock server certificate was rotated", current - 240000, "cleared"),
        event("sec-005", "info", "Policy updated", "Network filter rules were reloaded", current - 360000, "cleared"),
    ]
    return {"metrics": records, "events": events, "text": "3 active security events; 1 critical"}


def events_payload():
    current = now_ms()
    records = series("alerts.active", [2, 3], "events")
    events = [
        event("evt-001", "critical", "Service unavailable", "Mock health check failed", current),
        event("evt-002", "warning", "Latency high", "Mock p95 latency exceeds 800 ms", current - 30000),
        event("evt-003", "warning", "Disk usage high", "Mock storage reached 87%", current - 60000),
        event("evt-004", "info", "Deployment finished", "Mock release was deployed", current - 120000, "cleared"),
        event("evt-005", "info", "Probe recovered", "Mock probe recovered", current - 180000, "cleared"),
    ]
    return {"metrics": records, "events": events, "text": "3 active events"}


def window_payload(query):
    points = bounded_int(query, "points", 24, 2, 128)
    hours = bounded_int(query, "hours", 5, 1, 168)
    step_ms = hours * 3600000 // (points - 1)
    end = now_ms()
    start = end - hours * 3600000
    values = [round(50 + 25 * math.sin(index / 4) + index / 8, 2) for index in range(points)]
    records = series("series.value", values, "units", start_ms=start, step_ms=step_ms)
    return {"reset_at": end + step_ms, "window": {"from": start, "to": end, "points": points},
            "metrics": records, "events": [], "text": f"{points} points over {hours}h"}


def bounded_int(query, key, default, minimum, maximum):
    try:
        value = int(query.get(key, [str(default)])[0])
    except (ValueError, TypeError):
        value = default
    return max(minimum, min(maximum, value))


def text_payload(query):
    scenario = query.get("scenario", ["notes"])[0]
    if scenario == "key_value":
        return "host: srv-demo\nregion: lan\nversion: 2.0.0\nstatus: ready\n"
    if scenario == "log":
        return "[12:00:01] INFO poll started\n[12:00:02] INFO metrics mapped\n" \
               "[12:00:03] WARN latency above target\n[12:00:04] INFO window stored\n" \
               "[12:00:05] INFO display update queued\n[12:00:06] INFO poll complete\n"
    return "EEGO dashboard mock\nAll values are synthetic.\nEdit this source on your server; no device script is required.\n"


def png_chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", binascii.crc32(kind + data) & 0xffffffff)


def mock_png(weather=False):
    width = height = 64
    rows = []
    for y in range(height):
        row = bytearray([0])
        for x in range(width):
            border = x in (0, width - 1) or y in (0, height - 1)
            sun = (x - 21) ** 2 + (y - 22) ** 2 < 100
            cloud = 19 < x < 53 and 32 < y < 45 and not (x < 27 and y < 37)
            if weather:
                shade = 15 if border else 0 if sun else 5 if cloud else 12
            else:
                shade = 15 if border else 0 if (x // 8 + y // 8) % 2 else 12
            value = round(shade * 255 / 15)
            row.extend((value, value, value))
        rows.append(bytes(row))
    header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", header) + \
        png_chunk(b"IDAT", zlib.compress(b"".join(rows), 9)) + png_chunk(b"IEND", b"")


class Handler(BaseHTTPRequestHandler):
    server_version = "MockDashboard/1.0"
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        pass

    def record(self, path):
        with _lock:
            _poll_count[path] += 1
            _request_log.append({"method": self.command, "path": path, "poll": _poll_count[path]})
            if len(_request_log) > 200:
                del _request_log[:-200]
            return _poll_count[path]

    def send_body(self, body, content_type, status=200):
        if isinstance(body, str):
            body = body.encode("utf-8")
        etag = '"' + hashlib.sha256(body).hexdigest() + '"'
        if self.headers.get("If-None-Match") == etag:
            self.send_response(304)
            self.send_header("ETag", etag)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("ETag", etag)
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def send_json(self, payload, status=200):
        self.send_body(json.dumps(payload, ensure_ascii=False, separators=(",", ":")),
                       "application/json; charset=utf-8", status)

    def do_HEAD(self):
        self.do_GET()

    def do_GET(self):
        from urllib.parse import parse_qs, urlsplit
        parsed = urlsplit(self.path)
        path = parsed.path.strip("/")
        query = parse_qs(parsed.query)
        self.record(path)
        if path == "":
            return self.send_json({"service": "mock_dashboard_server", "port": self.server.server_port,
                "endpoints": ["token", "weather", "server", "security", "metrics", "events", "text",
                              "window?points=24&hours=5", "assets/status.png", "assets/weather.png",
                              "_log", "_reset"],
                "note": "Synthetic data only. Do not send credentials to this mock."})
        if path == "_log":
            with _lock:
                return self.send_json({"poll_count": dict(_poll_count), "requests": _request_log})
        if path == "_reset":
            with _lock:
                _poll_count.clear()
                _request_log.clear()
            return self.send_json({"ok": True})
        payloads = {"metrics": metrics_payload, "token": token_payload, "weather": weather_payload,
                    "server": server_payload, "security": security_payload, "events": events_payload}
        if path in payloads:
            return self.send_json(payloads[path]())
        if path == "window":
            return self.send_json(window_payload(query))
        if path == "text":
            return self.send_body(text_payload(query), "text/plain; charset=utf-8")
        if path == "assets/status.png":
            return self.send_body(mock_png(False), "image/png")
        if path == "assets/weather.png":
            return self.send_body(mock_png(True), "image/png")
        self.send_json({"error": "unknown endpoint"}, 404)


def self_test():
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    base = f"http://127.0.0.1:{server.server_port}"
    checks = []
    try:
        for scenario, expected in (("token", "token.used"), ("weather", "weather.temperature"),
                                   ("server", "server.cpu"), ("security", "security.alerts.active"),
                                   ("metrics", "series.value"), ("events", "alerts.active")):
            with urllib.request.urlopen(base + "/" + scenario, timeout=5) as response:
                payload = json.load(response)
                assert response.status == 200
                assert any(item["metric"] == expected for item in payload["metrics"]), scenario
                checks.append(f"{scenario}: {len(payload['metrics'])} metrics/{len(payload['events'])} events")
        with urllib.request.urlopen(base + "/window?points=32&hours=5", timeout=5) as response:
            payload = json.load(response)
        timestamps = [item["timestamp_ms"] for item in payload["metrics"]]
        assert len(timestamps) == 32 and timestamps == sorted(timestamps) and len(set(timestamps)) == 32
        assert payload["window"]["from"] == timestamps[0] and payload["reset_at"] > timestamps[-1]
        checks.append("window: 32 ordered timestamped points + reset_at")
        for scenario, marker in (("key_value", "host:"), ("log", "INFO"), ("notes", "synthetic")):
            with urllib.request.urlopen(base + "/text?scenario=" + scenario, timeout=5) as response:
                assert marker in response.read().decode("utf-8")
        checks.append("text: notes/key-value/log variants")
        for asset in ("status", "weather"):
            with urllib.request.urlopen(base + f"/assets/{asset}.png", timeout=5) as response:
                body = response.read()
                assert response.headers["Content-Type"] == "image/png"
                assert body.startswith(b"\x89PNG\r\n\x1a\n") and len(body) < 256 * 1024
                width, height = struct.unpack(">II", body[16:24])
                assert (width, height) == (64, 64)
        checks.append("assets: two decodable 64x64 PNG responses below 256 KiB")
        with urllib.request.urlopen(base + "/_log", timeout=5) as response:
            log = json.load(response)
        assert log["poll_count"]["token"] == 1 and all("?" not in item["path"] for item in log["requests"])
        checks.append("request log: counts available, query strings and headers excluded")
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)
    for check in checks:
        print("[PASS]", check)
    print(f"summary: {len(checks)} HTTP mock checks passed")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1", help="bind address; use 0.0.0.0 explicitly for LAN devices")
    parser.add_argument("--port", type=int, default=8978)
    parser.add_argument("--self-test", action="store_true", help="run loopback HTTP contract checks and exit")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    server = ThreadingHTTPServer((args.host, args.port), Handler)
    lan_ip = "127.0.0.1"
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.connect(("192.0.2.1", 80))
        lan_ip = sock.getsockname()[0]
        sock.close()
    except OSError:
        pass
    print(f"mock_dashboard_server: http://{args.host}:{args.port}/")
    print(f"device mock_host example: {lan_ip}:{args.port}; no credentials are needed or accepted")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
