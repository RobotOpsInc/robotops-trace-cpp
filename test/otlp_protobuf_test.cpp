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

// ROB-438: hand-rolled OTLP/protobuf wire serializer. This builds a known span
// tree, serializes it to the canonical ExportTraceServiceRequest protobuf, and
// (a) sanity-checks the wire in-process and (b) dumps the raw bytes to the file
// named by ROBOTOPS_TRACE_PB_OUT so a real opentelemetry-proto decoder can
// cross-verify wire correctness (see the PR's python cross-decode step).

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "robotops_trace/exporter.hpp"
#include "otlp_http_exporter.hpp"
#include "test_harness.hpp"

namespace
{

using robotops::AttributeValue;
using robotops::EventData;
using robotops::OtlpHttpExporter;
using robotops::Resource;
using robotops::SpanData;
using robotops::SpanKind;
using robotops::StatusCode;

// A fixed, fully-known span tree (deterministic ids/values) so an external
// decoder can assert exact round-trip equality.
std::vector<SpanData> build_known_tree(Resource & resource)
{
  resource.attributes.emplace_back("service.name", "rob438-verify");
  resource.attributes.emplace_back("telemetry.sdk.language", "cpp");

  SpanData root;
  for (std::size_t i = 0; i < root.context.trace_id.size(); ++i) {
    root.context.trace_id[i] = static_cast<std::uint8_t>(i + 1);   // 01..10
  }
  root.context.span_id = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
  // parent_span_id left all-zero == root (field must be OMITTED on the wire).
  root.name = "root";
  root.kind = SpanKind::Server;
  root.start_unix_nano = 1700000000000000000ULL;
  root.end_unix_nano = 1700000000500000000ULL;
  root.status_code = StatusCode::Error;
  root.status_message = "boom";
  root.attributes.emplace_back("robot.id", AttributeValue("arm-7"));
  root.attributes.emplace_back("ok", AttributeValue(true));
  root.attributes.emplace_back("count", AttributeValue(static_cast<std::int64_t>(42)));
  root.attributes.emplace_back("neg", AttributeValue(static_cast<std::int64_t>(-7)));
  root.attributes.emplace_back("ratio", AttributeValue(0.5));
  EventData event;
  event.time_unix_nano = 1700000000250000000ULL;
  event.name = "grasp";
  event.attributes.emplace_back("reason", AttributeValue("slip"));
  root.events.push_back(event);

  SpanData child;
  child.context.trace_id = root.context.trace_id;
  child.context.span_id = {0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x01};
  child.parent_span_id = root.context.span_id;
  child.name = "child";
  child.kind = SpanKind::Internal;
  child.start_unix_nano = 1700000000100000000ULL;
  child.end_unix_nano = 1700000000200000000ULL;
  // No status (Unset + empty) => the whole Status message must be omitted.

  return {root, child};
}

}  // namespace

// 11. Hand-rolled OTLP/protobuf serializer produces a well-formed wire body and
//     dumps it for the external opentelemetry-proto cross-decode.
TEST_CASE(otlp_protobuf_serialize_and_dump)
{
  Resource resource;
  const std::vector<SpanData> spans = build_known_tree(resource);

  const std::string body = OtlpHttpExporter::serialize(resource, spans);

  CHECK(!body.empty());
  // ExportTraceServiceRequest field 1 (resource_spans), wire type 2 (LEN):
  // tag byte == (1 << 3) | 2 == 0x0a.
  CHECK_EQ(static_cast<unsigned char>(body[0]), 0x0au);
  // The body is protobuf, not JSON: it must NOT start with '{'.
  CHECK(body[0] != '{');

  if (const char * out_path = std::getenv("ROBOTOPS_TRACE_PB_OUT")) {
    std::FILE * fp = std::fopen(out_path, "wb");
    CHECK(fp != nullptr);
    if (fp != nullptr) {
      const std::size_t written = std::fwrite(body.data(), 1, body.size(), fp);
      CHECK_EQ(written, body.size());
      std::fclose(fp);
      std::printf("    wrote %zu protobuf bytes to %s\n", body.size(), out_path);
    }
  }
}
