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

#include "global.hpp"

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "robotops_trace/config.hpp"
#include "detail/batch_processor.hpp"
#include "detail/log.hpp"
#include "otlp_http_json_exporter.hpp"

namespace robotops
{
namespace global
{

namespace
{

struct TracerState
{
  Resource resource;
  std::shared_ptr<SpanExporter> exporter;
  std::unique_ptr<detail::BatchProcessor> processor;
};

std::mutex g_mutex;
// Published with release / read with acquire so the hot path stays lock-free.
std::atomic<TracerState *> g_state{nullptr};

const char * env_or_null(const char * key) noexcept
{
  const char * value = std::getenv(key);
  if (value == nullptr || value[0] == '\0') {
    return nullptr;
  }
  return value;
}

std::size_t parse_size(const char * text, std::size_t fallback) noexcept
{
  try {
    const long long parsed = std::stoll(text);   // NOLINT(runtime/int)
    if (parsed > 0) {
      return static_cast<std::size_t>(parsed);
    }
  } catch (...) {
    // fall through to default
  }
  return fallback;
}

// Apply environment overrides on top of a Config. Env always wins so a fleet can
// retune or kill-switch without a redeploy.
void apply_env(Config & config) noexcept
{
  if (const char * value = env_or_null("ROBOTOPS_SERVICE_NAME")) {
    config.service_name = value;
  }
  if (const char * value = env_or_null("ROBOTOPS_OTLP_ENDPOINT")) {
    config.endpoint = value;
  }
  if (const char * value = std::getenv("ROBOTOPS_TRACE_ENABLED")) {
    // Explicit "0"/"false"/"off" is the hard kill switch.
    const std::string flag(value);
    if (flag == "0" || flag == "false" || flag == "off" || flag == "FALSE") {
      config.enabled = false;
    } else if (flag == "1" || flag == "true" || flag == "on" || flag == "TRUE") {
      config.enabled = true;
    }
  }
  if (const char * value = env_or_null("ROBOTOPS_TRACE_MAX_QUEUE")) {
    config.max_queue = parse_size(value, config.max_queue);
  }
  if (const char * value = env_or_null("ROBOTOPS_TRACE_MAX_BATCH")) {
    config.max_batch = parse_size(value, config.max_batch);
  }
  if (const char * value = env_or_null("ROBOTOPS_TRACE_SCHEDULE_DELAY_MS")) {
    config.schedule_delay =
      std::chrono::milliseconds(static_cast<std::chrono::milliseconds::rep>(
          parse_size(value, static_cast<std::size_t>(config.schedule_delay.count()))));
  }
}

Resource build_resource(const Config & config)
{
  Resource resource;
  resource.attributes.emplace_back("service.name", config.service_name);
  resource.attributes.emplace_back("telemetry.sdk.name", "robotops-trace-cpp");
  resource.attributes.emplace_back("telemetry.sdk.language", "cpp");
  resource.attributes.emplace_back("telemetry.sdk.version", version());
  for (const auto & attr : config.resource_attributes) {
    resource.attributes.push_back(attr);
  }
  return resource;
}

}  // namespace

bool is_active() noexcept
{
  return g_state.load(std::memory_order_acquire) != nullptr;
}

void submit(SpanData span) noexcept
{
  TracerState * state = g_state.load(std::memory_order_acquire);
  if (state == nullptr) {
    return;  // disabled / not initialized — drop.
  }
  state->processor->enqueue(std::move(span));
}

}  // namespace global

// ---------------------------------------------------------------------------
// Public lifecycle API (config.hpp)
// ---------------------------------------------------------------------------

void init() noexcept
{
  init(Config{});
}

void init(Config config) noexcept
{
  using global::g_mutex;
  using global::g_state;
  using global::TracerState;

  try {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_state.load(std::memory_order_relaxed) != nullptr) {
      detail::log_warn("robotops::init() called more than once; ignoring");
      return;
    }

    global::apply_env(config);

    if (!config.enabled) {
      // Kill switch / disabled: stay inactive. All ops remain no-ops.
      detail::log_debug("tracing disabled; init is a no-op");
      return;
    }

    std::shared_ptr<SpanExporter> exporter = config.exporter;
    Resource resource = global::build_resource(config);
    if (!exporter) {
      exporter = std::make_shared<OtlpHttpJsonExporter>(config.endpoint);
    }

    auto state = std::make_unique<TracerState>();
    state->resource = resource;
    state->exporter = exporter;
    state->processor = std::make_unique<detail::BatchProcessor>(
      exporter, resource, config.max_queue, config.max_batch, config.schedule_delay);

    g_state.store(state.release(), std::memory_order_release);
  } catch (...) {
    detail::log_warn("robotops::init() failed; tracing remains disabled");
  }
}

void shutdown() noexcept
{
  using global::g_mutex;
  using global::g_state;
  using global::TracerState;

  TracerState * state = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    state = g_state.exchange(nullptr, std::memory_order_acq_rel);
  }
  if (state == nullptr) {
    return;  // idempotent
  }
  if (state->processor) {
    state->processor->shutdown();
  }
  delete state;
}

bool force_flush(std::chrono::milliseconds timeout) noexcept
{
  global::TracerState * state = global::g_state.load(std::memory_order_acquire);
  if (state == nullptr || !state->processor) {
    return true;  // nothing to flush
  }
  return state->processor->force_flush(timeout);
}

}  // namespace robotops
