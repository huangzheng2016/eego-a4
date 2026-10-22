#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""List, validate, and expand EEGO dashboard presets without device-side scripting."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

from dashboard_validation import (
    SchemaValidator,
    _scan_secrets,
    bundle_from_preset,
    check_library,
    load_json,
    validate_bundle,
)

HOST_PORT_RE = re.compile(r"^(?:\[[0-9A-Fa-f:]+\]|[A-Za-z0-9.-]+)(?::[0-9]{1,5})?$")


def firmware_root(value=None):
    return Path(value).resolve() if value else Path(__file__).resolve().parent.parent


def load_catalog(root):
    catalog = load_json(root / "presets/index.json")
    return catalog, {entry["id"]: entry for entry in catalog.get("presets", [])}


def command_list(root, as_json):
    catalog, entries = load_catalog(root)
    if as_json:
        print(json.dumps(catalog, ensure_ascii=False, indent=2))
        return 0
    for entry in entries.values():
        print(f"{entry['id']:22} {entry['kind']:6} {entry['title']}  ({entry['file']})")
    print(f"\n{len(entries)} presets; use export to create a device-native dashboard bundle.")
    return 0


def command_export(root, preset_id, mock_host, output):
    _, entries = load_catalog(root)
    if preset_id not in entries:
        print(f"error: unknown preset {preset_id!r}", file=sys.stderr)
        return 2
    if mock_host is not None and (not HOST_PORT_RE.fullmatch(mock_host) or
                                  (mock_host.rsplit(":", 1)[-1].isdigit() and
                                   not 1 <= int(mock_host.rsplit(":", 1)[-1]) <= 65535)):
        print("error: --mock-host must be a host or host:port without scheme, path, or credentials",
              file=sys.stderr)
        return 2
    preset = load_json(root / "presets" / entries[preset_id]["file"])
    try:
        bundle = bundle_from_preset(preset, mock_host)
    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    schema = load_json(root / "components/dashboard_core/schema/eego-dashboard-v2.schema.json")
    errors = validate_bundle(bundle, schema) + _scan_secrets(bundle)
    if errors:
        for error in errors:
            print(f"error: {error}", file=sys.stderr)
        return 1
    rendered = json.dumps(bundle, ensure_ascii=False, indent=2) + "\n"
    if output:
        destination = Path(output)
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(rendered, encoding="utf-8")
        print(f"wrote {destination} ({len(rendered.encode('utf-8'))} bytes)")
    else:
        sys.stdout.write(rendered)
    return 0


def command_validate(root, paths):
    schema = load_json(root / "components/dashboard_core/schema/eego-dashboard-v2.schema.json")
    failed = False
    for name in paths:
        path = Path(name)
        try:
            raw = path.read_bytes()
            bundle = load_json(path)
            errors = validate_bundle(bundle, schema, len(raw)) + _scan_secrets(bundle)
        except (OSError, ValueError, json.JSONDecodeError) as exc:
            errors = [str(exc)]
        if errors:
            failed = True
            for error in errors:
                print(f"[FAIL] {path}: {error}")
        else:
            print(f"[PASS] {path}: schema, references, layout, source plans, and budgets")
    return 1 if failed else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", help="firmware directory (defaults to this tool's parent)")
    subparsers = parser.add_subparsers(dest="command", required=True)
    list_parser = subparsers.add_parser("list", help="list selectable panel/layout presets")
    list_parser.add_argument("--json", action="store_true")
    export_parser = subparsers.add_parser("export", help="expand one preset to a native bundle")
    export_parser.add_argument("preset_id")
    export_parser.add_argument("--mock-host", help="set the mock_host default to host[:port]")
    export_parser.add_argument("--output", help="output file; stdout when omitted")
    validate_parser = subparsers.add_parser("validate", help="validate one or more native bundles")
    validate_parser.add_argument("paths", nargs="+")
    subparsers.add_parser("validate-library", help="validate the complete catalog and four examples")
    args = parser.parse_args()
    root = firmware_root(args.root)
    try:
        if args.command == "list":
            return command_list(root, args.json)
        if args.command == "export":
            return command_export(root, args.preset_id, args.mock_host, args.output)
        if args.command == "validate":
            return command_validate(root, args.paths)
        errors, stats = check_library(root)
        for error in errors:
            print(f"[FAIL] {error}")
        if not errors:
            print(f"[PASS] {stats['presets']} presets + {stats['examples']} examples; "
                  f"max bundle={stats['max_bundle_bytes']} bytes, "
                  f"max dashboard={stats['max_dashboard_bytes']} bytes")
        return 1 if errors else 0
    except (OSError, ValueError, json.JSONDecodeError, KeyError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
