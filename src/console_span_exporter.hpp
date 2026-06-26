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

#ifndef CONSOLE_SPAN_EXPORTER_HPP_
#define CONSOLE_SPAN_EXPORTER_HPP_

#include <string>
#include <vector>

#include "robotops_trace/exporter.hpp"

namespace robotops
{

/// The debug exporter: serializes a batch to the OTLP/JSON
/// ExportTraceServiceRequest shape with a hand-rolled writer (no JSON lib) and
/// prints one request per batch to stdout instead of POSTing it. The default
/// OTLP/HTTP wire is now protobuf (OtlpHttpExporter); this JSON path is demoted
/// to a human-readable debug/console sink, selected with
/// ROBOTOPS_TRACE_EXPORTER=console (or Config::exporter_kind). No libcurl.
class ConsoleSpanExporter : public SpanExporter
{
public:
  ConsoleSpanExporter() = default;

  bool export_spans(
    const Resource & resource,
    const std::vector<SpanData> & spans) noexcept override;

  /// Build the OTLP/JSON request body for a batch (exposed for tests).
  static std::string serialize(
    const Resource & resource,
    const std::vector<SpanData> & spans);
};

}  // namespace robotops

#endif  // CONSOLE_SPAN_EXPORTER_HPP_
