// SPDX-License-Identifier: GPL-3.0-or-later
// 适配器注册表实现。
#include "net_stack/source_adapter.h"

namespace eego::net {

SourceAdapterRegistry& SourceAdapterRegistry::instance() {
  static SourceAdapterRegistry reg;
  return reg;
}

void SourceAdapterRegistry::registerAdapter(const std::string& kind, Factory factory) {
  factories_[kind] = std::move(factory);
}

std::unique_ptr<ISourceAdapter> SourceAdapterRegistry::create(const std::string& kind) const {
  auto it = factories_.find(kind);
  if (it == factories_.end() || !it->second) return nullptr;
  return it->second();
}

std::vector<std::string> SourceAdapterRegistry::kinds() const {
  std::vector<std::string> out;
  out.reserve(factories_.size());
  for (const auto& kv : factories_) out.push_back(kv.first);
  return out;
}

}  // namespace eego::net
