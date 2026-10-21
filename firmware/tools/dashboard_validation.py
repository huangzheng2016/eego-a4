#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Standard-library validation and preset expansion for EEGO dashboard bundles."""

from __future__ import annotations

import copy
import json
import math
import re
from pathlib import Path
from typing import Any

DASHBOARD_SCHEMA = "eego.dashboard/2"
BUNDLE_SCHEMA = "eego.dashboard-bundle/1"
PRESET_SCHEMA = "eego.dashboard-preset/1"
CATALOG_SCHEMA = "eego.dashboard-preset-catalog/1"
MAX_DASHBOARD_BYTES = 128 * 1024
MAX_BUNDLE_BYTES = 192 * 1024
MAX_SOURCE_PLAN_BYTES = 32 * 1024
MAX_NETWORK_IMAGE_BYTES = 256 * 1024
MAX_ENCODED_IMAGE_BYTES = 512 * 1024

ID_RE = re.compile(r"^[A-Za-z0-9_-]{1,64}$")
VARIABLE_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]{0,63}$")
DURATION_RE = re.compile(r"(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(ms|s|m|h|d|w)$")
SECRET_KEYS = {"password", "passwd", "secret", "api_key", "apikey", "auth_token", "ingest_key"}

REQUIRED_PRESETS = {
    "metric-single", "metric-dual", "metric-triple", "progress", "line-chart", "area-chart",
    "bar-chart", "sparkline", "table", "key-value", "log-tail", "event-stream",
    "alert-count", "alert-banner", "alert-table", "status-grid", "heatmap", "timeline",
    "text-single", "text-multiline", "image", "image-text", "button", "button-group",
    "chart-kpi", "chart-alerts", "kpi-2x2", "three-row-stack", "comparison", "mosaic",
}


def _no_duplicate_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON object key: {key}")
        result[key] = value
    return result


def _reject_constant(value):
    raise ValueError(f"non-finite JSON number: {value}")


def load_json(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=_no_duplicate_object,
                      parse_constant=_reject_constant)


def compact_bytes(value: Any) -> int:
    return len(json.dumps(value, ensure_ascii=False, separators=(",", ":"),
                          allow_nan=False).encode("utf-8"))


def json_depth(value: Any) -> int:
    if isinstance(value, dict):
        return 1 + max((json_depth(item) for item in value.values()), default=0)
    if isinstance(value, list):
        return 1 + max((json_depth(item) for item in value), default=0)
    return 1


def _same_json(left, right):
    return json.dumps(left, ensure_ascii=False, sort_keys=True) == json.dumps(
        right, ensure_ascii=False, sort_keys=True)


class SchemaValidator:
    """The Draft 2020-12 keywords used by eego-dashboard-v2.schema.json."""

    def __init__(self, root):
        self.root = root

    def validate(self, value, schema=None, path="$", errors=None):
        if errors is None:
            errors = []
        if schema is None:
            schema = self.root
        if len(errors) >= 200:
            return errors
        if "$ref" in schema:
            reference = schema["$ref"]
            if not reference.startswith("#/"):
                errors.append(f"{path}: unsupported schema reference {reference}")
                return errors
            target = self.root
            for part in reference[2:].split("/"):
                target = target[part.replace("~1", "/").replace("~0", "~")]
            self.validate(value, target, path, errors)
        expected = schema.get("type")
        if expected is not None and not self._type(value, expected):
            errors.append(f"{path}: expected {expected}")
            return errors
        if "const" in schema and not _same_json(value, schema["const"]):
            errors.append(f"{path}: expected constant {schema['const']!r}")
        if "enum" in schema and not any(_same_json(value, item) for item in schema["enum"]):
            errors.append(f"{path}: value is not in the allowed enum")
        if isinstance(value, str):
            if len(value) < schema.get("minLength", 0):
                errors.append(f"{path}: string is shorter than {schema['minLength']} code points")
            if "maxLength" in schema and len(value) > schema["maxLength"]:
                errors.append(f"{path}: string exceeds {schema['maxLength']} code points")
            if "pattern" in schema and re.search(schema["pattern"], value) is None:
                errors.append(f"{path}: string does not match {schema['pattern']}")
        if self._number(value):
            if "minimum" in schema and value < schema["minimum"]:
                errors.append(f"{path}: number is below {schema['minimum']}")
            if "maximum" in schema and value > schema["maximum"]:
                errors.append(f"{path}: number exceeds {schema['maximum']}")
        if isinstance(value, dict):
            for key in schema.get("required", []):
                if key not in value:
                    errors.append(f"{path}: missing required field {key}")
            if len(value) > schema.get("maxProperties", len(value)):
                errors.append(f"{path}: too many object properties")
            properties = schema.get("properties", {})
            additional = schema.get("additionalProperties")
            if additional is False:
                for key in value:
                    if key not in properties:
                        errors.append(f"{path}.{key}: unknown field")
            if "propertyNames" in schema:
                for key in value:
                    self.validate(key, schema["propertyNames"], f"{path}.{key}", errors)
            for key, child in value.items():
                if key in properties:
                    self.validate(child, properties[key], f"{path}.{key}", errors)
                elif isinstance(additional, dict):
                    self.validate(child, additional, f"{path}.{key}", errors)
        if isinstance(value, list):
            if len(value) < schema.get("minItems", 0):
                errors.append(f"{path}: too few items")
            if "maxItems" in schema and len(value) > schema["maxItems"]:
                errors.append(f"{path}: exceeds {schema['maxItems']} items")
            if schema.get("uniqueItems"):
                seen = set()
                for item in value:
                    marker = json.dumps(item, ensure_ascii=False, sort_keys=True)
                    if marker in seen:
                        errors.append(f"{path}: items must be unique")
                        break
                    seen.add(marker)
            if "items" in schema:
                for index, item in enumerate(value):
                    self.validate(item, schema["items"], f"{path}[{index}]", errors)
        for child in schema.get("allOf", []):
            self.validate(value, child, path, errors)
        if "if" in schema:
            branch = "then" if not self.validate(value, schema["if"], path, []) else "else"
            if branch in schema:
                self.validate(value, schema[branch], path, errors)
        if "not" in schema and not self.validate(value, schema["not"], path, []):
            errors.append(f"{path}: matches a forbidden schema")
        return errors

    @staticmethod
    def _number(value):
        return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value)

    @classmethod
    def _type(cls, value, expected):
        if isinstance(expected, list):
            return any(cls._type(value, item) for item in expected)
        if expected == "object":
            return isinstance(value, dict)
        if expected == "array":
            return isinstance(value, list)
        if expected == "string":
            return isinstance(value, str)
        if expected == "boolean":
            return isinstance(value, bool)
        if expected == "integer":
            return type(value) is int
        if expected == "number":
            return cls._number(value)
        if expected == "null":
            return value is None
        return False


def _plan_schema():
    string = {"type": "string"}
    path = {"type": "string", "maxLength": 1024}
    metric = {
        "type": "object", "additionalProperties": False,
        "properties": {key: {"type": "string"} for key in
                       ("name", "items_path", "name_path", "value_path", "timestamp_path", "unit",
                        "unit_path", "target_unit", "expression", "reset_at_path")},
    }
    metric["properties"]["fields"] = {"type": "object", "maxProperties": 32,
                                      "additionalProperties": {"type": "string"}}
    metric["properties"]["labels"] = {
        "type": "array", "maxItems": 16,
        "items": {"type": "object", "additionalProperties": False, "required": ["name"],
                  "properties": {"name": string, "path": string, "value": string}},
    }
    event = {
        "type": "object", "additionalProperties": False,
        "properties": {key: path for key in
                       ("items_path", "id_path", "rule_id_path", "timestamp_path", "severity_path",
                        "state_path", "title_path", "message_path", "linked_panel_id_path")},
    }
    event["properties"]["severity_map"] = {"type": "object", "maxProperties": 32,
        "additionalProperties": {"enum": ["info", "warning", "critical"]}}
    event["properties"]["clear_states"] = {"type": "array", "maxItems": 32, "items": string}
    properties = {
        "id": {"type": "string"}, "kind": {"enum": ["http", "static"]},
        "enabled": {"type": "boolean"}, "url": string,
        "refresh_ms": {"type": "integer", "minimum": 1000, "maximum": 86400000},
        "timeout_ms": {"type": "integer", "minimum": 100, "maximum": 120000},
        "max_response_bytes": {"type": "integer", "minimum": 1024, "maximum": 262144},
        "tls_verify": {"type": "boolean"}, "auth_token_ref": string,
        "headers": {"type": "object", "maxProperties": 16, "additionalProperties": string},
        "response": {"enum": ["generic_json", "prometheus", "standard_json", "metrics_json",
                                "events_json", "image", "text", "standard", "metrics", "events"]},
        "method": {"enum": ["GET", "POST", "HEAD"]}, "body": string, "static_body": string,
        "metrics": {"type": "array", "maxItems": 64, "items": metric},
        "events": {"type": "array", "maxItems": 16, "items": event},
        "text_path": path, "reset_at_path": path,
        "stale_after_ms": {"type": "integer", "minimum": 0, "maximum": 604800000},
        "conditional_get": {"type": "boolean"},
        "max_json_depth": {"type": "integer", "minimum": 1, "maximum": 16},
    }
    return {"type": "object", "additionalProperties": False, "required": ["id"],
            "properties": properties}


PLAN_SCHEMA = _plan_schema()
PLAN_VALIDATOR = SchemaValidator(PLAN_SCHEMA)
SOURCE_DEFAULTS = {
    "kind": "http", "enabled": True, "url": "", "refresh_ms": 60000, "timeout_ms": 10000,
    "max_response_bytes": 65536, "tls_verify": True, "auth_token_ref": "", "headers": {},
}


def bundle_from_preset(preset, mock_host=None):
    if preset.get("schema") != PRESET_SCHEMA or preset.get("schema_version") != 1:
        raise ValueError("expected eego.dashboard-preset/1")
    allowed = {"schema", "schema_version", "id", "kind", "title", "description", "dashboard",
               "source_plans"}
    unknown = set(preset) - allowed
    if unknown:
        raise ValueError(f"unknown preset fields: {sorted(unknown)}")
    template = preset.get("dashboard")
    if not isinstance(template, dict) or not isinstance(template.get("pages"), list) or not template["pages"]:
        raise ValueError("preset dashboard must contain pages")
    allowed_template = {"sources", "variables", "pages", "alert_rules", "assets", "samples", "events"}
    if set(template) - allowed_template:
        raise ValueError(f"preset contains device-level reserved fields: {sorted(set(template) - allowed_template)}")
    dashboard = {"schema": DASHBOARD_SCHEMA, "schema_version": 2, "id": preset.get("id"),
                 "title": preset.get("title", ""), "default_page_id": template["pages"][0].get("id"),
                 "refresh_ms": 60000}
    for key in allowed_template:
        dashboard[key] = copy.deepcopy(template.get(key, []))
    if mock_host is not None:
        for variable in dashboard["variables"]:
            if variable.get("name") == "mock_host":
                variable["default_value"] = mock_host
                break
        else:
            raise ValueError("preset has no mock_host variable to override")
    return {"schema": BUNDLE_SCHEMA, "schema_version": 1, "revision": 0,
            "dashboard": dashboard, "source_plans": copy.deepcopy(preset.get("source_plans", []))}


def _duplicates(values):
    seen = set()
    duplicates = set()
    for value in values:
        if value in seen:
            duplicates.add(value)
        seen.add(value)
    return sorted(duplicates)


def _time_ms(text):
    if text == "now":
        return 0
    duration = text[4:] if text.startswith("now-") else text[1:] if text.startswith("-") else text
    match = DURATION_RE.fullmatch(duration)
    if not match:
        return None
    number = float(duration[:-len(match.group(1))])
    factor = {"ms": 1, "s": 1000, "m": 60000, "h": 3600000, "d": 86400000,
              "w": 604800000}[match.group(1)]
    return -int(number * factor)


def _validate_placeholders(text, variables, path, errors):
    position = 0
    while position < len(text):
        escaped = text.startswith("$${", position)
        if not escaped and not text.startswith("${", position):
            position += 1
            continue
        start = position + (3 if escaped else 2)
        end = text.find("}", start)
        if end < 0:
            errors.append(f"{path}: unterminated variable placeholder")
            return
        name = text[start:end]
        if not VARIABLE_RE.fullmatch(name) or (not escaped and name not in variables):
            errors.append(f"{path}: invalid or undefined variable {name!r}")
        position = end + 1


def _valid_path(path, required=False):
    if not path:
        return not required
    if len(path) > 1024:
        return False
    position = 1 if path.startswith("$") else 0
    if position == 0 and path[0] in ".[":
        return False
    tokens = 0
    while position < len(path):
        if tokens >= 64:
            return False
        if path[position] == ".":
            position += 1
            if position >= len(path) or path[position] in ".[":
                return False
            if path[position] == "*":
                position += 1
                if position < len(path) and path[position] not in ".[":
                    return False
                tokens += 1
                continue
            start = position
            while position < len(path) and path[position] not in ".[":
                position += 1
            if position == start:
                return False
            tokens += 1
            continue
        if path[position] == "[":
            position += 1
            if position >= len(path):
                return False
            if path[position] == "*":
                position += 1
                if position >= len(path) or path[position] != "]":
                    return False
                position += 1
                tokens += 1
                continue
            if path[position] in "'\"":
                quote = path[position]
                position += 1
                key = ""
                closed = False
                while position < len(path):
                    char = path[position]
                    position += 1
                    if char == quote:
                        closed = True
                        break
                    if char == "\\":
                        if position >= len(path) or path[position] not in (quote, "\\"):
                            return False
                        char = path[position]
                        position += 1
                    elif ord(char) < 0x20:
                        return False
                    key += char
                    if len(key) > 256:
                        return False
                if not closed or not key or position >= len(path) or path[position] != "]":
                    return False
                position += 1
                tokens += 1
                continue
            start = position
            while position < len(path) and path[position].isdigit():
                position += 1
            if position == start or position - start > 9 or position >= len(path) or path[position] != "]":
                return False
            position += 1
            tokens += 1
            continue
        if tokens or position != 0:
            return False
        start = position
        while position < len(path) and path[position] not in ".[":
            position += 1
        if position == start:
            return False
        tokens += 1
    return True


def _validate_expression(expression, path, errors):
    if not expression:
        return
    if len(expression) > 512 or expression.count("(") != expression.count(")"):
        errors.append(f"{path}: invalid expression length or unbalanced parentheses")
        return
    functions = re.findall(r"\b([A-Za-z_][A-Za-z0-9_.]*)\s*\(", expression)
    allowed = {"abs", "min", "max", "clamp", "floor", "ceil", "round", "sqrt", "pow",
               "isfinite", "isnull", "coalesce", "convert"}
    for function in functions:
        if function not in allowed:
            errors.append(f"{path}: unsupported expression function {function}")
    if ";" in expression or re.search(r"(?<![=!<>])=(?!=)", expression):
        errors.append(f"{path}: unsupported expression syntax")


def _validate_plan(plan, path, errors):
    errors.extend(PLAN_VALIDATOR.validate(plan, path=path, errors=[]))
    if not isinstance(plan, dict):
        return
    response = plan.get("response", "standard_json")
    if response in ("standard", "metrics", "events"):
        response = {"standard": "standard_json", "metrics": "metrics_json", "events": "events_json"}[response]
    if response == "generic_json" and not plan.get("metrics") and not plan.get("events") and not plan.get("text_path"):
        errors.append(f"{path}: generic_json requires a metric, event, or text mapping")
    for key in ("text_path", "reset_at_path"):
        if key in plan and not _valid_path(plan[key], key == "reset_at_path" and False):
            errors.append(f"{path}.{key}: invalid JSONPath")
    for index, mapping in enumerate(plan.get("metrics", [])):
        item_path = f"{path}.metrics[{index}]"
        if bool(mapping.get("name")) == bool(mapping.get("name_path")):
            errors.append(f"{item_path}: exactly one of name/name_path is required")
        for key in ("items_path", "value_path", "name_path", "timestamp_path", "unit_path", "reset_at_path"):
            default = "$" if key == "items_path" else "$.value" if key == "value_path" else ""
            if not _valid_path(mapping.get(key, default), key in ("items_path", "value_path")):
                errors.append(f"{item_path}.{key}: invalid JSONPath")
        _validate_expression(mapping.get("expression", ""), item_path + ".expression", errors)
        for label_index, label in enumerate(mapping.get("labels", [])):
            if bool(label.get("path")) == bool(label.get("value")):
                errors.append(f"{item_path}.labels[{label_index}]: exactly one of path/value is required")
            if not _valid_path(label.get("path", "")):
                errors.append(f"{item_path}.labels[{label_index}].path: invalid JSONPath")
        for field, field_path in mapping.get("fields", {}).items():
            if not field or not _valid_path(field_path, True):
                errors.append(f"{item_path}.fields.{field}: invalid expression field mapping")
    for index, mapping in enumerate(plan.get("events", [])):
        item_path = f"{path}.events[{index}]"
        for key in ("items_path", "id_path", "rule_id_path", "timestamp_path", "severity_path",
                    "state_path", "title_path", "message_path", "linked_panel_id_path"):
            default = "$.events[*]" if key == "items_path" else "$." + key.removesuffix("_path")
            if not _valid_path(mapping.get(key, default), key == "items_path"):
                errors.append(f"{item_path}.{key}: invalid JSONPath")
    if plan.get("kind", "http") == "static" and response not in ("image", "text") and plan.get("static_body"):
        try:
            json.loads(plan["static_body"], parse_constant=_reject_constant)
        except (ValueError, json.JSONDecodeError) as exc:
            errors.append(f"{path}.static_body: invalid JSON: {exc}")
    if len(plan.get("static_body", "").encode("utf-8")) > plan.get("max_response_bytes", 65536):
        errors.append(f"{path}.static_body: exceeds max_response_bytes")


def validate_bundle(bundle, schema, raw_bytes=None, strict_layout=True):
    errors = []
    if not isinstance(bundle, dict):
        return ["$: bundle must be an object"]
    allowed = {"schema", "schema_version", "revision", "dashboard", "source_plans"}
    for key in set(bundle) - allowed:
        errors.append(f"$.{key}: unknown bundle field")
    if bundle.get("schema") != BUNDLE_SCHEMA or bundle.get("schema_version") != 1:
        errors.append("$: expected eego.dashboard-bundle/1")
    revision = bundle.get("revision", 0)
    if type(revision) is not int or not 0 <= revision <= 9223372036854775807:
        errors.append("$.revision: expected a non-negative int64 revision")
    dashboard = bundle.get("dashboard")
    if not isinstance(dashboard, dict):
        return errors + ["$.dashboard: required object"]
    schema_errors = SchemaValidator(schema).validate(dashboard)
    errors.extend(f"$.dashboard{error[1:] if error.startswith('$') else error}" for error in schema_errors)
    plans = bundle.get("source_plans", [])
    if not isinstance(plans, list) or len(plans) > 12:
        errors.append("$.source_plans: expected an array of at most 12 entries")
        plans = []
    if raw_bytes is not None and raw_bytes > MAX_BUNDLE_BYTES:
        errors.append(f"$: input bundle is {raw_bytes} bytes, above {MAX_BUNDLE_BYTES}")
    bundle_size = compact_bytes(bundle)
    dashboard_size = compact_bytes(dashboard)
    if bundle_size > MAX_BUNDLE_BYTES:
        errors.append(f"$: serialized bundle is {bundle_size} bytes, above {MAX_BUNDLE_BYTES}")
    if dashboard_size > MAX_DASHBOARD_BYTES:
        errors.append(f"$.dashboard: serialized dashboard is {dashboard_size} bytes, above {MAX_DASHBOARD_BYTES}")
    if json_depth(bundle) > 18:
        errors.append("$: bundle JSON depth exceeds 18")
    if json_depth(dashboard) > 16:
        errors.append("$.dashboard: dashboard JSON depth exceeds 16")
    for index, plan in enumerate(plans):
        if compact_bytes(plan) > MAX_SOURCE_PLAN_BYTES:
            errors.append(f"$.source_plans[{index}]: exceeds {MAX_SOURCE_PLAN_BYTES} bytes")
    if schema_errors:
        return errors

    sources = dashboard.get("sources", [])
    variables = dashboard.get("variables", [])
    pages = dashboard.get("pages", [])
    rules = dashboard.get("alert_rules", [])
    assets = dashboard.get("assets", [])
    samples = dashboard.get("samples", [])
    events = dashboard.get("events", [])
    source_ids = [item["id"] for item in sources]
    page_ids = [item["id"] for item in pages]
    variable_names = [item["name"] for item in variables]
    asset_ids = [item["id"] for item in assets]
    rule_ids = [item["id"] for item in rules]
    event_ids = [item["id"] for item in events]
    panel_items = [(page, panel) for page in pages for panel in page.get("panels", [])]
    panel_ids = [panel["id"] for _, panel in panel_items]
    for label, values in (("source", source_ids), ("page", page_ids), ("variable", variable_names),
                          ("asset", asset_ids), ("alert rule", rule_ids), ("event", event_ids),
                          ("panel", panel_ids)):
        for duplicate in _duplicates(values):
            errors.append(f"$.dashboard: duplicate {label} ID {duplicate}")
    source_set, page_set, variable_set = set(source_ids), set(page_ids), set(variable_names)
    panel_set, rule_set = set(panel_ids), set(rule_ids)
    asset_by_id = {item["id"]: item for item in assets}
    if dashboard.get("default_page_id", "") not in page_set:
        errors.append("$.dashboard.default_page_id: unknown page")
    for source_id in source_ids:
        if source_id in ("all", "current"):
            errors.append(f"$.dashboard.sources: reserved source ID {source_id}")

    for index, variable in enumerate(variables):
        path = f"$.dashboard.variables[{index}]"
        if variable.get("kind") == "query":
            if variable.get("source_id") not in source_set:
                errors.append(f"{path}.source_id: unknown query source")
        elif variable.get("source_id", ""):
            errors.append(f"{path}.source_id: only query variables may reference a source")
        options = variable.get("options", [])
        option_values = [option["value"] for option in options]
        for duplicate in _duplicates(option_values):
            errors.append(f"{path}.options: duplicate option value {duplicate}")
        default = variable.get("default_value", "")
        if options and variable.get("kind") != "query" and default not in option_values:
            if not (variable.get("include_all") and default == "__all__"):
                errors.append(f"{path}.default_value: not one of the declared options")
        if variable.get("kind") == "time_range" and default and _time_ms(default) is None:
            errors.append(f"{path}.default_value: invalid relative time range")

    for index, source in enumerate(sources):
        path = f"$.dashboard.sources[{index}]"
        _validate_placeholders(source.get("url", ""), variable_set, path + ".url", errors)
        for key, value in source.get("headers", {}).items():
            _validate_placeholders(value, variable_set, f"{path}.headers.{key}", errors)
        if source.get("auth_token_ref") and not source["auth_token_ref"].startswith("nvs:"):
            errors.append(f"{path}.auth_token_ref: use an NVS reference, never a literal token")
    for index, asset in enumerate(assets):
        path = f"$.dashboard.assets[{index}]"
        _validate_placeholders(asset.get("uri", ""), variable_set, path + ".uri", errors)
        if asset.get("kind", "image") == "image" and asset.get("size_bytes", 0) > MAX_ENCODED_IMAGE_BYTES:
            errors.append(f"{path}.size_bytes: image exceeds the {MAX_ENCODED_IMAGE_BYTES}-byte decode input budget")

    for page_index, page in enumerate(pages):
        occupied = []
        for panel_index, panel in enumerate(page.get("panels", [])):
            path = f"$.dashboard.pages[{page_index}].panels[{panel_index}]"
            source_id, metric = panel.get("source_id", ""), panel.get("metric", "")
            if bool(source_id) != bool(metric):
                errors.append(f"{path}: source_id and metric must be provided together")
            if source_id and source_id not in source_set:
                errors.append(f"{path}.source_id: unknown source {source_id}")
            window = panel.get("time_window", {})
            start, end = _time_ms(window.get("from", "-1h")), _time_ms(window.get("to", "now"))
            if start is None or end is None or start > end:
                errors.append(f"{path}.time_window: invalid or reversed window")
            _validate_expression(panel.get("value_expression", ""), path + ".value_expression", errors)
            _validate_placeholders(panel.get("title", ""), variable_set, path + ".title", errors)
            _validate_placeholders(panel.get("text", {}).get("content", ""), variable_set,
                                   path + ".text.content", errors)
            chart = panel.get("chart", {})
            if chart.get("y_min") is not None and chart.get("y_max") is not None and chart["y_min"] >= chart["y_max"]:
                errors.append(f"{path}.chart: y_min must be below y_max")
            if panel.get("kind") == "image":
                asset_id = panel.get("image", {}).get("asset_id")
                if asset_id not in asset_by_id or asset_by_id[asset_id].get("kind", "image") != "image":
                    errors.append(f"{path}.image.asset_id: unknown image asset")
            if panel.get("kind") == "table":
                keys = [column["key"] for column in panel.get("columns", [])]
                for duplicate in _duplicates(keys):
                    errors.append(f"{path}.columns: duplicate key {duplicate}")
            links = panel.get("linked_panel_ids", [])
            if panel.get("kind") == "card" and panel.get("style", {}).get("variant") in ("dual", "triple"):
                variant = panel["style"]["variant"]
                expected = 1 if variant == "dual" else 2
                same_page = {item["id"]: item for item in page.get("panels", [])}
                series = {(source_id, metric)}
                if len(links) != expected:
                    errors.append(f"{path}.linked_panel_ids: {variant} requires {expected} linked cards")
                for link in links:
                    target = same_page.get(link)
                    if not target or target.get("kind") != "card":
                        errors.append(f"{path}.linked_panel_ids: {link} is not a card on the same page")
                        continue
                    key = (target.get("source_id", ""), target.get("metric", ""))
                    if not all(key) or key in series:
                        errors.append(f"{path}.linked_panel_ids: linked cards need independent source/metric series")
                    if target.get("style", {}).get("variant") in ("dual", "triple"):
                        errors.append(f"{path}.linked_panel_ids: nested multi-metric cards are unsupported")
                    series.add(key)
            actions = panel.get("actions", [])
            for duplicate in _duplicates([item["id"] for item in actions]):
                errors.append(f"{path}.actions: duplicate action ID {duplicate}")
            for action_index, item in enumerate(actions):
                action_path = f"{path}.actions[{action_index}]"
                kind, target = item["kind"], item.get("target", "")
                valid = {"goto_page": target in page_set, "goto_panel": target in panel_set,
                         "set_variable": target in variable_set,
                         "refresh": target in ("all", "current") or target in source_set,
                         "back": not target, "ack_alert": target in ("current", "focused")}.get(kind, False)
                if not valid:
                    errors.append(f"{action_path}.target: invalid {kind} target {target!r}")
                for key in ("label", "target", "value"):
                    _validate_placeholders(item.get(key, ""), variable_set, f"{action_path}.{key}", errors)
            if strict_layout and panel.get("visible", True):
                grid = panel["grid"]
                rect = (grid["x"], grid["y"], grid["x"] + grid["w"], grid["y"] + grid["h"])
                for other_id, other in occupied:
                    if max(rect[0], other[0]) < min(rect[2], other[2]) and max(rect[1], other[1]) < min(rect[3], other[3]):
                        errors.append(f"{path}.grid: overlaps visible panel {other_id}")
                occupied.append((panel["id"], rect))

    for index, rule in enumerate(rules):
        path = f"$.dashboard.alert_rules[{index}]"
        if rule.get("source_id") not in source_set:
            errors.append(f"{path}.source_id: unknown source")
        if rule.get("style", {}).get("linked_panel_id") and rule["style"]["linked_panel_id"] not in panel_set:
            errors.append(f"{path}.style.linked_panel_id: unknown panel")
        _validate_expression(rule.get("expression", ""), path + ".expression", errors)
        _validate_placeholders(rule.get("message", ""), variable_set, path + ".message", errors)
    for index, sample in enumerate(samples):
        if sample.get("source_id") and sample["source_id"] not in source_set:
            errors.append(f"$.dashboard.samples[{index}].source_id: unknown source")
    for index, event in enumerate(events):
        path = f"$.dashboard.events[{index}]"
        if event.get("source_id") and event["source_id"] not in source_set:
            errors.append(f"{path}.source_id: unknown source")
        if event.get("rule_id") and event["rule_id"] not in rule_set:
            errors.append(f"{path}.rule_id: unknown alert rule")
        if event.get("linked_panel_id") and event["linked_panel_id"] not in panel_set:
            errors.append(f"{path}.linked_panel_id: unknown panel")

    source_by_id = {item["id"]: item for item in sources}
    plan_ids = []
    for index, plan in enumerate(plans):
        path = f"$.source_plans[{index}]"
        _validate_plan(plan, path, errors)
        if not isinstance(plan, dict):
            continue
        plan_ids.append(plan.get("id"))
        source = source_by_id.get(plan.get("id"))
        if source is None:
            errors.append(f"{path}.id: no matching dashboard source")
            continue
        normalized = dict(SOURCE_DEFAULTS)
        normalized.update(source)
        normalized["id"] = source["id"]
        for key in ("kind", "enabled", "url", "refresh_ms", "timeout_ms", "max_response_bytes",
                    "tls_verify", "auth_token_ref", "headers"):
            if plan.get(key, SOURCE_DEFAULTS[key]) != normalized[key]:
                errors.append(f"{path}.{key}: must match the dashboard source connection field")
        _validate_placeholders(plan.get("body", ""), variable_set, path + ".body", errors)
    for duplicate in _duplicates(plan_ids):
        errors.append(f"$.source_plans: duplicate source plan {duplicate}")
    return errors


def _scan_secrets(value, path="$", errors=None):
    if errors is None:
        errors = []
    if isinstance(value, dict):
        for key, child in value.items():
            lowered = key.lower()
            if lowered in SECRET_KEYS and isinstance(child, str) and child:
                errors.append(f"{path}.{key}: literal secret fields are forbidden")
            _scan_secrets(child, f"{path}.{key}", errors)
    elif isinstance(value, list):
        for index, child in enumerate(value):
            _scan_secrets(child, f"{path}[{index}]", errors)
    return errors


def check_library(firmware_root: Path):
    preset_root = firmware_root / "presets"
    errors = []
    stats = {"presets": 0, "examples": 0, "max_bundle_bytes": 0, "max_dashboard_bytes": 0}
    try:
        schema = load_json(firmware_root / "components/dashboard_core/schema/eego-dashboard-v2.schema.json")
        catalog = load_json(preset_root / "index.json")
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        return [f"presets: unable to load schema/catalog: {exc}"], stats
    if catalog.get("schema") != CATALOG_SCHEMA or catalog.get("schema_version") != 1:
        errors.append("presets/index.json: expected eego.dashboard-preset-catalog/1")
    entries = catalog.get("presets", [])
    if not isinstance(entries, list) or not 24 <= len(entries) <= 30:
        errors.append("presets/index.json: catalog must contain 24–30 presets")
        entries = []
    listed_ids = [entry.get("id") for entry in entries if isinstance(entry, dict)]
    for duplicate in _duplicates(listed_ids):
        errors.append(f"presets/index.json: duplicate preset ID {duplicate}")
    missing = REQUIRED_PRESETS - set(listed_ids)
    if missing:
        errors.append(f"presets/index.json: missing required presets {sorted(missing)}")
    expected_files = {"index.json"}
    for entry in entries:
        if not isinstance(entry, dict):
            errors.append("presets/index.json: every entry must be an object")
            continue
        preset_id, relative = entry.get("id"), entry.get("file")
        if not ID_RE.fullmatch(preset_id or "") or not isinstance(relative, str):
            errors.append("presets/index.json: invalid id/file entry")
            continue
        target = (preset_root / relative).resolve()
        try:
            target.relative_to(preset_root.resolve())
        except ValueError:
            errors.append(f"presets/index.json: unsafe preset path {relative}")
            continue
        expected_files.add(Path(relative).as_posix())
        try:
            preset = load_json(target)
            if preset.get("id") != preset_id or preset.get("kind") != entry.get("kind"):
                errors.append(f"presets/{relative}: id/kind does not match catalog")
            bundle = bundle_from_preset(preset)
            bundle_errors = validate_bundle(bundle, schema, strict_layout=True)
            bundle_errors += _scan_secrets(bundle)
            errors.extend(f"presets/{relative}{error[1:] if error.startswith('$') else error}"
                          for error in bundle_errors)
            stats["presets"] += 1
            stats["max_bundle_bytes"] = max(stats["max_bundle_bytes"], compact_bytes(bundle))
            stats["max_dashboard_bytes"] = max(stats["max_dashboard_bytes"], compact_bytes(bundle["dashboard"]))
        except (OSError, ValueError, json.JSONDecodeError, KeyError, TypeError) as exc:
            errors.append(f"presets/{relative}: {exc}")
    actual_files = {path.relative_to(preset_root).as_posix()
                    for directory in ("panels", "layouts")
                    for path in (preset_root / directory).rglob("*.json")}
    for unlisted in sorted(actual_files - expected_files):
        errors.append(f"presets/{unlisted}: file is not referenced by index.json")
    examples = sorted((preset_root / "examples").glob("*.json"))
    if len(examples) != 4:
        errors.append(f"presets/examples: expected 4 complete examples, found {len(examples)}")
    for path in examples:
        try:
            raw = path.read_bytes()
            bundle = load_json(path)
            bundle_errors = validate_bundle(bundle, schema, len(raw), strict_layout=True)
            bundle_errors += _scan_secrets(bundle)
            errors.extend(f"presets/examples/{path.name}{error[1:] if error.startswith('$') else error}"
                          for error in bundle_errors)
            stats["examples"] += 1
            stats["max_bundle_bytes"] = max(stats["max_bundle_bytes"], compact_bytes(bundle))
            stats["max_dashboard_bytes"] = max(stats["max_dashboard_bytes"], compact_bytes(bundle["dashboard"]))
        except (OSError, ValueError, json.JSONDecodeError, KeyError, TypeError) as exc:
            errors.append(f"presets/examples/{path.name}: {exc}")
    return errors, stats
