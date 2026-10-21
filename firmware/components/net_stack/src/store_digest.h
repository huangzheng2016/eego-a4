// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// AlertStore 变更侦测摘要（net_task 内部使用；无 ESP-IDF 依赖，可宿主编译）。
//
// net_task 在每次可能改写 store 的操作后（摄入命令、适配器 runCycle、告警
// 动作、过期 tick）计算摘要并与上次发布快照时的值比较：变了才 buildSnapshot
// + publish，没变就跳过（避免每次轮询 304 也全量拷贝 500 条记录）。
//
// 摘要覆盖每条记录的 id/state/severity/muted/occurrences/last_seen/expires_at/
// ack_at/cleared_at —— store 的任何变更路径（ingest 新建/去重更新、ack/ackAll、
// clear、expire、evict/purge、mute 重估）至少会翻动其中一项，且每次变更都会
// touch() 落库行，不存在"变了但摘要不变"的路径。
//
// 只能在 store 属主任务（net_task）上下文调用。

#include <cstdint>

#include "alert_core/store.h"

namespace eego::net::internal {

uint64_t alertStoreDigest(const alert_core::AlertStore& store);

}  // namespace eego::net::internal
