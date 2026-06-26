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

#ifndef ROBOTOPS_TRACE__EXPORTER_HPP_
#define ROBOTOPS_TRACE__EXPORTER_HPP_

#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "robotops_trace/span.hpp"

/// \file exporter.hpp
/// \brief The closed-span record (SpanData), the SpanExporter interface, and an
/// in-memory exporter for tests. Export is behind an abstract interface so the
/// default OTLP/HTTP + protobuf backend is swappable (a JSON console exporter is
/// the bundled debug alternative).

namespace robotops
{

/// A span event: a timestamped, named point with optional attributes.
struct EventData
{
  std::uint64_t time_unix_nano{0};
  std::string name;
  std::vector<std::pair<std::string, AttributeValue>> attributes;
};

/// The finalized, immutable record handed to exporters when a span closes.
struct SpanData
{
  SpanContext context;
  std::array<std::uint8_t, 8> parent_span_id{};   ///< all-zero == root
  std::string name;
  SpanKind kind{SpanKind::Internal};
  std::uint64_t start_unix_nano{0};
  std::uint64_t end_unix_nano{0};
  StatusCode status_code{StatusCode::Unset};
  std::string status_message;
  std::vector<std::pair<std::string, AttributeValue>> attributes;
  std::vector<EventData> events;
};

/// Resource attributes (service.name, etc.) attached to every batch.
struct Resource
{
  std::vector<std::pair<std::string, std::string>> attributes;
};

/// Abstract batch exporter. Implementations MUST NOT throw.
class SpanExporter
{
public:
  virtual ~SpanExporter() = default;

  /// Export a batch. Returns false on failure (logged + dropped, best-effort).
  virtual bool export_spans(
    const Resource & resource,
    const std::vector<SpanData> & spans) noexcept = 0;

  /// Flush anything buffered inside the exporter. Default: nothing to do.
  virtual bool force_flush(std::chrono::milliseconds timeout) noexcept
  {
    (void)timeout;
    return true;
  }

  /// Release resources. Called once on tracer shutdown.
  virtual void shutdown() noexcept {}
};

/// Stores every exported span in memory. Thread-safe. For tests.
class InMemorySpanExporter : public SpanExporter
{
public:
  bool export_spans(
    const Resource & resource,
    const std::vector<SpanData> & spans) noexcept override;

  /// Snapshot of all spans exported so far.
  std::vector<SpanData> spans() const;
  /// Number of spans exported so far.
  std::size_t size() const;
  /// Drop all stored spans.
  void clear();

private:
  mutable std::mutex mutex_;
  std::vector<SpanData> spans_;
  Resource resource_;
};

}  // namespace robotops

#endif  // ROBOTOPS_TRACE__EXPORTER_HPP_
