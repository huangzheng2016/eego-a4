#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exact host-libc strtod semantics used by dashboard numeric parsers."""

from __future__ import annotations

import ctypes
import math

_libc = ctypes.CDLL(None, use_errno=True)
_strtod = _libc.strtod
_strtod.restype = ctypes.c_double
_strtod.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_void_p)]


def parse_finite_strtod(text):
    """Return (accepted, value), including full-consumption and errno checks."""
    try:
        raw = text.encode("ascii")
    except UnicodeEncodeError:
        return False, 0.0
    if not raw or b"\x00" in raw:
        return False, 0.0
    buffer = ctypes.create_string_buffer(raw)
    end = ctypes.c_void_p()
    ctypes.set_errno(0)
    value = _strtod(buffer, ctypes.byref(end))
    error = ctypes.get_errno()
    start_address = ctypes.addressof(buffer)
    consumed = (end.value or start_address) - start_address
    accepted = error == 0 and consumed == len(raw) and math.isfinite(value)
    return accepted, value
