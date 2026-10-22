# EEGO A4 Configurable Alert and Data Panel

[中文](README.md) · [Full firmware guide](firmware/README.md) ·
[English integrated guide](firmware/README.md#english-integrated-guide) ·
[Documentation map](docs/README.md)

The primary product in this repository is the **ESP-IDF configurable alert/data-panel
firmware** under `firmware/`, for the EEGO A4 (ESP32-S3 + 7.8-inch UC8279C e-paper).
It integrates HTTP sources, declarative parsing, variables, panel layouts, alert
styles, independent refresh, touch/custom input, WiFi/BLE, frontlight, storage,
diagnostics, sleep, and recovery. It has no Arduino dependency and does not execute
arbitrary scripts.

The legacy Arduino diagnostic template remains available as a **hardware reference
and driver-development baseline**, not as the current product. Its examples and
physical-device results must not be transferred to the new ESP-IDF firmware.

## Quick start

Install Python 3 and ESP-IDF 5.5.1, then run host checks and a complete build:

```sh
python3 firmware/tools/validate_firmware.py
python3 firmware/tools/preset_tool.py validate-library
idf.py -C firmware -B .tmp/firmware-build build
idf.py -C firmware -B .tmp/firmware-build size
idf.py -C firmware -B .tmp/firmware-build size-components
```

Flashing is USB-only. Make a full backup and verify partitions and image hashes first;
see the [recovery reference](docs/RECOVERY.md) and [firmware guide](firmware/README.md).
Remote OTA is not included in this release.

First boot and configuration:

1. With no admin password, join `EEGO-A4-Setup` and open `http://192.168.4.1/`.
2. Select WiFi and set an admin password of at least eight characters. The device
   saves only after the STA connection succeeds, then reboots.
3. Open `http://<device-ip>/`, sign in, and use Dashboard → import → validate → apply.
4. To start from a preset, expand it into the device-native bundle first:

   ```sh
   python3 firmware/tools/preset_tool.py list
   python3 firmware/tools/preset_tool.py export chart-kpi \
     --mock-host 192.168.1.20:8978 --output .tmp/chart-kpi.json
   python3 firmware/tools/preset_tool.py validate .tmp/chart-kpi.json
   ```

See the [preset guide](firmware/presets/README.md) for 30 starting points and complete
TOKEN-usage, weather, server-metrics, and security-event scenarios. On a physical
device, `mock_host` must be the mock computer's LAN address, not `127.0.0.1`.

## Product capabilities

- **Dashboard:** card, chart, table, text, image, button, and alert panels; standard
  JSON, bounded JSONPath, Prometheus, text, and PNG/JPEG; variables, thresholds, alert
  surfaces, linked panels, time windows, and independent source/panel schedules.
- **HTTP configuration:** bilingual UI, Bearer login, unsaved-draft protection, schema
  validation, import/export, revision conflicts, normalized source tests, and atomic
  apply. Complex transformation belongs in your server-side scripts, not on the device.
- **Input:** tap/swipe, Up/Down/Front gestures, fixed Power/Home safety slots, and up
  to 24 sparse custom bindings. Dangerous actions require a fresh hold of the same
  control for at least 700 ms within five seconds; recovery and BLE actions require
  physical Power.
- **Whole-device settings:** language, WiFi, power, frontlight, data/EPD refresh,
  RTC/timezone, SD, diagnostics, network recovery, and factory reset. Local settings
  and the typed DeviceSettings API share validation and live application.
- **BLE:** local PoP/Security 1 provisioning after initialization and an opt-in passive
  manufacturer/service observer with bounded scan windows. Persistent phone GATT,
  Bluetooth Classic, audio, and generic Bluetooth networking are out of scope.

See the [complete English integrated guide](firmware/README.md#english-integrated-guide)
for fields, APIs, interaction details, limitations, and recovery procedures.

## Current limits and validation status

- No remote OTA in this release. HTTP and BLE are unreachable during deep sleep;
  battery mode serves requests only during wake windows.
- The runtime resolves NVS `auth_token_ref` values, but there is no public UI/API/serial
  workflow for writing an arbitrary source token into NVS. The Bearer provisioning chain
  is therefore incomplete and not accepted.
- Ordinary custom headers are for non-secret values only. Sensitive headers are masked
  in API responses but currently persisted verbatim to SD; do not use them for secrets.
  Servers should return the minimum required data and never unnecessary credentials.
- Host/static/mock/native-C++/DOM checks and the full ESP-IDF build pass on the final
  baseline, including all 30 presets and four scenarios. **These are not physical-device
  acceptance.** Display, touch/electrical behavior, live device networking, BLE/RF
  coexistence, power consumption, and long-term stability still require the categorized
  [physical acceptance checklist](firmware/README.md#真机验收清单当前全部未执行), tied to
  the exact flashed image hash.

## Documentation

Start with:

1. [English integrated firmware guide](firmware/README.md#english-integrated-guide)
2. [完整中文固件指南](firmware/README.md)
3. [Preset catalog and examples](firmware/presets/README.md)
4. [Repository documentation map](docs/README.md)

Project governance: [contributing](CONTRIBUTING.md), [security](SECURITY.md),
[support](SUPPORT.md), [changelog](CHANGELOG.md), and
[third-party notices](THIRD_PARTY_NOTICES.md).

## Legacy Arduino hardware reference (not the primary product)

`src/`, `lib/`, `examples/`, and most files under `docs/` belong to the legacy
Arduino/PlatformIO hardware template. Use them for pinout, buses, drivers, display safe
areas, diagnostics, and safe-recovery reference only. They do not describe the new
firmware architecture or validate it on hardware. The legacy template has its own
version, 1.0.18; do not confuse its images or version with the ESP-IDF product.

- [Hardware reference](docs/HARDWARE_REFERENCE.md)
- [Legacy drivers and API](docs/DRIVER_GUIDE.md)
- [Legacy architecture and bus ownership](docs/ARCHITECTURE.md)
- [Legacy examples](examples/README.md)
- [Legacy diagnostics](docs/DIAGNOSTICS_GUIDE.md)
- [Legacy physical-validation scope](docs/VALIDATION.md)
- [Display safe area](docs/SAFE_AREA.md)
- [Backup, flashing, and recovery reference](docs/RECOVERY.md)

Run the legacy template's full regression, which also builds its diagnostic firmware
and five examples:

```sh
python3 scripts/validate_project.py
```

Run only repository structure, text, and Markdown-link checks:

```sh
python3 scripts/validate_project.py --quick
```

## Repository layout

```text
firmware/                 Primary ESP-IDF alert/data-panel product
  components/             Dashboard, pipeline, admin, BLE, settings, hardware
  main/                   Application, runtime, input, and recovery composition
  presets/                30 host presets and four complete bundle examples
  tools/                  Validation, preset, mock, and serial-smoke tools
src/  lib/  examples/     Legacy Arduino hardware template (reference)
docs/                     Product navigation and legacy hardware/recovery material
scripts/                  Repository checks and legacy packaging/safety tools
```

`.tmp/`, `.pio/`, `release/`, and `captures/` are generated directories and must not be
committed. Build, validation, and packaging commands never flash a device automatically.
Distribute release binaries as Release assets rather than source-tree artifacts.

## Licensing and provenance

The legacy Arduino template is under the root [MIT License](LICENSE). The new
`firmware/` product is a [GPL-3.0-or-later](firmware/LICENSE) derivative. See
[NOTICE.md](NOTICE.md), [firmware NOTICE](firmware/NOTICE.md), and the respective
third-party notices for provenance, trademarks, extracted data, and licensing boundaries.
The project is not affiliated with or endorsed by EEGO, Xteink, CrossLink, or their owners.
