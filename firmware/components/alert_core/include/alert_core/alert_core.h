// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Umbrella header for the alert_core component.
//
// alert_core is the pure-C++ alert domain core (M12): no UI, no network, no
// ESP-IDF dependencies. One ingest task owns AlertStore and mutates it;
// everything else (UI, admin API) reads immutable snapshots or goes through
// the store's task-confined query API.
//
//   json.h / sha1.h / ulid.h / time_util.h   foundations
//   types.h         AlertRecord, AlertEvent, ingest validation, fingerprints
//   type_registry.h 39 presets + generic.any, bilingual, overrides
//   store.h         dedup/ack/escalate/clear/expire/mute/500-ring semantics
//   snapshot.h      immutable snapshots + double-buffered lock-free publishing
//   persistence.h   alerts.db JSONL + compaction + source cache, via IAlertFileIo
//   file_io_stdio.h host/diagnostics stdio IO implementation

#include "alert_core/file_io_stdio.h"
#include "alert_core/json.h"
#include "alert_core/persistence.h"
#include "alert_core/sha1.h"
#include "alert_core/snapshot.h"
#include "alert_core/store.h"
#include "alert_core/time_util.h"
#include "alert_core/type_registry.h"
#include "alert_core/types.h"
#include "alert_core/ulid.h"
