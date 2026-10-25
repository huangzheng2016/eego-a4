#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Expand every preset/example into a plain eego.dashboard/2 JSON with data.

Each source URL is answered in-process by tools/mock_dashboard_server.py (the
same synthetic data the device sees from the mock), so panels show values:
metrics -> samples, events -> events, /text -> text panel content, PNG assets
-> files under <out>/assets/. A synthetic "gallery" fixture adds small/narrow
panels, empty states, big numbers and CJK titles for degradation checks.

usage: make_fixtures.py <out-dir>
"""
from __future__ import annotations

import copy
import importlib.util
import json
import re
import sys
from pathlib import Path
from urllib.parse import parse_qs, urlsplit

FW = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location("mock", FW / "tools" / "mock_dashboard_server.py")
mock = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mock)

# Fixed clock so renders are reproducible (2026-10-25 09:30 local-ish).
FIXED_NOW = 1793000000000
mock.now_ms = lambda: FIXED_NOW


def interpolate(text, variables):
    return re.sub(r"\$\{(\w+)\}", lambda m: variables.get(m.group(1), m.group(0)), text)


def answer(url):
    """Returns ('json', payload) | ('text', str) | ('png', bytes) | (None, None)."""
    parts = urlsplit(url)
    path = parts.path.strip("/")
    query = parse_qs(parts.query)
    if path == "text":
        return "text", mock.text_payload(query)
    if path == "window":
        return "json", mock.window_payload(query)
    if path == "assets/status.png":
        return "png", mock.mock_png(False)
    if path == "assets/weather.png":
        return "png", mock.mock_png(True)
    table = {"metrics": mock.metrics_payload, "events": mock.events_payload,
             "security": mock.security_payload}
    if path in table:
        return "json", table[path]()
    table_q = {"token": mock.token_payload, "weather": mock.weather_payload,
               "server": mock.server_payload}
    if path in table_q:
        return "json", table_q[path](query)
    return None, None


def dashboard_of(name, root):
    if root.get("schema", "").startswith("eego.dashboard-preset"):
        tmpl = root["dashboard"]
        dash = {"schema": "eego.dashboard/2", "schema_version": 2, "id": root["id"],
                "title": root.get("title", ""),
                "default_page_id": tmpl["pages"][0]["id"], "refresh_ms": 60000}
        for key in ("sources", "variables", "pages", "alert_rules", "assets", "samples", "events"):
            dash[key] = copy.deepcopy(tmpl.get(key, []))
        return dash
    return copy.deepcopy(root["dashboard"])


def populate(dash, out_dir):
    variables = {v["name"]: v.get("default_value", "") for v in dash.get("variables", [])}
    texts = {}
    for source in dash.get("sources", []):
        kind, payload = answer(interpolate(source.get("url", ""), variables))
        if kind == "json":
            for record in payload.get("metrics", []):
                sample = dict(record)
                sample["source_id"] = source["id"]
                dash["samples"].append(sample)
            for record in payload.get("events", []):
                event = dict(record)
                event["source_id"] = source["id"]
                dash["events"].append(event)
            if payload.get("text"):
                texts[source["id"]] = payload["text"]
        elif kind == "text":
            texts[source["id"]] = payload
        elif kind == "png":
            path = out_dir / urlsplit(interpolate(source["url"], variables)).path.strip("/")
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(payload)
    for asset in dash.get("assets", []):
        kind, payload = answer(interpolate(asset.get("uri", ""), variables))
        if kind == "png":
            path = out_dir / urlsplit(interpolate(asset["uri"], variables)).path.strip("/")
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(payload)
    for page in dash["pages"]:
        for panel in page["panels"]:
            if panel.get("kind") == "text" and panel.get("source_id") in texts:
                panel.setdefault("text", {})["content"] = texts[panel["source_id"]].replace("${", "$${")
    return dash


def panel(pid, kind, x, y, w, h, title, **extra):
    out = {"id": pid, "kind": kind, "grid": {"x": x, "y": y, "w": w, "h": h}, "title": title,
           "visible": True, "refresh_ms": 60000}
    out.update(extra)
    if kind == "table" and "columns" not in out:
        out["columns"] = [{"key": "name", "title": "Name", "width": 8},
                          {"key": "value", "title": "Value", "width": 6}]
    return out


def gallery():
    """Edge cases: small panels, empty data, large numbers, negatives, CJK."""
    now = FIXED_NOW
    samples = []

    def series(metric, values, unit="", labels=None, step=300000):
        for i, v in enumerate(values):
            samples.append({"source_id": "g", "metric": metric, "value": v, "unit": unit,
                            "timestamp_ms": now - step * (len(values) - 1 - i),
                            "labels": labels or {}})

    series("big", [1834220, 1912345.678], "tokens")
    series("neg", [12.5, -3.25], "C")
    series("pct", [71.234, 64.9], "percent")
    series("tiny", [0.00123, 0.00119], "s")
    series("crit", [91, 97], "%")
    series("bytes", [5.1e9, 5.3e9], "B")
    series("trend", [3, 5, 4, 8, 6, 9, 12, 10, 14, 13, 17, 15, 19, 22, 18, 25], "req/s")
    series("neg_trend", [5, 3, -2, -6, -4, 1, 4, 2, -1, 3], "")
    for i, (name, value, status) in enumerate((("api", 0, "ok"), ("db", 2, "down"),
                                               ("cache", 1, "degraded"), ("queue", 0, "ok"),
                                               ("cdn", 0, "ok"), ("auth", 0, "ok"),
                                               ("mail", 1, "degraded"), ("dns", 0, "ok"))):
        samples.append({"source_id": "g", "metric": "grid", "value": value, "unit": "state",
                        "timestamp_ms": now, "labels": {"name": name, "status": status}})
    thresholds = [{"value": 80, "severity": "warning", "label": "High"},
                  {"value": 95, "severity": "critical", "label": "Critical"}]
    state_thresholds = [{"value": 1, "severity": "warning", "label": "Degraded"},
                        {"value": 2, "severity": "critical", "label": "Down"}]
    p1 = [
        panel("big", "card", 0, 0, 12, 2, "累计 Token 用量", source_id="g", metric="big",
              style={"variant": "default", "precision": 0}),
        panel("neg", "card", 12, 0, 6, 2, "Outdoor", source_id="g", metric="neg",
              style={"variant": "default", "precision": 1, "unit": "°C"}),
        panel("crit", "card", 18, 0, 6, 2, "Disk", source_id="g", metric="crit",
              thresholds=thresholds, style={"variant": "default", "precision": 0}),
        panel("tiny", "card", 0, 2, 6, 1, "Latency", source_id="g", metric="tiny",
              style={"variant": "default", "precision": 4}),
        panel("bytes", "card", 6, 2, 6, 1, "Storage", source_id="g", metric="bytes",
              style={"variant": "default", "precision": 1}),
        panel("delta", "card", 12, 2, 6, 1, "Δ Temp", source_id="g", metric="neg",
              style={"variant": "delta", "precision": 1, "unit": "°C"}),
        panel("empty_card", "card", 18, 2, 6, 1, "No data", source_id="g", metric="missing",
              style={"variant": "default"}),
        panel("prog_small", "card", 0, 3, 8, 1, "Quota", source_id="g", metric="pct",
              style={"variant": "progress", "precision": 0}),
        panel("status_small", "card", 8, 3, 8, 1, "DB", source_id="g", metric="crit",
              thresholds=thresholds, style={"variant": "status"}),
        panel("spark_small", "chart", 16, 3, 8, 1, "Requests", source_id="g", metric="trend",
              chart={"kind": "sparkline"}, style={"unit": "req/s", "precision": 0}),
        panel("bars_neg", "chart", 0, 4, 12, 2, "Net change", source_id="g", metric="neg_trend",
              chart={"kind": "bar"}, style={"precision": 0}),
        panel("line_small", "chart", 12, 4, 12, 2, "Trend", source_id="g", metric="trend",
              chart={"kind": "line", "fill": True}, style={"unit": "req/s", "precision": 0},
              thresholds=[{"value": 20, "severity": "warning", "label": "Busy"}]),
        panel("grid8", "table", 0, 6, 12, 2, "Services", source_id="g", metric="grid",
              thresholds=state_thresholds, style={"variant": "status_grid", "precision": 0}),
        panel("empty_chart", "chart", 12, 6, 6, 2, "Empty chart", source_id="g",
              metric="missing", chart={"kind": "line"}),
        panel("empty_table", "table", 18, 6, 6, 2, "Empty table", source_id="g",
              metric="missing", style={"variant": "default"}),
    ]
    p2 = [
        panel("wide_big", "card", 0, 0, 24, 3, "Requests / sec", source_id="g", metric="trend",
              style={"variant": "default", "precision": 0}),
        panel("narrow1", "card", 0, 3, 4, 2, "CPU", source_id="g", metric="pct",
              style={"variant": "default", "precision": 1}),
        panel("narrow2", "card", 4, 3, 4, 2, "Tokens", source_id="g", metric="big",
              style={"variant": "default", "precision": 0}),
        panel("table_wide", "table", 8, 3, 16, 3, "Recent", source_id="g", metric="trend",
              columns=[{"key": "timestamp_ms", "title": "Time", "width": 6},
                       {"key": "value", "title": "Rate", "width": 6}],
              style={"variant": "default", "precision": 0}),
        panel("timeline_g", "table", 0, 5, 8, 1, "Incidents", source_id="g", metric="neg_trend",
              thresholds=[{"value": 3, "severity": "warning", "label": "w"}],
              style={"variant": "timeline"}),
        panel("alerts_empty", "alert", 0, 6, 12, 2, "Alerts"),
        panel("text_empty", "text", 12, 6, 12, 2, "Notes", text={"content": "", "multiline": True,
                                                                 "max_lines": 4},
              style={"variant": "multiline"}),
    ]
    return {"schema": "eego.dashboard/2", "schema_version": 2, "id": "gallery",
            "title": "Gallery", "default_page_id": "edge", "refresh_ms": 60000,
            "sources": [{"id": "g", "kind": "http", "enabled": True, "url": "http://x/none",
                         "refresh_ms": 60000, "timeout_ms": 10000, "max_response_bytes": 65536,
                         "tls_verify": True, "auth_token_ref": "", "headers": {}}],
            "variables": [],
            "pages": [{"id": "edge", "title": "Edge", "visible": True, "grid_columns": 24, "panels": p1},
                      {"id": "edge2", "title": "Edge 2", "visible": True, "grid_columns": 24, "panels": p2}],
            "alert_rules": [], "assets": [], "samples": samples, "events": []}


def main():
    out_dir = Path(sys.argv[1] if len(sys.argv) > 1 else "fixtures")
    out_dir.mkdir(parents=True, exist_ok=True)
    count = 0
    for sub in ("panels", "layouts", "examples"):
        for path in sorted((FW / "presets" / sub).glob("*.json")):
            root = json.loads(path.read_text(encoding="utf-8"))
            dash = populate(dashboard_of(path.stem, root), out_dir)
            (out_dir / f"{sub}-{path.stem}.json").write_text(
                json.dumps(dash, ensure_ascii=False, indent=1), encoding="utf-8")
            count += 1
    (out_dir / "gallery.json").write_text(json.dumps(gallery(), ensure_ascii=False, indent=1),
                                          encoding="utf-8")
    print(f"{count + 1} fixtures -> {out_dir}")


if __name__ == "__main__":
    main()
