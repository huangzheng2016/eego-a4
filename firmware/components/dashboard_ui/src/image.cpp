// SPDX-License-Identifier: GPL-3.0-or-later
#include "dashboard_ui/image.h"

#include <algorithm>
#include <cstring>
#include <new>
#include <vector>

#include "dashboard_ui/text.h"
#include "panel_style.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "sd_card.h"

namespace eego::dashboard::ui {
namespace {

struct HeapBytes {
  uint8_t* data = nullptr;
  size_t capacity = 0;
  size_t size = 0;

  HeapBytes() = default;
  ~HeapBytes() { heap_caps_free(data); }
  HeapBytes(const HeapBytes&) = delete;
  HeapBytes& operator=(const HeapBytes&) = delete;
  HeapBytes(HeapBytes&& other) noexcept { *this = std::move(other); }
  HeapBytes& operator=(HeapBytes&& other) noexcept {
    if (this == &other) return *this;
    heap_caps_free(data);
    data = other.data;
    capacity = other.capacity;
    size = other.size;
    other.data = nullptr;
    other.capacity = 0;
    other.size = 0;
    return *this;
  }
  bool allocate(size_t bytes) {
    data = static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    capacity = data == nullptr ? 0 : bytes;
    size = 0;
    return data != nullptr;
  }
};

const Asset* findAsset(const Dashboard& dashboard, std::string_view id) {
  for (const Asset& asset : dashboard.assets) {
    if (asset.id == id && asset.kind == AssetKind::Image) return &asset;
  }
  return nullptr;
}

bool hasParentSegment(std::string_view uri) {
  size_t start = 0;
  while (start <= uri.size()) {
    const size_t end = uri.find_first_of("/\\", start);
    if (uri.substr(start, end - start) == "..") return true;
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  return false;
}

bool validUri(std::string_view uri) {
  const bool network = uri.substr(0, 7) == "http://" || uri.substr(0, 8) == "https://";
  const bool local = uri.substr(0, 3) == "sd:" || (!uri.empty() && uri.front() == '/');
  if (!network && !local) return false;
  if (local && (uri == "sd:" || uri == "/")) return false;
  for (const unsigned char c : uri) {
    if (c <= 0x20 || c == 0x7f) return false;
  }
  return network || !hasParentSegment(uri.substr(0, 3) == "sd:" ? uri.substr(3) : uri);
}

std::string interpolate(const std::string& input, const VariableValues& variables) {
  std::string output;
  std::string error;
  if (!interpolateVariables(input, variables, output, error, kMaxUrlCodePoints * 4)) return {};
  return output;
}

const MetricSample* latestSample(const Dashboard& dashboard, const PanelSpec& panel) {
  const MetricSample* latest = nullptr;
  for (const MetricSample& sample : dashboard.samples) {
    if (!panel.source_id.empty() && sample.source_id != panel.source_id) continue;
    if (!panel.metric.empty() && sample.metric != panel.metric) continue;
    if (latest == nullptr || sample.timestamp_ms >= latest->timestamp_ms) latest = &sample;
  }
  return latest;
}

std::string cacheKey(const ImageDependency& image) {
  std::string key = image.asset->id;
  key.push_back('\n');
  key += image.uri;
  key.push_back('\n');
  key += image.asset->sha256;
  key += std::to_string(image.asset->size_bytes);
  return key;
}

void drawFailure(PortraitCanvas& canvas, const PanelSpec& panel, const Rect& bounds) {
  if (bounds.empty()) return;
  const std::string message = panel.image.alt_text.empty() ? "Image unavailable" : panel.image.alt_text;
  style::emptyState(canvas, FontManager::instance(), bounds, message);
}

}  // namespace

ImageDependency resolveImageDependency(const Dashboard& dashboard, const PanelSpec& panel,
                                       const VariableValues& variables) {
  ImageDependency resolved;
  resolved.base_asset = findAsset(dashboard, panel.image.asset_id);
  resolved.asset = resolved.base_asset;
  if (resolved.base_asset != nullptr) resolved.uri = interpolate(resolved.base_asset->uri, variables);
  if (!panel.value_path.empty()) {
    resolved.field_sample = latestSample(dashboard, panel);
    if (resolved.field_sample != nullptr) {
      std::string_view key(panel.value_path);
      if (key.substr(0, 2) == "$.") key.remove_prefix(2);
      if (key.substr(0, 7) == "labels.") key.remove_prefix(7);
      for (const KeyValue& label : resolved.field_sample->labels) {
        if (label.key == key) {
          resolved.field_value = label.value;
          break;
        }
      }
    }
  }
  if (!resolved.field_value.empty()) {
    if (const Asset* referenced = findAsset(dashboard, resolved.field_value)) {
      resolved.dynamic_asset = referenced;
      resolved.asset = referenced;
      resolved.uri = interpolate(referenced->uri, variables);
    } else {
      resolved.uri = interpolate(resolved.field_value, variables);
    }
  }
  resolved.valid = resolved.asset != nullptr && validUri(resolved.uri) &&
                   resolved.asset->size_bytes <= kMaxEncodedImageBytes;
  return resolved;
}

struct ImagePipeline::Impl {
  struct Entry {
    std::string asset_id;
    std::string key;
    HeapBytes bytes;
    uint64_t used = 0;
    int64_t loaded_at = 0;
  };
  std::vector<Entry> entries;
  uint64_t clock = 0;
  size_t bytes = 0;
};

bool SdImageProvider::load(const Asset&, std::string_view resolved_uri, ImageBuffer& output) {
  if (output.data == nullptr || output.capacity == 0 || !validUri(resolved_uri) ||
      resolved_uri.substr(0, 7) == "http://" || resolved_uri.substr(0, 8) == "https://") {
    return false;
  }
  std::string path(resolved_uri);
  if (path.substr(0, 3) == "sd:") path.erase(0, 3);
  if (path.substr(0, 8) == "/sdcard/") path.erase(0, 8);
  while (!path.empty() && path.front() == '/') path.erase(path.begin());
  if (path.empty()) return false;
  SdCard& sd = SdCard::instance();
  if (!sd.ready()) return false;
  const size_t file_size = sd.fileSize(path.c_str());
  if (file_size == 0 || file_size > output.capacity || file_size > kMaxEncodedImageBytes)
    return false;
  const size_t read = sd.readFile(path.c_str(), output.data, file_size);
  if (read != file_size) return false;
  output.size = read;
  return true;
}

ImagePipeline::ImagePipeline() : impl_(new (std::nothrow) Impl) {}
ImagePipeline::~ImagePipeline() { delete impl_; }

void ImagePipeline::invalidate(std::string_view asset_id) {
  if (impl_ == nullptr) return;
  if (asset_id.empty()) {
    clear();
    return;
  }
  for (size_t i = 0; i < impl_->entries.size();) {
    if (impl_->entries[i].asset_id == asset_id) {
      impl_->bytes -= impl_->entries[i].bytes.size;
      impl_->entries.erase(impl_->entries.begin() + static_cast<std::ptrdiff_t>(i));
    } else {
      ++i;
    }
  }
}

void ImagePipeline::clear() {
  if (impl_ == nullptr) return;
  impl_->entries.clear();
  impl_->bytes = 0;
}

size_t ImagePipeline::cacheBytes() const {
  return impl_ == nullptr ? 0 : impl_->bytes;
}

void ImagePipeline::draw(PortraitCanvas& canvas, const Dashboard& dashboard,
                         const PanelSpec& panel, const Rect& bounds,
                         const VariableValues& variables) {
  const ImageDependency resolved = resolveImageDependency(dashboard, panel, variables);
  if (impl_ == nullptr || !resolved.valid || bounds.empty()) {
    drawFailure(canvas, panel, bounds);
    return;
  }
  const std::string key = cacheKey(resolved);
  const int64_t now = esp_timer_get_time();
  const int64_t cache_micros = static_cast<int64_t>(resolved.asset->cache_seconds) * 1000000;
  Impl::Entry* cached = nullptr;
  for (size_t i = 0; i < impl_->entries.size(); ++i) {
    Impl::Entry& entry = impl_->entries[i];
    if (entry.key != key) continue;
    const bool expired = cache_micros == 0 || now - entry.loaded_at >= cache_micros;
    if (expired) {
      impl_->bytes -= entry.bytes.size;
      impl_->entries.erase(impl_->entries.begin() + static_cast<std::ptrdiff_t>(i));
    } else {
      cached = &entry;
    }
    break;
  }

  HeapBytes temporary;
  const uint8_t* data = nullptr;
  size_t size = 0;
  if (cached != nullptr) {
    cached->used = ++impl_->clock;
    data = cached->bytes.data;
    size = cached->bytes.size;
    ++cache_hits_;
  } else {
    ++cache_misses_;
    if (!temporary.allocate(kMaxEncodedImageBytes)) {
      drawFailure(canvas, panel, bounds);
      return;
    }
    ImageBuffer output{temporary.data, temporary.capacity, 0};
    bool loaded = provider_ != nullptr && provider_->load(*resolved.asset, resolved.uri, output);
    if (!loaded && resolved.uri.substr(0, 7) != "http://" &&
        resolved.uri.substr(0, 8) != "https://") {
      output.size = 0;
      loaded = sd_provider_.load(*resolved.asset, resolved.uri, output);
    }
    if (!loaded || output.size == 0 || output.size > output.capacity) {
      drawFailure(canvas, panel, bounds);
      return;
    }
    temporary.size = output.size;
    data = temporary.data;
    size = temporary.size;
  }

  uint32_t source_width = 0;
  uint32_t source_height = 0;
  if (!inspectImage(data, size, source_width, source_height)) {
    if (cached != nullptr) invalidate(resolved.asset->id);
    drawFailure(canvas, panel, bounds);
    return;
  }

  int16_t draw_x = bounds.x;
  int16_t draw_y = bounds.y;
  int16_t draw_width = bounds.w;
  int16_t draw_height = bounds.h;
  if (panel.image.fit == ImageFit::Cover) {
    uint64_t scaled_width = bounds.w;
    uint64_t scaled_height = bounds.h;
    if (static_cast<uint64_t>(source_width) * bounds.h >
        static_cast<uint64_t>(source_height) * bounds.w) {
      scaled_width = (static_cast<uint64_t>(source_width) * bounds.h + source_height - 1) /
                     source_height;
    } else {
      scaled_height = (static_cast<uint64_t>(source_height) * bounds.w + source_width - 1) /
                      source_width;
    }
    if (scaled_width > INT16_MAX || scaled_height > INT16_MAX ||
        scaled_width * scaled_height > kMaxImagePixels) {
      drawFailure(canvas, panel, bounds);
      return;
    }
    draw_width = static_cast<int16_t>(scaled_width);
    draw_height = static_cast<int16_t>(scaled_height);
    draw_x = bounds.x + (bounds.w - draw_width) / 2;
    draw_y = bounds.y + (bounds.h - draw_height) / 2;
  }

  if (!ImageRenderer::instance().renderFromBuffer(canvas, data, size, draw_x, draw_y,
                                                  draw_width, draw_height)) {
    if (cached != nullptr) invalidate(resolved.asset->id);
    drawFailure(canvas, panel, bounds);
    return;
  }

  if (cached == nullptr && resolved.asset->cache_seconds > 0 && size <= kMaxImageCacheBytes) {
    while (!impl_->entries.empty() &&
           (impl_->entries.size() >= kMaxImageCacheEntries ||
            impl_->bytes + size > kMaxImageCacheBytes)) {
      auto oldest = std::min_element(
          impl_->entries.begin(), impl_->entries.end(),
          [](const Impl::Entry& lhs, const Impl::Entry& rhs) { return lhs.used < rhs.used; });
      impl_->bytes -= oldest->bytes.size;
      impl_->entries.erase(oldest);
    }
    Impl::Entry entry;
    entry.asset_id = resolved.asset->id;
    entry.key = key;
    entry.used = ++impl_->clock;
    entry.loaded_at = now;
    if (entry.bytes.allocate(size)) {
      std::memcpy(entry.bytes.data, data, size);
      entry.bytes.size = size;
      impl_->bytes += size;
      impl_->entries.push_back(std::move(entry));
    }
  }
}

}  // namespace eego::dashboard::ui
