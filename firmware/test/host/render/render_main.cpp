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
#include <sstream>
#include <string>
#include <vector>

#include "dashboard_core/config.h"
#include "dashboard_ui/layout.h"
#include "dashboard_ui/renderer.h"
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
      // Same as the device: the bottom 22 px status line is reserved.
      config.content = ui::Rect{ui::kDashboardContent.x, ui::kDashboardContent.y,
                                ui::kDashboardContent.w,
                                static_cast<int16_t>(28 + 712 - 22 - ui::kDashboardContent.y)};
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
        // Status line placeholder (drawn by the runtime on device).
        canvas.drawLine(28, 718, 28 + 495, 718, 0);
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
  std::printf("%d screens rendered, %d failures\n", written, failures);
  return failures == 0 ? 0 : 1;
}
