# Third-party notices

Current firmware build and runtime dependencies are listed in
[`firmware/THIRD_PARTY_NOTICES.md`](firmware/THIRD_PARTY_NOTICES.md), including
ESP-IDF 5.5.1, Mbed TLS and the vendored LodePNG copy. Their complete license
texts remain with the corresponding source or toolchain artifacts.

The optional host serial tool uses pyserial 3.5 (BSD-3-Clause):
<https://github.com/pyserial/pyserial>. It is installed only for host diagnostics
and is not linked into the firmware.

The former Arduino/PlatformIO dependencies were removed with the legacy template
and are not current build dependencies. Historical source and binary-data
provenance is documented in [`NOTICE.md`](NOTICE.md).
