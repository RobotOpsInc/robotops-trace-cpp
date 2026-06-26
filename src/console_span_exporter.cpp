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

#include "console_span_exporter.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace robotops
{

namespace
{

// --- hand-rolled JSON helpers ------------------------------------------------
// Repurposed from the original OTLP/HTTP-JSON exporter. The default OTLP wire is
// now protobuf; this JSON serializer backs the debug/console sink only.

void append_escaped(std::string & out, const std::string & value)
{
  out.push_back('"');
  for (const char ch : value) {
    switch (ch) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(ch) < 0x20) {
          char buf[7];
          const unsigned int code = static_cast<unsigned char>(ch);
          std::snprintf(buf, sizeof(buf), "\\u%04x", code);
          out += buf;
        } else {
          out.push_back(ch);
        }
        break;
    }
  }
  out.push_back('"');
}

std::string to_hex(const std::uint8_t * bytes, std::size_t count)
{
  static const char * digits = "0123456789abcdef";
  std::string out;
  out.reserve(count * 2);
  for (std::size_t i = 0; i < count; ++i) {
    out.push_back(digits[(bytes[i] >> 4) & 0xF]);
    out.push_back(digits[bytes[i] & 0xF]);
  }
  return out;
}

bool all_zero(const std::uint8_t * bytes, std::size_t count)
{
  for (std::size_t i = 0; i < count; ++i) {
    if (bytes[i] != 0) {
      return false;
    }
  }
  return true;
}

// Serialize one attribute value as an OTLP AnyValue object.
void append_any_value(std::string & out, const AttributeValue & value)
{
  switch (value.type()) {
    case AttributeValue::Type::String:
      out += "{\"stringValue\":";
      append_escaped(out, value.string_value());
      out += "}";
      break;
    case AttributeValue::Type::Bool:
      out += value.bool_value() ? "{\"boolValue\":true}" : "{\"boolValue\":false}";
      break;
    case AttributeValue::Type::Int:
      // int64 is encoded as a decimal STRING per OTLP/JSON.
      out += "{\"intValue\":\"";
      out += std::to_string(value.int_value());
      out += "\"}";
      break;
    case AttributeValue::Type::Double: {
        out += "{\"doubleValue\":";
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.17g", value.double_value());
        out += buf;
        out += "}";
        break;
      }
  }
}

void append_key_value(std::string & out, const std::string & key, const AttributeValue & value)
{
  out += "{\"key\":";
  append_escaped(out, key);
  out += ",\"value\":";
  append_any_value(out, value);
  out += "}";
}

void append_attributes(
  std::string & out,
  const std::vector<std::pair<std::string, AttributeValue>> & attributes)
{
  out += "[";
  bool first = true;
  for (const auto & attr : attributes) {
    if (!first) {
      out += ",";
    }
    first = false;
    append_key_value(out, attr.first, attr.second);
  }
  out += "]";
}

}  // namespace

std::string ConsoleSpanExporter::serialize(
  const Resource & resource,
  const std::vector<SpanData> & spans)
{
  std::string out;
  out.reserve(256 + spans.size() * 256);

  out += "{\"resourceSpans\":[{\"resource\":{\"attributes\":[";
  bool first_attr = true;
  for (const auto & attr : resource.attributes) {
    if (!first_attr) {
      out += ",";
    }
    first_attr = false;
    append_key_value(out, attr.first, AttributeValue(attr.second));
  }
  out += "]},\"scopeSpans\":[{\"scope\":{\"name\":\"robotops-trace-cpp\",\"version\":";
  append_escaped(out, std::string(version()));
  out += "},\"spans\":[";

  bool first_span = true;
  for (const auto & span : spans) {
    if (!first_span) {
      out += ",";
    }
    first_span = false;

    out += "{\"traceId\":\"";
    out += to_hex(span.context.trace_id.data(), span.context.trace_id.size());
    out += "\",\"spanId\":\"";
    out += to_hex(span.context.span_id.data(), span.context.span_id.size());
    out += "\"";

    if (!all_zero(span.parent_span_id.data(), span.parent_span_id.size())) {
      out += ",\"parentSpanId\":\"";
      out += to_hex(span.parent_span_id.data(), span.parent_span_id.size());
      out += "\"";
    }

    out += ",\"name\":";
    append_escaped(out, span.name);
    out += ",\"kind\":";
    out += std::to_string(static_cast<int>(span.kind));

    out += ",\"startTimeUnixNano\":\"";
    out += std::to_string(span.start_unix_nano);
    out += "\",\"endTimeUnixNano\":\"";
    out += std::to_string(span.end_unix_nano);
    out += "\"";

    if (!span.attributes.empty()) {
      out += ",\"attributes\":";
      append_attributes(out, span.attributes);
    }

    if (!span.events.empty()) {
      out += ",\"events\":[";
      bool first_event = true;
      for (const auto & event : span.events) {
        if (!first_event) {
          out += ",";
        }
        first_event = false;
        out += "{\"timeUnixNano\":\"";
        out += std::to_string(event.time_unix_nano);
        out += "\",\"name\":";
        append_escaped(out, event.name);
        if (!event.attributes.empty()) {
          out += ",\"attributes\":";
          append_attributes(out, event.attributes);
        }
        out += "}";
      }
      out += "]";
    }

    if (span.status_code != StatusCode::Unset) {
      out += ",\"status\":{\"code\":";
      out += std::to_string(static_cast<int>(span.status_code));
      if (!span.status_message.empty()) {
        out += ",\"message\":";
        append_escaped(out, span.status_message);
      }
      out += "}";
    }

    out += "}";
  }

  out += "]}]}]}";
  return out;
}

bool ConsoleSpanExporter::export_spans(
  const Resource & resource,
  const std::vector<SpanData> & spans) noexcept
{
  if (spans.empty()) {
    return true;
  }
  try {
    const std::string body = serialize(resource, spans);
    std::fwrite(body.data(), 1, body.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
    return true;
  } catch (...) {
    return false;
  }
}

}  // namespace robotops
