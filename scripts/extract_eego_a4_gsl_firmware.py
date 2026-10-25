#!/usr/bin/env python3
"""Extract the EEGO A4 GSLX680 table from a user-supplied ESP app image."""

from __future__ import annotations

import argparse
import hashlib
import struct
from pathlib import Path


TABLE_ADDRESS = 0x3C2CE71C
TABLE_LENGTH = 36_696
EXPECTED_IMAGE_SHA256 = (
    "43264c93c8e371db6f2e44574027e50d9444dcff40cc6a645828d2558d70cf24"
)
EXPECTED_TABLE_SHA256 = (
    "076ac8cb283d055660f79f7ea0e10a33533563065d6866c095ad3e74537f3051"
)


def read_mapped_range(image: bytes, address: int, length: int) -> bytes:
    if len(image) < 24 or image[0] != 0xE9:
        raise ValueError("not an ESP application image (missing 0xe9 header)")
    segment_count = image[1]
    offset = 24
    for _ in range(segment_count):
        if offset + 8 > len(image):
            raise ValueError("truncated ESP segment header")
        load_address, segment_length = struct.unpack_from("<II", image, offset)
        data_offset = offset + 8
        if load_address <= address and address + length <= load_address + segment_length:
            start = data_offset + address - load_address
            return image[start : start + length]
        offset = data_offset + segment_length
    raise ValueError(f"DROM range 0x{address:08x}+{length} was not found")


def render_header(
    records: list[tuple[int, int]], image_sha: str, table_sha: str
) -> str:
    lines = [
        "// SPDX-License-Identifier: GPL-3.0-or-later",
        "// 移植自 eego-a4-weread（GPL-3.0）同名文件；原始硬件事实来自 eego-a4-template-firmware（MIT）。",
        "#pragma once",
        "",
        "#include <stdint.h>",
        "#include <stddef.h>",
        "",
        "// GSLX680 触控固件表（EEGO A4）",
        "// 提取自 CrossLink EEGO A4 v1.0.10 ESP32-S3 应用镜像。",
        "// 每条记录为 {页内寄存器偏移 offset, 32 位写入值 value}；",
        "// offset == 0xf0 表示页选择寄存器，其余 offset 为当前页内的 32 位寄存器地址。",
        "// 上传协议：写 0xf0 选页，随后将连续寄存器（offset 递增 4）批量写入。",
        f"// Source image SHA-256: {image_sha}",
        f"// Raw table SHA-256:   {table_sha}",
        "",
        "namespace eego {",
        "",
        "struct Gslx680FirmwareEntry {",
        "  uint8_t offset;",
        "  uint32_t value;",
        "};",
        "",
        "constexpr Gslx680FirmwareEntry GSLX680_FIRMWARE[] = {",
    ]
    for index in range(0, len(records), 3):
        group = records[index : index + 3]
        rendered = ", ".join(
            f"{{0x{register:02x}, 0x{value:08x}}}" for register, value in group
        )
        lines.append(f"    {rendered},")
    lines.extend(
        [
            "};",
            "",
            "constexpr size_t GSLX680_FIRMWARE_COUNT =",
            "    sizeof(GSLX680_FIRMWARE) / sizeof(GSLX680_FIRMWARE[0]);",
            "",
            "}  // namespace eego",
            "",
        ]
    )
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Reproduce the checked-in GSLX680 table from the known CrossLink "
            "EEGO A4 v1.0.10 app image."
        )
    )
    parser.add_argument("image", type=Path, help="user-supplied app-only .bin")
    parser.add_argument("output", type=Path, help="generated C++ header")
    args = parser.parse_args()

    image = args.image.read_bytes()
    image_sha = hashlib.sha256(image).hexdigest()
    if image_sha != EXPECTED_IMAGE_SHA256:
        raise ValueError(
            f"unexpected source image SHA-256 {image_sha}; "
            f"expected {EXPECTED_IMAGE_SHA256}"
        )

    table = read_mapped_range(image, TABLE_ADDRESS, TABLE_LENGTH)
    table_sha = hashlib.sha256(table).hexdigest()
    if table_sha != EXPECTED_TABLE_SHA256:
        raise ValueError(
            f"unexpected GSL table SHA-256 {table_sha}; "
            f"expected {EXPECTED_TABLE_SHA256}"
        )

    records: list[tuple[int, int]] = []
    for offset in range(0, len(table), 8):
        register, pad0, pad1, pad2, value = struct.unpack_from(
            "<BBBBI", table, offset
        )
        if pad0 or pad1 or pad2:
            raise ValueError(f"non-zero record padding at table offset 0x{offset:x}")
        records.append((register, value))
    if len(records) != 4_587 or records[0] != (0xF0, 2):
        raise ValueError("GSL table structure did not match the recovered A4 image")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        render_header(records, image_sha, table_sha), encoding="utf-8"
    )
    print(f"wrote {len(records)} records to {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
