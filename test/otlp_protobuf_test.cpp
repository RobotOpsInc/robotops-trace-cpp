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
#include <cstring>
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
  const bool starts_like_json = (body[0] == '{');
  CHECK(!starts_like_json);

  if (const char * out_path = std::getenv("ROBOTOPS_TRACE_PB_OUT")) {
    std::FILE * fp = std::fopen(out_path, "wb");
    CHECK(fp);
    if (fp != nullptr) {
      const std::size_t written = std::fwrite(body.data(), 1, body.size(), fp);
      CHECK_EQ(written, body.size());
      std::fclose(fp);
      std::printf("    wrote %zu protobuf bytes to %s\n", body.size(), out_path);
    }
  }
}

// --- a deliberately tiny protobuf reader (ROB-444) --------------------------
// Just enough to walk the LEN-delimited message tree and pull scalar fields so
// the array-attribute wire (AnyValue.array_value, field 5 => ArrayValue{ repeated
// AnyValue values=1 }) can be decode-verified IN-PROCESS, with no protobuf lib.
namespace
{

std::uint64_t read_varint(const std::string & b, std::size_t & i)
{
  std::uint64_t v = 0;
  int shift = 0;
  while (i < b.size()) {
    const unsigned char c = static_cast<unsigned char>(b[i++]);
    v |= static_cast<std::uint64_t>(c & 0x7F) << shift;
    if ((c & 0x80) == 0) {
      break;
    }
    shift += 7;
  }
  return v;
}

// Skip a field's payload given its wire type, advancing i.
void skip_field(const std::string & b, std::size_t & i, std::uint32_t wire_type)
{
  switch (wire_type) {
    case 0: read_varint(b, i); break;                       // varint
    case 1: i += 8; break;                                  // fixed64
    case 5: i += 4; break;                                  // fixed32
    case 2: {                                               // length-delimited
        const std::uint64_t len = read_varint(b, i);
        i += static_cast<std::size_t>(len);
        break;
      }
    default: i = b.size(); break;                           // unknown => stop
  }
}

// All length-delimited (wire type 2) payloads carrying field number `field`.
std::vector<std::string> len_fields(const std::string & b, std::uint32_t field)
{
  std::vector<std::string> out;
  std::size_t i = 0;
  while (i < b.size()) {
    const std::uint64_t tag = read_varint(b, i);
    const std::uint32_t f = static_cast<std::uint32_t>(tag >> 3);
    const std::uint32_t wt = static_cast<std::uint32_t>(tag & 0x7);
    if (wt == 2) {
      const std::uint64_t len = read_varint(b, i);
      std::string payload = b.substr(i, static_cast<std::size_t>(len));
      i += static_cast<std::size_t>(len);
      if (f == field) {
        out.push_back(std::move(payload));
      }
    } else {
      skip_field(b, i, wt);
    }
  }
  return out;
}

// All fixed64 (wire type 1) raw values carrying field number `field`.
std::vector<std::uint64_t> fixed64_fields(const std::string & b, std::uint32_t field)
{
  std::vector<std::uint64_t> out;
  std::size_t i = 0;
  while (i < b.size()) {
    const std::uint64_t tag = read_varint(b, i);
    const std::uint32_t f = static_cast<std::uint32_t>(tag >> 3);
    const std::uint32_t wt = static_cast<std::uint32_t>(tag & 0x7);
    if (wt == 1) {
      std::uint64_t v = 0;
      for (int k = 0; k < 8; ++k) {
        v |= static_cast<std::uint64_t>(static_cast<unsigned char>(b[i + k])) << (8 * k);
      }
      i += 8;
      if (f == field) {
        out.push_back(v);
      }
    } else {
      skip_field(b, i, wt);
    }
  }
  return out;
}

}  // namespace

// 12. (ROB-444) Array-valued attributes serialize to OTLP arrayValue. Build a
//     span carrying a string[] and a double[] attribute, serialize to protobuf,
//     and decode-verify the AnyValue.array_value (field 5 => ArrayValue{ repeated
//     AnyValue values=1 }) round-trips both arrays element-for-element.
TEST_CASE(otlp_protobuf_array_value_round_trip)
{
  Resource resource;
  resource.attributes.emplace_back("service.name", "rob444-arrays");

  SpanData span;
  for (std::size_t i = 0; i < span.context.trace_id.size(); ++i) {
    span.context.trace_id[i] = static_cast<std::uint8_t>(i + 1);
  }
  span.context.span_id = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
  span.name = "ros2_control::update";
  span.kind = SpanKind::Internal;
  span.start_unix_nano = 1700000000000000000ULL;
  span.end_unix_nano = 1700000000100000000ULL;
  span.attributes.emplace_back(
    "robot.joint.name",
    AttributeValue(std::vector<std::string>{"shoulder", "elbow", "wrist"}));
  span.attributes.emplace_back(
    "robot.target.position",
    AttributeValue(std::vector<double>{0.25, -1.5, 3.0}));

  const std::string body = OtlpHttpExporter::serialize(resource, {span});
  CHECK(!body.empty());

  // Walk ExportTraceServiceRequest(1) -> ResourceSpans(2) -> ScopeSpans(2) ->
  // Span(9) -> KeyValue{ key(1), AnyValue value(2) }.
  const auto resource_spans = len_fields(body, 1);
  CHECK_EQ(resource_spans.size(), static_cast<std::size_t>(1));
  const auto scope_spans = len_fields(resource_spans.at(0), 2);
  CHECK_EQ(scope_spans.size(), static_cast<std::size_t>(1));
  const auto spans = len_fields(scope_spans.at(0), 2);
  CHECK_EQ(spans.size(), static_cast<std::size_t>(1));
  const auto key_values = len_fields(spans.at(0), 9);
  CHECK_EQ(key_values.size(), static_cast<std::size_t>(2));

  bool saw_names = false;
  bool saw_positions = false;
  for (const auto & kv : key_values) {
    const auto keys = len_fields(kv, 1);
    const auto values = len_fields(kv, 2);
    CHECK_EQ(keys.size(), static_cast<std::size_t>(1));
    CHECK_EQ(values.size(), static_cast<std::size_t>(1));
    const std::string & key = keys.at(0);
    const std::string & any_value = values.at(0);

    // AnyValue.array_value (field 5) => ArrayValue; elements are AnyValue(field 1).
    const auto array_value = len_fields(any_value, 5);
    CHECK_EQ(array_value.size(), static_cast<std::size_t>(1));
    const auto elements = len_fields(array_value.at(0), 1);

    if (key == "robot.joint.name") {
      saw_names = true;
      CHECK_EQ(elements.size(), static_cast<std::size_t>(3));
      // Each element AnyValue.string_value is field 1 (wire type 2).
      const char * expected[] = {"shoulder", "elbow", "wrist"};
      for (std::size_t e = 0; e < elements.size() && e < 3; ++e) {
        const auto strs = len_fields(elements.at(e), 1);
        CHECK_EQ(strs.size(), static_cast<std::size_t>(1));
        CHECK_EQ(strs.at(0), std::string(expected[e]));
      }
    } else if (key == "robot.target.position") {
      saw_positions = true;
      CHECK_EQ(elements.size(), static_cast<std::size_t>(3));
      // Each element AnyValue.double_value is field 4 (fixed64, IEEE754 LE).
      const double expected[] = {0.25, -1.5, 3.0};
      for (std::size_t e = 0; e < elements.size() && e < 3; ++e) {
        const auto bits = fixed64_fields(elements.at(e), 4);
        CHECK_EQ(bits.size(), static_cast<std::size_t>(1));
        double d = 0.0;
        std::memcpy(&d, &bits.at(0), sizeof(d));
        CHECK_EQ(d, expected[e]);
      }
    }
  }
  CHECK(saw_names);
  CHECK(saw_positions);
}
