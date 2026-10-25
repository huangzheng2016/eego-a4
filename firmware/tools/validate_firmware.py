#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""validate_firmware.py — firmware/ 树静态审计（FW6/M19）。

纯标准库，主机侧运行，不需要 ESP-IDF 环境：

    python3 firmware/tools/validate_firmware.py [--root firmware/] [--json]

检查项（任一 FAIL 退出码为 1）：
  1. required_files  必需文件齐全（构建入口、组件清单、工具、许可文档）
  2. component_deps  组件依赖方向（CMake REQUIRES 分层 + 头文件 include 扫描），
                     落实 M12 定稿：main→{admin,alert_core,diag}→net_stack→
                     {eego_ui,eego_hal}→board；net_stack/admin 不得 include UI 头；
                     alert_core 保持纯 C++（无任何 ESP-IDF/项目头）
  3. forbidden       禁止残留：weread 协议/登录/加密代码、boot_logo 商标素材、
                     miniz、secrets（私钥块/密钥文件名/硬编码凭据赋值）
  4. license_headers 第一方源码必须有 SPDX-License-Identifier；third_party 必须
                     保留上游许可文本或 SPDX
  5. partitions      partitions.csv 布局一致性（A/B 等尺寸、不重叠、16MB 内）
  6. sdkconfig       sdkconfig.defaults 关键键齐全且与 partitions.csv 一致
  7. admin_schema    GET /config/schema 的 JSON 与 C++ 常量静态一致
                     （schema_version/默认值/上限 ↔ kSchemaVersion/AdminDomainConfig/
                     kMaxSources/kMaxTtlSeconds）
  8. dashboard_presets 30 个预设与 4 个完整示例：v2 schema、目录/来源/变量/
                     面板/动作/资源引用、source plan、布局及 128/192/32KiB 预算

本工具只做静态文本/结构检查，不编译、不刷写；不替代设备端导入校验。
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

from dashboard_validation import check_library

# ---------------------------------------------------------------------------
# 审计策略（修改树结构时同步更新本表）
# ---------------------------------------------------------------------------

# M12 定稿依赖方向：component -> 允许依赖的项目组件（不含 ESP-IDF 系统组件）
ALLOWED_DEPS: dict[str, set[str]] = {
    "main": {"board", "eego_hal", "eego_ui", "admin", "alert_core", "net_stack", "diag"},
    "admin": {"alert_core", "net_stack", "eego_hal"},
    "alert_core": set(),
    "diag": set(),
    "net_stack": {"alert_core", "eego_hal"},
    "eego_ui": {"eego_hal", "lodepng"},
    "eego_hal": set(),
    "board": {"eego_hal"},
    "lodepng": set(),
}

# 头文件级禁令：component -> 禁止出现的 include 前缀（CMake 之外的第二道防线）
FORBIDDEN_INCLUDES: dict[str, set[str]] = {
    "alert_core": {"esp_", "freertos", "eego_hal", "eego_ui", "net_stack", "admin/", "board/", "diag/"},
    "net_stack": {"eego_ui", "admin/", "diag/"},
    "admin": {"eego_ui", "diag/"},
    "eego_ui": {"net_stack", "admin/", "alert_core", "diag/"},
    "eego_hal": {"eego_ui", "net_stack", "admin/", "alert_core", "diag/", "board/"},
    "board": {"eego_ui", "net_stack", "admin/", "alert_core", "diag/"},
    "diag": {"eego_ui", "net_stack", "admin/", "alert_core", "eego_hal", "board/"},
}

# weread 协议/登录/加密残留（ M12 P4 清理定稿；来源声明中的 "eego-a4-weread"
# 字样是许可 provenance，不在禁止之列，故只禁具体 artifact 名）
FORBIDDEN_TOKENS: dict[str, str] = {
    "weread_api": "weread 协议 API 残留",
    "weread_client": "weread 会话客户端残留",
    "weread_crypto": "weread_crypto 未复用（M12 定稿）",
    "weread.qq.com": "weread 服务端点残留",
    "weread_token": "weread 令牌残留",
    "weread_cookie": "weread cookie 残留",
    "wr_skey": "weread cookie (wr_skey) 残留",
    "wr_rt": "weread cookie (wr_rt) 残留",
    "wr_vid": "weread cookie (wr_vid) 残留",
    "boot_logo": "boot_logo 商标素材未复用（M12 定稿）",
    "bootlogo": "boot_logo 商标素材未复用（M12 定稿）",
    "boot-logo": "boot_logo 商标素材未复用（M12 定稿）",
    "miniz": "miniz 未采用（M12 定稿）",
}

FORBIDDEN_FILE_PATTERNS = [
    (re.compile(r"weread", re.I), "weread 残留文件"),
    (re.compile(r"boot[_-]?logo", re.I), "boot_logo 商标素材文件"),
    (re.compile(r"miniz", re.I), "miniz 文件（未采用）"),
    (re.compile(r"\.(bmp|png|jpe?g|gif)$", re.I), "二进制图片素材（boot_logo/商标风险，禁止入库）"),
    (re.compile(r"\.env$", re.I), "secrets 文件"),
    (re.compile(r"\.(pem|key|p12|pfx)$", re.I), "密钥文件"),
    (re.compile(r"id_rsa", re.I), "私钥文件"),
]

# 内容扫描豁免：本策略文件自身含检测 token 定义；NOTICE/README 等审计声明文件
# 必须点名 weread_crypto/boot_logo/miniz 以记录「未复用/未采用」，属声明而非残留。
CONTENT_SCAN_EXCLUDE = {
    "tools/validate_firmware.py",
    "README.md",
    "LICENSE",
    "NOTICE.md",
    "THIRD_PARTY_NOTICES.md",
}

# 硬编码凭据赋值：name = "literal" / name: "literal"（值 ≥8 字符且非掩码/占位）
SECRET_ASSIGN_RE = re.compile(
    r"""(?ix)
    \b(password|passwd|secret|token|api[_-]?key|api[_-]?token|auth[_-]?token|ingest[_-]?key)\b
    \s*[:=]\s*
    "([^"\n]{8,})"
    """
)
SECRET_ASSIGN_ALLOW_SUBSTR = ("********", "${", "placeholder", "example", "your_")

REQUIRED_FILES = [
    "CMakeLists.txt",
    "sdkconfig.defaults",
    "partitions.csv",
    "main/CMakeLists.txt",
    "main/app_main.cpp",
    "tools/validate_firmware.py",
    "tools/mock_alert_server.py",
    "tools/smoke_serial_test.py",
    "README.md",
    "LICENSE",
    "NOTICE.md",
    "THIRD_PARTY_NOTICES.md",
]

CODE_SUFFIXES = {".cpp", ".h", ".py", ".csv", ".txt", ".cmake"}
FIRST_PARTY_NO_SPDX_ALLOW = {
    # Kconfig 导入格式，继承自已验证的阅读器固件经验；行内注释已说明来源。
    # 保持与上游 weread sdkconfig.defaults 的可 diff 性，不加 SPDX 行。
    "sdkconfig.defaults",
}

FLASH_SIZE_16MB = 0x1000000


# ---------------------------------------------------------------------------
# 报告
# ---------------------------------------------------------------------------

class Report:
    def __init__(self) -> None:
        self.rows: list[tuple[str, str, str]] = []  # (level, check, detail)
        self.pass_n = 0
        self.warn_n = 0
        self.fail_n = 0

    def ok(self, check: str, detail: str = "") -> None:
        self.rows.append(("PASS", check, detail))
        self.pass_n += 1

    def warn(self, check: str, detail: str) -> None:
        self.rows.append(("WARN", check, detail))
        self.warn_n += 1

    def fail(self, check: str, detail: str) -> None:
        self.rows.append(("FAIL", check, detail))
        self.fail_n += 1

    def check(self, cond: bool, name: str, ok_detail: str = "", fail_detail: str = "") -> bool:
        if cond:
            self.ok(name, ok_detail)
        else:
            self.fail(name, fail_detail or ok_detail)
        return cond

    def print(self, as_json: bool = False) -> None:
        if as_json:
            print(json.dumps({
                "pass": self.pass_n, "warn": self.warn_n, "fail": self.fail_n,
                "rows": [{"level": l, "check": c, "detail": d} for l, c, d in self.rows],
            }, ensure_ascii=False, indent=2))
            return
        for level, check, detail in self.rows:
            line = f"[{level}] {check}"
            if detail:
                line += f" — {detail}"
            print(line)
        print(f"\nsummary: pass={self.pass_n} warn={self.warn_n} fail={self.fail_n}")


# ---------------------------------------------------------------------------
# 工具函数
# ---------------------------------------------------------------------------

def read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        return f"<< unreadable: {exc} >>"


def list_code_files(root: Path) -> list[Path]:
    files = []
    for p in sorted(root.rglob("*")):
        if not p.is_file():
            continue
        if p.suffix.lower() in CODE_SUFFIXES or p.name == "CMakeLists.txt":
            files.append(p)
    return files


def component_sources(root: Path, comp: str) -> list[Path]:
    if comp == "main":
        base = root / "main"
    elif comp == "lodepng":
        base = root / "components" / "third_party" / "lodepng"
    else:
        base = root / "components" / comp
    if not base.is_dir():
        return []
    return [p for p in sorted(base.rglob("*"))
            if p.is_file() and p.suffix.lower() in {".cpp", ".h"}]


# ---------------------------------------------------------------------------
# 检查 1：必需文件
# ---------------------------------------------------------------------------

def check_required_files(root: Path, rep: Report) -> None:
    missing = [rel for rel in REQUIRED_FILES if not (root / rel).is_file()]
    rep.check(not missing, "required_files",
              f"{len(REQUIRED_FILES)} 个必需文件齐全",
              f"缺失: {', '.join(missing)}")

    comp_dirs = sorted(p for p in (root / "components").iterdir()
                       if p.is_dir() and (p / "CMakeLists.txt").is_file())
    third_party = sorted(p for p in (root / "components" / "third_party").iterdir()
                         if p.is_dir() and (p / "CMakeLists.txt").is_file())
    for d in comp_dirs + third_party:
        rep.check(True, "required_files", f"组件 {d.name} 含 CMakeLists.txt")
    rep.check(len(comp_dirs) >= 6, "required_files",
              f"{len(comp_dirs)} 个第一方组件 + {len(third_party)} 个 third_party 组件")


# ---------------------------------------------------------------------------
# 检查 2：组件依赖方向
# ---------------------------------------------------------------------------

CMAKE_KEYWORDS = {"SRCS", "INCLUDE_DIRS", "REQUIRES", "PRIV_REQUIRES", "EMBED_FILES",
                  "EMBED_TXTFILES", "idf_component_register"}


def parse_cmake_requires(path: Path) -> set[str]:
    """从 idf_component_register 块提取 REQUIRES/PRIV_REQUIRES 的项目组件名。"""
    text = read_text(path)
    m = re.search(r"idf_component_register\s*\((.*?)\)", text, re.S)
    if not m:
        return set()
    tokens = re.findall(r"[A-Za-z0-9_/.\-]+", m.group(1))
    deps: set[str] = set()
    collect = False
    for tok in tokens:
        if tok in ("REQUIRES", "PRIV_REQUIRES"):
            collect = True
            continue
        if tok in CMAKE_KEYWORDS:
            collect = False
            continue
        if collect:
            deps.add(tok)
    return deps


def check_component_deps(root: Path, rep: Report) -> None:
    comps = sorted(ALLOWED_DEPS.keys())
    for comp in comps:
        cmakelists = root / "main" / "CMakeLists.txt" if comp == "main" else (
            root / "components" / "third_party" / comp / "CMakeLists.txt"
            if comp == "lodepng" else root / "components" / comp / "CMakeLists.txt")
        if not cmakelists.is_file():
            rep.fail("component_deps", f"{comp}: 缺 CMakeLists.txt")
            continue
        deps = parse_cmake_requires(cmakelists)
        project_deps = {d for d in deps if d in ALLOWED_DEPS and d != comp}
        unknown_local = project_deps - ALLOWED_DEPS[comp]
        if unknown_local:
            rep.fail("component_deps",
                     f"{comp}: 越层依赖 {sorted(unknown_local)}（允许: {sorted(ALLOWED_DEPS[comp]) or '无'}）")
        else:
            rep.ok("component_deps", f"{comp}: REQUIRES {sorted(project_deps) or '∅'} 符合分层")

    # 头文件 include 扫描（CMake 之外的实际包含关系）
    include_re = re.compile(r'^\s*#\s*include\s+["<]([^">]+)[">]', re.M)
    for comp, forbidden in FORBIDDEN_INCLUDES.items():
        bad: list[str] = []
        for src in component_sources(root, comp):
            for inc in include_re.findall(read_text(src)):
                for prefix in forbidden:
                    if inc.startswith(prefix) or inc == prefix.rstrip("/"):
                        bad.append(f"{src.relative_to(root)}: {inc}")
        if bad:
            rep.fail("component_deps", f"{comp}: 越层 include {bad[:4]}")
        else:
            rep.ok("component_deps", f"{comp}: include 无越层")


# ---------------------------------------------------------------------------
# 检查 3：禁止残留（weread/商标/miniz/secrets）
# ---------------------------------------------------------------------------

def check_forbidden(root: Path, rep: Report) -> None:
    hits: list[str] = []
    for p in sorted(root.rglob("*")):
        if not p.is_file():
            continue
        rel = str(p.relative_to(root))
        for pat, why in FORBIDDEN_FILE_PATTERNS:
            if pat.search(p.name):
                hits.append(f"{rel}（{why}）")
    rep.check(not hits, "forbidden_files", "无 weread/boot_logo/miniz/密钥文件残留",
              "; ".join(hits[:8]))

    token_hits: list[str] = []
    for p in list_code_files(root) + sorted(root.rglob("*.md")):
        rel = str(p.relative_to(root))
        if rel in CONTENT_SCAN_EXCLUDE:
            continue
        text = read_text(p)
        low = text.lower()
        for token, why in FORBIDDEN_TOKENS.items():
            if token in low:
                token_hits.append(f"{rel}: {why}")
    rep.check(not token_hits, "forbidden_tokens",
              "无 weread 协议/boot_logo/miniz 内容残留",
              "; ".join(token_hits[:8]))

    secret_hits: list[str] = []
    for p in list_code_files(root):
        rel = str(p.relative_to(root))
        if rel in CONTENT_SCAN_EXCLUDE:
            continue
        text = read_text(p)
        if "PRIVATE KEY-----" in text:
            secret_hits.append(f"{rel}: 私钥块")
        for m in SECRET_ASSIGN_RE.finditer(text):
            value = m.group(2)
            if any(s in value for s in SECRET_ASSIGN_ALLOW_SUBSTR):
                continue
            line_no = text.count("\n", 0, m.start()) + 1
            secret_hits.append(f"{rel}:{line_no}: 疑似硬编码 {m.group(1)}")
    rep.check(not secret_hits, "forbidden_secrets", "无私钥/硬编码凭据",
              "; ".join(secret_hits[:8]))


# ---------------------------------------------------------------------------
# 检查 4：许可头
# ---------------------------------------------------------------------------

def check_license_headers(root: Path, rep: Report) -> None:
    missing: list[str] = []
    third_party_bad: list[str] = []
    for p in list_code_files(root):
        rel = str(p.relative_to(root))
        if rel in FIRST_PARTY_NO_SPDX_ALLOW:
            continue
        head = "\n".join(read_text(p).splitlines()[:10])
        has_spdx = "SPDX-License-Identifier" in head
        if rel.startswith("components/third_party/"):
            if not has_spdx and "Copyright" not in head:
                third_party_bad.append(rel)
        elif not has_spdx:
            missing.append(rel)
    if missing:
        rep.fail("license_headers",
                 f"{len(missing)} 个第一方文件缺 SPDX 头: {missing[0]} 等"
                 + ("（alert_core 为 M15 遗留缺口，已另行上报）" if any("alert_core" in m for m in missing) else ""))
    else:
        rep.ok("license_headers", "第一方源码均含 SPDX-License-Identifier")
    rep.check(not third_party_bad, "license_headers",
              "third_party 保留上游许可文本",
              f"缺上游许可引用: {third_party_bad[:4]}")


# ---------------------------------------------------------------------------
# 检查 5：partitions.csv
# ---------------------------------------------------------------------------

def check_partitions(root: Path, rep: Report) -> None:
    path = root / "partitions.csv"
    if not path.is_file():
        rep.fail("partitions", "partitions.csv 缺失")
        return
    parts: list[dict[str, str]] = []
    for line in read_text(path).splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        cols = [c.strip() for c in line.split(",")]
        if len(cols) < 5:
            continue
        parts.append({"name": cols[0], "type": cols[1], "sub": cols[2],
                      "off": cols[3], "size": cols[4]})
    names = {p["name"] for p in parts}
    required = {"nvs", "otadata", "app0", "app1"}
    rep.check(required <= names, "partitions",
              "nvs/otadata/app0/app1 齐全",
              f"缺分区: {sorted(required - names)}")

    try:
        rows = sorted(
            (int(p["off"], 16), int(p["size"], 16), p["name"]) for p in parts)
    except ValueError:
        rep.fail("partitions", "offset/size 非十六进制")
        return
    overlap = []
    for (off, size, name), (noff, _, _) in zip(rows, rows[1:]):
        if off + size > noff:
            overlap.append(f"{name}@0x{off:x}")
    rep.check(not overlap, "partitions", "分区无重叠", f"重叠: {overlap}")
    end = rows[-1][0] + rows[-1][1]
    rep.check(end <= FLASH_SIZE_16MB, "partitions",
              f"布局止于 0x{end:x} ≤ 16MB",
              f"布局越界 0x{end:x} > 16MB")
    ota = {p["name"]: int(p["size"], 16) for p in parts if p["sub"].startswith("ota_")}
    rep.check(len(ota) == 2 and len(set(ota.values())) == 1, "partitions",
              f"A/B 等尺寸 {[hex(s) for s in ota.values()]}",
              f"A/B 分区缺失或尺寸不等: {ota}")


# ---------------------------------------------------------------------------
# 检查 6：sdkconfig.defaults
# ---------------------------------------------------------------------------

SDKCONFIG_REQUIRED = {
    "CONFIG_IDF_TARGET": '"esp32s3"',
    "CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG": "y",
    "CONFIG_PARTITION_TABLE_CUSTOM": "y",
    "CONFIG_PARTITION_TABLE_CUSTOM_FILENAME": '"partitions.csv"',
    "CONFIG_ESPTOOLPY_FLASHSIZE_16MB": "y",
    "CONFIG_SPIRAM": "y",
    "CONFIG_SPIRAM_MODE_OCT": "y",
    "CONFIG_SPIRAM_SPEED_80M": "y",
    "CONFIG_SPIRAM_USE_CAPS_ALLOC": "y",
    "CONFIG_ESP_MAIN_TASK_STACK_SIZE": "32768",
    "CONFIG_LWIP_TCPIP_TASK_STACK_SIZE": "8192",
    "CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE": "8192",
    "CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE": "y",
    "CONFIG_FATFS_LFN_HEAP": "y",
    "CONFIG_ESP_HTTP_CLIENT_ENABLE_HTTPS": "y",
    "CONFIG_MBEDTLS_CERTIFICATE_BUNDLE": "y",
}


def check_sdkconfig(root: Path, rep: Report) -> None:
    path = root / "sdkconfig.defaults"
    if not path.is_file():
        rep.fail("sdkconfig", "sdkconfig.defaults 缺失")
        return
    kv: dict[str, str] = {}
    for line in read_text(path).splitlines():
        line = line.strip()
        m = re.match(r"^(CONFIG_[A-Z0-9_]+)=(.*)$", line)
        if m:
            kv[m.group(1)] = m.group(2)
    bad = [f"{k}={v}（实际 {kv.get(k, '缺失')}）"
           for k, v in SDKCONFIG_REQUIRED.items() if kv.get(k) != v]
    rep.check(not bad, "sdkconfig",
              f"{len(SDKCONFIG_REQUIRED)} 个关键键齐全且匹配",
              "; ".join(bad[:6]))
    custom = kv.get("CONFIG_PARTITION_TABLE_CUSTOM_FILENAME", "").strip('"')
    rep.check(bool(custom) and (root / custom).is_file(), "sdkconfig",
              f"自定义分区表 {custom} 存在",
              f"分区表文件 {custom} 缺失")


# ---------------------------------------------------------------------------
# 检查 7：admin 配置 schema 静态一致性
# ---------------------------------------------------------------------------

def extract_schema_json(admin_config_cpp: str) -> dict | None:
    m = re.search(r'R"JSON\((.*?)\)JSON"', admin_config_cpp, re.S)
    if not m:
        return None
    try:
        return json.loads(m.group(1))
    except json.JSONDecodeError:
        return None


def extract_const_int(text: str, name: str) -> int | None:
    m = re.search(rf"constexpr\s+\w+\s+{re.escape(name)}\s*=\s*([0-9\s*+]+);", text)
    if not m:
        return None
    try:
        return eval(m.group(1), {"__builtins__": {}}, {})  # 仅数字与运算符
    except Exception:
        return None


def check_admin_schema(root: Path, rep: Report) -> None:
    cpp = root / "components" / "admin" / "src" / "admin_config.cpp"
    hdr = root / "components" / "admin" / "include" / "admin" / "admin_config.h"
    types_h = root / "components" / "alert_core" / "include" / "alert_core" / "types.h"
    if not (cpp.is_file() and hdr.is_file() and types_h.is_file()):
        rep.fail("admin_schema", "admin_config.cpp/.h 或 types.h 缺失")
        return
    cpp_text, hdr_text, types_text = read_text(cpp), read_text(hdr), read_text(types_h)

    schema = extract_schema_json(cpp_text)
    if schema is None:
        rep.fail("admin_schema", "schemaJson() 不是合法 JSON")
        return
    rep.ok("admin_schema", "schemaJson() 解析成功")

    top = set(schema.get("top_level", {}).keys())
    rep.check(top == {"schema_version", "admin", "sources", "alert_type_overrides", "mute_rules"},
              "admin_schema", "顶层键与 M13 §3 + M21 mute_rules 一致",
              f"顶层键异常: {sorted(top)}")

    k_ver = extract_const_int(cpp_text, "kSchemaVersion")
    s_ver = schema["top_level"]["schema_version"].get("const")
    rep.check(k_ver is not None and k_ver == s_ver, "admin_schema",
              f"schema_version const={s_ver} == kSchemaVersion",
              f"schema const={s_ver} vs kSchemaVersion={k_ver}")

    k_sources = extract_const_int(cpp_text, "kMaxSources")
    s_sources = schema["top_level"]["sources"].get("max_items")
    rep.check(k_sources == s_sources, "admin_schema",
              f"sources.max_items={s_sources} == kMaxSources",
              f"sources.max_items={s_sources} vs kMaxSources={k_sources}")

    k_overrides = extract_const_int(cpp_text, "kMaxOverrides")
    s_overrides = schema["top_level"]["alert_type_overrides"].get("max_entries")
    rep.check(k_overrides == s_overrides, "admin_schema",
              f"overrides.max_entries={s_overrides} == kMaxOverrides",
              f"overrides.max_entries={s_overrides} vs kMaxOverrides={k_overrides}")

    k_ttl = extract_const_int(types_text, "kMaxTtlSeconds")
    ttl_txt = schema["top_level"]["alert_type_overrides"]["value"]["ttl_s"]
    m_ttl = re.search(r"0\.\.(\d+)", ttl_txt)
    s_ttl = int(m_ttl.group(1)) if m_ttl else None
    rep.check(k_ttl is not None and k_ttl == s_ttl, "admin_schema",
              f"ttl_s 上限={s_ttl} == kMaxTtlSeconds",
              f"ttl_s 上限={s_ttl} vs kMaxTtlSeconds={k_ttl}")

    # AdminDomainConfig 默认值 ↔ schema defaults
    fields = schema["top_level"]["admin"]["fields"]
    expect = [
        ("network", "auto_ap_fallback", r"auto_ap_fallback\s*=\s*(\w+)", "false"),
        ("security", "session_ttl_s", r"session_ttl_s\s*=\s*(\d+)", "86400"),
        ("security", "login_lock_threshold", r"login_lock_threshold\s*=\s*(\d+)", "5"),
        ("display", "brightness", r"brightness\s*=\s*(-?\d+)", "0"),
        ("display", "warmth", r"warmth\s*=\s*(-?\d+)", "50"),
        ("logging", "sd_log", r"sd_log\s*=\s*(\w+)", "false"),
    ]
    bad: list[str] = []
    for section, key, pattern, _ in expect:
        m = re.search(pattern, hdr_text)
        actual = m.group(1) if m else None
        declared = fields[section]["fields"][key].get("default")
        if actual is None or str(declared).lower() != actual.lower():
            bad.append(f"admin.{section}.{key}: schema={declared} vs 头文件={actual}")
    m = re.search(r'log_level\s*=\s*"(\w+)"', hdr_text)
    lvl = fields["logging"]["fields"]["level"]
    if not m or m.group(1) != lvl.get("default"):
        bad.append(f"admin.logging.level: schema={lvl.get('default')} vs 头文件={m.group(1) if m else '缺失'}")
    rep.check(not bad, "admin_schema",
              "AdminDomainConfig 默认值与 schema 一致",
              "; ".join(bad))

    ranges = [
        ("security", "session_ttl_s", (300, 86400)),
        ("security", "login_lock_threshold", (3, 10)),
        ("display", "brightness", (0, 100)),
        ("display", "warmth", (0, 100)),
    ]
    bad_r = [f"{s}.{k} range={fields[s]['fields'][k].get('range')} != {r}"
             for s, k, r in ranges if tuple(fields[s]["fields"][k].get("range", ())) != r]
    rep.check(not bad_r, "admin_schema", "schema 范围与 M13 §3 一致", "; ".join(bad_r))


# ---------------------------------------------------------------------------
# 检查 8：dashboard 预设库、完整示例、source plan 与资源预算
# ---------------------------------------------------------------------------

def check_dashboard_library(root: Path, rep: Report) -> None:
    errors, stats = check_library(root)
    rep.check(not errors, "dashboard_presets",
              f"{stats['presets']} 个预设 + {stats['examples']} 个完整示例；"
              f"最大 bundle={stats['max_bundle_bytes']}B dashboard={stats['max_dashboard_bytes']}B",
              f"{len(errors)} 个错误: " + " | ".join(errors[:8]))


# ---------------------------------------------------------------------------
# 主流程
# ---------------------------------------------------------------------------

def main() -> int:
    ap = argparse.ArgumentParser(description="firmware/ 树静态审计（依赖/许可/dashboard/schema/预算）")
    ap.add_argument("--root", default=None,
                    help="firmware/ 目录（默认取本脚本所在目录的上一级）")
    ap.add_argument("--json", action="store_true", help="JSON 输出")
    args = ap.parse_args()

    root = Path(args.root) if args.root else Path(__file__).resolve().parent.parent
    root = root.resolve()
    if not (root / "CMakeLists.txt").is_file():
        print(f"error: {root} 不像 firmware/ 目录（缺 CMakeLists.txt）", file=sys.stderr)
        return 2

    rep = Report()
    check_required_files(root, rep)
    check_component_deps(root, rep)
    check_forbidden(root, rep)
    check_license_headers(root, rep)
    check_partitions(root, rep)
    check_sdkconfig(root, rep)
    check_admin_schema(root, rep)
    check_dashboard_library(root, rep)
    rep.print(args.json)
    return 1 if rep.fail_n else 0


if __name__ == "__main__":
    sys.exit(main())
