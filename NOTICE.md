# Notices and provenance

This is an independent project. It is not produced, endorsed, or supported by
EEGO, Xteink, CrossLink, or their respective owners. Product and project names
may be trademarks of their owners.

## Source code

The repository contains an ESP-IDF firmware tree and host tooling. The firmware
is GPL-3.0-or-later; see `firmware/LICENSE` and `firmware/NOTICE.md`. Root-level
host tooling and historical hardware research retain the MIT terms in
[`LICENSE`](LICENSE) unless a file states otherwise.

The former Arduino template and its vendored FreeInk subset have been removed.
Their historical contributions to the current firmware, including hardware
facts and interoperability data, remain recorded here and in source headers.

## Binary-analysis inputs

Hardware facts were cross-checked against three user-supplied, unencrypted
ESP32-S3 images. The images themselves are intentionally not included:

| Research input | SHA-256 |
|---|---|
| CrossLink full image | `79c4cdbae8fbb66e8ec586b61ac1209561bee83068627b69fd1f657d970c1483` |
| CrossLink v1.0.10 app image | `43264c93c8e371db6f2e44574027e50d9444dcff40cc6a645828d2558d70cf24` |
| EEGO official v1.2.7 image | `37e121af158cf63ca4483d79d4e66a0537f153c7b0d1c0a02a7acc74753f059c` |

`firmware/components/eego_hal/include/gslx680_firmware.h` and
`firmware/components/eego_hal/include/uc8279c_luts.h` contain binary-derived
interoperability data. The GSL table can be reproduced from a lawful,
user-supplied image with `scripts/extract_eego_a4_gsl_firmware.py`.

The repository does not grant rights to third-party firmware, fonts, product
artwork, or trademarks. Before redistributing binary-derived tables, a
publisher should independently assess the applicable law and the source
material's terms. This notice documents provenance; it is not legal advice.
