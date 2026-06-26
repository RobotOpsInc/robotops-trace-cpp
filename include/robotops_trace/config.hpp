// Copyright 2026 Robot Ops Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef ROBOTOPS_TRACE__CONFIG_HPP_
#define ROBOTOPS_TRACE__CONFIG_HPP_

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "robotops_trace/exporter.hpp"

/// \file config.hpp
/// \brief Tracer lifecycle: Config + init/shutdown/force_flush.

namespace robotops
{

/// Tracer configuration. Every field has an env override (see comments); env
/// always wins so a fleet can retune or kill-switch without a redeploy.
struct Config
{
  /// env ROBOTOPS_SERVICE_NAME overrides.
  std::string service_name{"unknown_service"};
  /// env ROBOTOPS_OTLP_ENDPOINT overrides; "/v1/traces" is appended.
  std::string endpoint{"http://127.0.0.1:4318"};
  /// env ROBOTOPS_TRACE_ENABLED=0 hard-disables (the runtime kill switch).
  bool enabled{true};
  /// Extra resource attributes merged with service.name.
  std::vector<std::pair<std::string, std::string>> resource_attributes;
  /// env ROBOTOPS_TRACE_MAX_QUEUE — bounded queue capacity (drop when full).
  std::size_t max_queue{2048};
  /// env ROBOTOPS_TRACE_MAX_BATCH — max spans per export call.
  std::size_t max_batch{512};
  /// env ROBOTOPS_TRACE_SCHEDULE_DELAY_MS — periodic flush interval.
  std::chrono::milliseconds schedule_delay{5000};
  /// env ROBOTOPS_TRACE_EXPORTER selects the default exporter when `exporter`
  /// is null: "otlp" (default) => OTLP/HTTP + protobuf over libcurl;
  /// "console" => the JSON debug sink (prints to stdout, no network).
  std::string exporter_kind{"otlp"};
  /// null => default exporter built from `endpoint` + `exporter_kind`.
  std::shared_ptr<SpanExporter> exporter;
};

/// Initialize the tracer from the environment (builds a default Config).
/// Idempotent; a second call is a no-op + warn. Never throws.
void init() noexcept;

/// Initialize the tracer with an explicit Config (env still overrides fields).
/// Idempotent; a second call is a no-op + warn. Never throws.
void init(Config config) noexcept;

/// Flush + join the processor and release the exporter. Idempotent.
void shutdown() noexcept;

/// Block until the queue is drained or `timeout` elapses. Returns true if
/// fully drained. Never throws.
bool force_flush(std::chrono::milliseconds timeout) noexcept;

}  // namespace robotops

#endif  // ROBOTOPS_TRACE__CONFIG_HPP_
