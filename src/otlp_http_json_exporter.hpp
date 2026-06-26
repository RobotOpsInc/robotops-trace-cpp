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

#ifndef OTLP_HTTP_JSON_EXPORTER_HPP_
#define OTLP_HTTP_JSON_EXPORTER_HPP_

#include <chrono>
#include <mutex>
#include <string>
#include <vector>

#include "robotops_trace/exporter.hpp"

namespace robotops
{

/// The default exporter: serializes a batch to the OTLP/JSON
/// ExportTraceServiceRequest shape with a hand-rolled writer (no JSON lib) and
/// POSTs it to "<endpoint>/v1/traces" via libcurl. A single CURL handle is
/// reused behind a mutex on the background thread. Any non-2xx / transport error
/// is logged at debug and the batch dropped (best-effort). libcurl is confined
/// entirely to this .cpp — it never appears in a public header.
class OtlpHttpJsonExporter : public SpanExporter
{
public:
  explicit OtlpHttpJsonExporter(std::string endpoint);
  ~OtlpHttpJsonExporter() override;

  bool export_spans(
    const Resource & resource,
    const std::vector<SpanData> & spans) noexcept override;

  void shutdown() noexcept override;

  /// Build the OTLP/JSON request body for a batch (exposed for tests).
  static std::string serialize(
    const Resource & resource,
    const std::vector<SpanData> & spans);

private:
  std::string traces_url_;
  std::mutex curl_mutex_;
  void * curl_{nullptr};   // CURL* (opaque to keep libcurl out of the header)
};

}  // namespace robotops

#endif  // OTLP_HTTP_JSON_EXPORTER_HPP_
