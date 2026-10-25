// SPDX-License-Identifier: GPL-3.0-or-later
// Dashboard render harness: renders every page/screen of every fixture
// dashboard (see make_fixtures.py) through the real dashboard_ui renderer into
// PNGs. Two views per screen:
//   <out>/<font>/<name>__<page>_<screen>.png       full refresh (4-level gray)
//   <out>/<font>/fast/<name>__<page>_<screen>.png  fast refresh (gray < 8 -> black)
//
// usage: dashboard_render <fixtures-dir> <out-dir> [--font fallback|<ttf/ttc path>]
//                         [--px 12] [--only substring]
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "dashboard_core/config.h"
#include "dashboard_ui/layout.h"
#include "dashboard_ui/renderer.h"
#include "device_status.h"
#include "net_stack/wifi_list.h"
#include "panel_render.h"
#include "ui_text.h"
#include "host_font.h"
#include "lodepng.h"
#include "portrait_canvas.h"
#include "uc8279c_display.h"

namespace fs = std::filesystem;
using namespace eego;
using namespace eego::dashboard;

namespace {

std::string slurp(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

// Resolves http(s)://host/<path> to <fixtures>/<path> (the mock's asset files).
class FileImageProvider final : public ui::ImageProvider {
 public:
  explicit FileImageProvider(fs::path root) : root_(std::move(root)) {}
  bool load(const Asset&, std::string_view uri, ui::ImageBuffer& out) override {
    std::string path(uri);
    const size_t scheme = path.find("://");
    if (scheme != std::string::npos) {
      const size_t slash = path.find('/', scheme + 3);
      path = slash == std::string::npos ? "" : path.substr(slash + 1);
    }
    const size_t query = path.find('?');
    if (query != std::string::npos) path.resize(query);
    if (path.empty()) return false;
    const std::string bytes = slurp(root_ / path);
    if (bytes.empty() || bytes.size() > out.capacity) return false;
    std::copy(bytes.begin(), bytes.end(), out.data);
    out.size = bytes.size();
    return true;
  }

 private:
  fs::path root_;
};

bool encodeGray(const std::vector<unsigned char>& pixels, unsigned w, unsigned h,
                const fs::path& path) {
  unsigned char* png = nullptr;
  size_t png_size = 0;
  if (lodepng_encode_memory(&png, &png_size, pixels.data(), w, h, LCT_GREY, 8) != 0) return false;
  fs::create_directories(path.parent_path());
  std::ofstream file(path, std::ios::binary);
  file.write(reinterpret_cast<const char*>(png), static_cast<std::streamsize>(png_size));
  std::free(png);
  return static_cast<bool>(file);
}

// Contact sheets: every screen at half size, 4 x 2 per sheet, for quick review.
class ContactSheet {
 public:
  static constexpr unsigned kCols = 4;
  static constexpr unsigned kRows = 2;
  static constexpr unsigned kCellW = LOGICAL_WIDTH / 2 + 8;
  static constexpr unsigned kCellH = LOGICAL_HEIGHT / 2 + 8;

  explicit ContactSheet(fs::path dir) : dir_(std::move(dir)) { reset(); }
  ~ContactSheet() { flush(); }

  void add(const Uc8279cDisplay& display) {
    const unsigned col = count_ % kCols;
    const unsigned row = count_ / kCols;
    for (unsigned y = 0; y < LOGICAL_HEIGHT / 2; ++y) {
      for (unsigned x = 0; x < LOGICAL_WIDTH / 2; ++x) {
        unsigned sum = 0;
        for (unsigned dy = 0; dy < 2; ++dy)
          for (unsigned dx = 0; dx < 2; ++dx)
            sum += (display.grayAt(static_cast<int16_t>(x * 2 + dx), static_cast<int16_t>(y * 2 + dy)) >> 2) * 85;
        pixels_[(row * kCellH + 4 + y) * (kCols * kCellW) + col * kCellW + 4 + x] =
            static_cast<unsigned char>(sum / 4);
      }
    }
    if (++count_ == kCols * kRows) flush();
  }

 private:
  void reset() {
    pixels_.assign(static_cast<size_t>(kCols * kCellW) * kRows * kCellH, 200);
    count_ = 0;
  }
  void flush() {
    if (count_ == 0) return;
    char name[32];
    std::snprintf(name, sizeof(name), "sheet_%02u.png", sheet_++);
    encodeGray(pixels_, kCols * kCellW, kRows * kCellH, dir_ / name);
    reset();
  }
  fs::path dir_;
  std::vector<unsigned char> pixels_;
  unsigned count_ = 0;
  unsigned sheet_ = 0;
};

constexpr int16_t kStatusLineH = 40;  // dashboard_runtime.cpp kStatusLineH
constexpr int16_t kStatusLineY = 28 + 712 - kStatusLineH;
constexpr int kTz = 480;
constexpr uint64_t kSampleUnix = 1792893600ULL + 5 * 60;  // 10:05 local (UTC+8)

bool writePng(const Uc8279cDisplay& display, const fs::path& path, bool fast);

// Device-local screens drawn by main/panel_render + device_status (status bar,
// Status page with the power/sleep section, Settings with the keep-awake row) and
// the dashboard status line in every device state, for visual review.
int renderDeviceScreens(PortraitCanvas& canvas, Uc8279cDisplay& display, const fs::path& out,
                        const Dashboard* sample) {
  using namespace panel;
  int failures = 0;
  auto save = [&](const std::string& name) {
    if (!writePng(display, out / "device" / name, false) ||
        !writePng(display, out / "device" / "fast" / name, true)) {
      ++failures;
    }
  };
  struct Variant {
    const char* name;
    DeviceStateInfo info;
    bool sleep;
  };
  DeviceStateInfo keep;
  keep.state = DeviceState::KeepAwake;
  keep.keep_awake_min = 25;
  DeviceStateInfo idle;
  idle.state = DeviceState::Idle;
  idle.next_unix = kSampleUnix + 5 * 60;
  DeviceStateInfo awake;
  awake.state = DeviceState::Awake;
  awake.awake_timeout_s = 60;
  DeviceStateInfo asleep;
  asleep.state = DeviceState::Sleeping;
  asleep.next_unix = kSampleUnix + 55 * 60;
  const Variant variants[] = {{"running", DeviceStateInfo{}, false}, {"keep_awake", keep, false},
                              {"idle", idle, false},                {"awake", awake, false},
                              {"sleeping", asleep, true}};
  for (const Lang lang : {Lang::Zh, Lang::En}) {
    setLang(lang);
    const std::string suffix = lang == Lang::Zh ? "_zh.png" : "_en.png";
    for (const Variant& v : variants) {
      canvas.clear();
      if (sample != nullptr) {
        ui::DashboardRenderer renderer;
        ui::RenderOptions options;
        options.page_id = sample->pages.front().id;
        options.force_full = true;
        options.layout_config.content = ui::Rect{ui::kDashboardContent.x, ui::kDashboardContent.y,
                                                 ui::kDashboardContent.w,
                                                 static_cast<int16_t>(kStatusLineY - 28)};
        VariableValues variables;
        std::string error;
        defaultVariableValues(sample->variables, {}, variables, error);
        options.variables = variables;
        renderer.render(canvas, *sample, options);
      }
      StatusLineContent line;
      line.icon = v.info.state;
      const std::vector<std::string> labels = deviceStateLabels(v.info, kTz);
      line.left = labels.front();
      line.left_alt.assign(labels.begin() + 1, labels.end());
      const std::string updated = updatedLabel(v.sleep ? kSampleUnix - 5 * 60 : kSampleUnix, kTz);
      line.right = v.sleep ? "85% · " + updated : updated + " · 1/2";
      line.right_alt.push_back(updated);
      canvas.resetClip();
      drawStatusLine(canvas, 28, kStatusLineY, 496, kStatusLineH, line);
      save(std::string("statusline_") + v.name + suffix);
    }

    // Status page: status bar (keep-awake) + power/sleep section + soft keys.
    canvas.clear();
    canvas.useUiContentRect();
    StatusBarModel bar;
    std::snprintf(bar.timeText, sizeof(bar.timeText), "10:05");
    bar.batPercent = 85;
    bar.batMv = 4020;
    bar.wifiState = 2;
    bar.state = DeviceState::KeepAwake;
    std::snprintf(bar.stateText, sizeof(bar.stateText), "%s", deviceStateLabel(keep, kTz).c_str());
    std::snprintf(bar.stateShort, sizeof(bar.stateShort), "%s",
                  deviceStateLabels(keep, kTz)[1].c_str());
    drawStatusBar(canvas, bar);
    KvModel kv;
    auto big = [&](Str label, std::string value) {
      kv.rows.emplace_back(tr(label), std::move(value));
      kv.big.resize(kv.rows.size(), false);
      kv.big.back() = true;
    };
    auto heading = [&](Str label) {
      kv.rows.emplace_back(tr(label), "");
      kv.big.resize(kv.rows.size(), false);
      kv.big.back() = true;  // section headings: body-size bold
    };
    heading(Str::SecPower);
    big(Str::LblMode, deviceStateLabel(keep, kTz));
    big(Str::LblPower, std::string(tr(Str::ModeMains)) + " 85% · " + tr(Str::ReasonUsbHost));
    big(Str::LblKeepAwake, lang == Lang::Zh ? "剩余 25 分 · 至 10:30" : "25MIN LEFT · UNTIL 10:30");
    big(Str::LblWakeReason, tr(Str::WakeReset));
    char sleeps[48];
    std::snprintf(sleeps, sizeof(sleeps), tr(Str::SleepsFmt), 3u);
    big(Str::LblSleeps, sleeps);
    heading(Str::SecNetwork);
    big(Str::LblWifi, std::string(tr(Str::WifiConnected)) + " · office-2g");
    std::snprintf(kv.pageText, sizeof(kv.pageText), "1/3");
    drawStatus(canvas, kv);
    const SoftKeyModel keys[3] = {{tr(Str::Refresh), true}, {tr(Str::KeepAwakeKey), true},
                                  {tr(Str::Back), true}};
    drawSoftKeys(canvas, keys, 3, 1);
    canvas.resetClip();
    save(std::string("status_page") + suffix);

    // Status page 2: network section while offline — per saved network the last
    // failure reason (short text + code), AP fallback and the offline diag URL.
    canvas.clear();
    canvas.useUiContentRect();
    bar.wifiState = 4;
    drawStatusBar(canvas, bar);
    kv = KvModel{};
    const bool en = lang == Lang::En;
    heading(Str::SecNetwork);
    big(Str::LblWifi, tr(Str::WifiOffline));
    kv.rows.emplace_back("HydrogenSH", std::string(eego::net::wifiReasonShort(2, en)) + " (2)");
    kv.big.resize(kv.rows.size(), true);
    kv.rows.emplace_back("office-2g", std::string(eego::net::wifiReasonShort(201, en)) + " (201) · " +
                                          tr(Str::WifiNotSeen));
    kv.big.resize(kv.rows.size(), true);
    kv.rows.emplace_back("phone-ap", tr(Str::WifiNotTried));
    kv.big.resize(kv.rows.size(), true);
    big(Str::ApFallback, "EEGO-A4-Setup");
    big(Str::WifiDiag, "192.168.4.1/wifi-diag");
    big(Str::TimeLabel, "SNTP · 10-25 10:01");
    std::snprintf(kv.pageText, sizeof(kv.pageText), "2/3");
    drawStatus(canvas, kv);
    drawSoftKeys(canvas, keys, 3, 1);
    canvas.resetClip();
    save(std::string("status_page_wifi") + suffix);

    // Settings page 1 with the keep-awake action row (idle state in the bar).
    canvas.clear();
    canvas.useUiContentRect();
    bar.state = DeviceState::Idle;
    std::snprintf(bar.stateText, sizeof(bar.stateText), "%s", deviceStateLabel(idle, kTz).c_str());
    std::snprintf(bar.stateShort, sizeof(bar.stateShort), "%s",
                  deviceStateLabels(idle, kTz)[1].c_str());
    drawStatusBar(canvas, bar);
    S4Model s4;
    S4RowModel row;
    char label[64];
    std::snprintf(label, sizeof(label), tr(Str::SetKeepAwakeFmt), 30u);
    row.label = label;
    row.value = tr(Str::Off);
    row.isAction = true;
    s4.rows.push_back(row);
    row.label = tr(Str::SetNetwork);
    row.value = "192.168.1.50";
    row.isAction = false;
    s4.rows.push_back(row);
    row.label = tr(Str::SetLanguage);
    row.value = tr(Str::LangZh);
    s4.rows.push_back(row);
    s4.pageCount = 5;
    drawS4(canvas, s4);
    const SoftKeyModel skeys[3] = {{tr(Str::PrevPage), false}, {tr(Str::Back), true},
                                   {tr(Str::NextPage), true}};
    drawSoftKeys(canvas, skeys, 3, static_cast<size_t>(-1));
    canvas.resetClip();
    save(std::string("settings_page") + suffix);
  }
  setLang(Lang::Zh);
  return failures;
}

bool writePng(const Uc8279cDisplay& display, const fs::path& path, bool fast) {
  const unsigned w = LOGICAL_WIDTH;
  const unsigned h = LOGICAL_HEIGHT;
  std::vector<unsigned char> pixels(static_cast<size_t>(w) * h);
  for (unsigned y = 0; y < h; ++y) {
    for (unsigned x = 0; x < w; ++x) {
      const uint8_t g = display.grayAt(static_cast<int16_t>(x), static_cast<int16_t>(y));
      unsigned char v;
      if (fast) v = g >= 8 ? 255 : 0;
      else v = static_cast<unsigned char>((g >> 2) * 85);  // 0-3,4-7,8-11,12-15
      pixels[y * w + x] = v;
    }
  }
  return encodeGray(pixels, w, h, path);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr,
                 "usage: %s <fixtures-dir> <out-dir> [--font fallback|<font file>] [--px N] "
                 "[--only substring]\n",
                 argv[0]);
    return 2;
  }
  const fs::path fixtures = argv[1];
  const fs::path out_root = argv[2];
  std::string font = "fallback";
  std::string only;
  int px = 12;
  for (int i = 3; i + 1 < argc; i += 2) {
    const std::string key = argv[i];
    if (key == "--font") font = argv[i + 1];
    else if (key == "--px") px = std::atoi(argv[i + 1]);
    else if (key == "--only") only = argv[i + 1];
  }
  std::string label = "fallback";
  if (font == "fallback") {
    render_host::useFallbackFont();
  } else {
    std::string error;
    if (!render_host::useFreeTypeFont(font, px, error)) {
      std::fprintf(stderr, "font: %s\n", error.c_str());
      return 2;
    }
    label = "ft" + std::to_string(px);
  }
  const fs::path out = out_root / label;
  std::printf("font %s, line height %d -> %s\n", label.c_str(), render_host::fontLineHeight(),
              out.string().c_str());

  std::vector<fs::path> files;
  for (const auto& entry : fs::directory_iterator(fixtures)) {
    if (entry.is_regular_file() && entry.path().extension() == ".json") files.push_back(entry.path());
  }
  std::sort(files.begin(), files.end());

  static Uc8279cDisplay display;
  PortraitCanvas canvas(display);
  FileImageProvider images(fixtures);
  int failures = 0;
  int written = 0;
  ContactSheet sheet(out / "sheets");
  for (const fs::path& file : files) {
    const std::string name = file.stem().string();
    if (!only.empty() && name.find(only) == std::string::npos) continue;
    ParseResult parsed = parseDashboard(slurp(file));
    if (!parsed.ok()) {
      std::fprintf(stderr, "%s: parse failed: %s %s\n", name.c_str(),
                   parsed.errors.front().field.c_str(), parsed.errors.front().message.c_str());
      ++failures;
      continue;
    }
    const Dashboard& dash = parsed.dashboard;
    VariableValues variables;
    std::string error;
    defaultVariableValues(dash.variables, {}, variables, error);
    for (const Page& page : dash.pages) {
      if (!page.visible) continue;
      ui::LayoutConfig config;
      // Same as the device: the bottom kStatusLineH (40 px) status line is reserved.
      config.content = ui::Rect{ui::kDashboardContent.x, ui::kDashboardContent.y,
                                ui::kDashboardContent.w,
                                static_cast<int16_t>(kStatusLineY - ui::kDashboardContent.y)};
      const ui::PageLayout probe = ui::buildPageLayout(page, 0, config);
      const size_t screens = std::max<size_t>(1, probe.page_count);
      for (size_t screen = 0; screen < screens; ++screen) {
        ui::DashboardRenderer renderer;
        renderer.setImageProvider(&images);
        ui::RenderOptions options;
        options.page_id = page.id;
        options.page_index = screen;
        options.variables = variables;
        options.force_full = true;
        options.layout_config = config;
        canvas.clear();
        renderer.render(canvas, dash, options);
        // Status line as the runtime draws it (state icon + label, updated time, screen).
        panel::StatusLineContent line;
        line.left = panel::deviceStateLabel(panel::DeviceStateInfo{}, kTz);
        line.right = panel::updatedLabel(kSampleUnix, kTz) + " · " + std::to_string(screen + 1) +
                     "/" + std::to_string(screens);
        panel::drawStatusLine(canvas, 28, kStatusLineY, 496, kStatusLineH, line);
        const std::string stem = name + "__" + page.id + "_" + std::to_string(screen) + ".png";
        if (!writePng(display, out / stem, false) || !writePng(display, out / "fast" / stem, true)) {
          std::fprintf(stderr, "%s: png write failed\n", stem.c_str());
          ++failures;
        } else {
          sheet.add(display);
          ++written;
        }
      }
    }
  }
  {
    // Device screens use the first fixture that parses (dashboard behind the status line).
    std::optional<Dashboard> sample;
    for (const fs::path& file : files) {
      ParseResult parsed = parseDashboard(slurp(file));
      if (parsed.ok() && !parsed.dashboard.pages.empty()) {
        sample = parsed.dashboard;
        break;
      }
    }
    failures += renderDeviceScreens(canvas, display, out, sample ? &*sample : nullptr);
  }
  std::printf("%d screens rendered, %d failures\n", written, failures);
  return failures == 0 ? 0 : 1;
}
