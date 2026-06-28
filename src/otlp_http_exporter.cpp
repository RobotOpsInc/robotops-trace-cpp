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

#include "otlp_http_exporter.hpp"

#include <curl/curl.h>

#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "detail/log.hpp"

namespace robotops
{

namespace
{

std::once_flag g_curl_global_once;

void ensure_curl_global() noexcept
{
  std::call_once(g_curl_global_once, [] {curl_global_init(CURL_GLOBAL_DEFAULT);});
}

// Normalize "host:port" / "host:port/" into a clean "<endpoint>/v1/traces".
std::string make_traces_url(std::string endpoint)
{
  while (!endpoint.empty() && endpoint.back() == '/') {
    endpoint.pop_back();
  }
  return endpoint + "/v1/traces";
}

constexpr char kUnixScheme[] = "unix://";

// Parse the configured endpoint into (traces_url, unix_socket_path) (ROB-441).
//   "unix:///abs/path" => UDS: socket path = "/abs/path", and the HTTP request
//     uses a dummy "http://localhost/v1/traces" authority while libcurl's
//     CURLOPT_UNIX_SOCKET_PATH routes the POST over the socket.
//   anything else (e.g. "http://host:port") => TCP: socket path empty,
//     traces_url = "<endpoint>/v1/traces".
// The scheme parsing mirrors the Python exporter + the agent receiver contract.
std::pair<std::string, std::string> parse_endpoint(const std::string & endpoint)
{
  const std::size_t prefix = std::strlen(kUnixScheme);
  if (endpoint.compare(0, prefix, kUnixScheme) == 0) {
    // Everything after "unix://" is the (absolute) socket path; for the
    // canonical "unix:///run/..." form this keeps the leading '/'.
    return {std::string("http://localhost/v1/traces"), endpoint.substr(prefix)};
  }
  return {make_traces_url(endpoint), std::string()};
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

// --- hand-rolled protobuf wire writer ---------------------------------------
// Wire types: 0=varint, 1=64-bit LE, 2=length-delimited, 5=32-bit LE.
// Tag byte = (field_number << 3) | wire_type. Nested messages are serialized
// innermost-first into their own buffer, then length-prefixed into the parent.

void write_varint(std::string & out, std::uint64_t value)
{
  while (value >= 0x80) {
    out.push_back(static_cast<char>((value & 0x7F) | 0x80));
    value >>= 7;
  }
  out.push_back(static_cast<char>(value));
}

void write_tag(std::string & out, std::uint32_t field, std::uint32_t wire_type)
{
  write_varint(out, (static_cast<std::uint64_t>(field) << 3) | wire_type);
}

// A length-delimited field (wire type 2): tag, varint(len), bytes.
void write_len_field(std::string & out, std::uint32_t field, const std::string & bytes)
{
  write_tag(out, field, 2);
  write_varint(out, bytes.size());
  out.append(bytes);
}

// A length-delimited field carrying raw bytes (e.g. trace_id / span_id, which go
// on the wire RAW — not hex).
void write_bytes_field(
  std::string & out, std::uint32_t field, const std::uint8_t * data, std::size_t len)
{
  write_tag(out, field, 2);
  write_varint(out, len);
  out.append(reinterpret_cast<const char *>(data), len);
}

void write_string_field(std::string & out, std::uint32_t field, const std::string & value)
{
  write_len_field(out, field, value);
}

void write_varint_field(std::string & out, std::uint32_t field, std::uint64_t value)
{
  write_tag(out, field, 0);
  write_varint(out, value);
}

// fixed64 (wire type 1): tag, 8 bytes little-endian.
void write_fixed64_field(std::string & out, std::uint32_t field, std::uint64_t value)
{
  write_tag(out, field, 1);
  for (int i = 0; i < 8; ++i) {
    out.push_back(static_cast<char>(value & 0xFF));
    value >>= 8;
  }
}

void write_double_field(std::string & out, std::uint32_t field, double value)
{
  std::uint64_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));   // IEEE754 little-endian
  write_fixed64_field(out, field, bits);
}

// OTLP AnyValue: set exactly one of string(1)/bool(2)/int64(3)/double(4), or —
// for arrays — array_value(5) => ArrayValue{ repeated AnyValue values=1 }, where
// every element is itself an AnyValue (encoded by recursing into the scalar form).
std::string encode_any_value(const AttributeValue & value)
{
  std::string out;
  switch (value.type()) {
    case AttributeValue::Type::String:
      write_string_field(out, 1, value.string_value());
      break;
    case AttributeValue::Type::Bool:
      write_varint_field(out, 2, value.bool_value() ? 1u : 0u);
      break;
    case AttributeValue::Type::Int:
      // Plain varint int64 (NOT zigzag); two's-complement reinterpreted as u64.
      write_varint_field(out, 3, static_cast<std::uint64_t>(value.int_value()));
      break;
    case AttributeValue::Type::Double:
      write_double_field(out, 4, value.double_value());
      break;
    case AttributeValue::Type::StringArray: {
        std::string arr;
        for (const auto & elem : value.string_array_value()) {
          write_len_field(arr, 1, encode_any_value(AttributeValue(elem)));
        }
        write_len_field(out, 5, arr);
        break;
      }
    case AttributeValue::Type::BoolArray: {
        std::string arr;
        for (const bool elem : value.bool_array_value()) {
          write_len_field(arr, 1, encode_any_value(AttributeValue(elem)));
        }
        write_len_field(out, 5, arr);
        break;
      }
    case AttributeValue::Type::IntArray: {
        std::string arr;
        for (const std::int64_t elem : value.int_array_value()) {
          write_len_field(arr, 1, encode_any_value(AttributeValue(elem)));
        }
        write_len_field(out, 5, arr);
        break;
      }
    case AttributeValue::Type::DoubleArray: {
        std::string arr;
        for (const double elem : value.double_array_value()) {
          write_len_field(arr, 1, encode_any_value(AttributeValue(elem)));
        }
        write_len_field(out, 5, arr);
        break;
      }
  }
  return out;
}

// OTLP KeyValue: 1=string key, 2=AnyValue value.
std::string encode_key_value(const std::string & key, const AttributeValue & value)
{
  std::string out;
  write_string_field(out, 1, key);
  write_len_field(out, 2, encode_any_value(value));
  return out;
}

// OTLP Span.Event: 1=fixed64 time_unix_nano, 2=string name, 3=repeated KeyValue.
std::string encode_event(const EventData & event)
{
  std::string out;
  write_fixed64_field(out, 1, event.time_unix_nano);
  write_string_field(out, 2, event.name);
  for (const auto & attr : event.attributes) {
    write_len_field(out, 3, encode_key_value(attr.first, attr.second));
  }
  return out;
}

// OTLP Status: 2=string message (omit if empty), 3=StatusCode code (omit if 0).
std::string encode_status(const SpanData & span)
{
  std::string out;
  if (!span.status_message.empty()) {
    write_string_field(out, 2, span.status_message);
  }
  if (span.status_code != StatusCode::Unset) {
    write_varint_field(out, 3, static_cast<std::uint64_t>(span.status_code));
  }
  return out;
}

// OTLP Span (selected fields per the ROB-438 field map).
std::string encode_span(const SpanData & span)
{
  std::string out;
  write_bytes_field(out, 1, span.context.trace_id.data(), span.context.trace_id.size());
  write_bytes_field(out, 2, span.context.span_id.data(), span.context.span_id.size());
  if (!all_zero(span.parent_span_id.data(), span.parent_span_id.size())) {
    write_bytes_field(out, 4, span.parent_span_id.data(), span.parent_span_id.size());
  }
  write_string_field(out, 5, span.name);
  write_varint_field(out, 6, static_cast<std::uint64_t>(span.kind));
  write_fixed64_field(out, 7, span.start_unix_nano);
  write_fixed64_field(out, 8, span.end_unix_nano);
  for (const auto & attr : span.attributes) {
    write_len_field(out, 9, encode_key_value(attr.first, attr.second));
  }
  for (const auto & event : span.events) {
    write_len_field(out, 11, encode_event(event));
  }
  // Omit the whole Status message when it is Unset with no message.
  if (span.status_code != StatusCode::Unset || !span.status_message.empty()) {
    write_len_field(out, 15, encode_status(span));
  }
  return out;
}

// OTLP InstrumentationScope: 1=string name, 2=string version.
std::string encode_scope()
{
  std::string out;
  write_string_field(out, 1, "robotops-trace-cpp");
  write_string_field(out, 2, std::string(version()));
  return out;
}

// OTLP Resource: 1=repeated KeyValue attributes (string values only here).
std::string encode_resource(const Resource & resource)
{
  std::string out;
  for (const auto & attr : resource.attributes) {
    write_len_field(out, 1, encode_key_value(attr.first, AttributeValue(attr.second)));
  }
  return out;
}

std::size_t discard_body(void * /*data*/, std::size_t size, std::size_t nmemb, void * /*user*/)
{
  return size * nmemb;
}

}  // namespace

std::string OtlpHttpExporter::serialize(
  const Resource & resource,
  const std::vector<SpanData> & spans)
{
  // ScopeSpans: 1=InstrumentationScope scope, 2=repeated Span spans.
  std::string scope_spans;
  write_len_field(scope_spans, 1, encode_scope());
  for (const auto & span : spans) {
    write_len_field(scope_spans, 2, encode_span(span));
  }

  // ResourceSpans: 1=Resource resource, 2=repeated ScopeSpans scope_spans.
  std::string resource_spans;
  write_len_field(resource_spans, 1, encode_resource(resource));
  write_len_field(resource_spans, 2, scope_spans);

  // ExportTraceServiceRequest: 1=repeated ResourceSpans resource_spans.
  std::string out;
  write_len_field(out, 1, resource_spans);
  return out;
}

OtlpHttpExporter::OtlpHttpExporter(std::string endpoint)
{
  auto parsed = parse_endpoint(endpoint);
  traces_url_ = std::move(parsed.first);
  unix_socket_path_ = std::move(parsed.second);
  ensure_curl_global();
  curl_ = curl_easy_init();
  if (curl_ == nullptr) {
    detail::log_warn("curl_easy_init failed; OTLP exporter will drop all batches");
  }
}

OtlpHttpExporter::~OtlpHttpExporter()
{
  shutdown();
}

void OtlpHttpExporter::shutdown() noexcept
{
  std::lock_guard<std::mutex> lock(curl_mutex_);
  if (curl_ != nullptr) {
    curl_easy_cleanup(static_cast<CURL *>(curl_));
    curl_ = nullptr;
  }
}

bool OtlpHttpExporter::export_spans(
  const Resource & resource,
  const std::vector<SpanData> & spans) noexcept
{
  if (spans.empty()) {
    return true;
  }
  try {
    const std::string body = serialize(resource, spans);

    std::lock_guard<std::mutex> lock(curl_mutex_);
    CURL * handle = static_cast<CURL *>(curl_);
    if (handle == nullptr) {
      return false;
    }

    curl_easy_reset(handle);

    struct curl_slist * headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/x-protobuf");

    curl_easy_setopt(handle, CURLOPT_URL, traces_url_.c_str());
    // ROB-441: when the endpoint was "unix://...", route this otherwise-normal
    // HTTP POST over the Unix-domain socket. The dummy "http://localhost"
    // authority in traces_url_ supplies the Host/path; the socket option does
    // the real connect. A missing/dead socket fails the connect within the
    // bounded timeouts below — identical best-effort drop to the TCP path.
    if (!unix_socket_path_.empty()) {
      curl_easy_setopt(handle, CURLOPT_UNIX_SOCKET_PATH, unix_socket_path_.c_str());
    }
    curl_easy_setopt(handle, CURLOPT_POST, 1L);
    // The protobuf body contains embedded NULs, so the size MUST be explicit —
    // libcurl cannot strlen() it.
    curl_easy_setopt(
      handle, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
    curl_easy_setopt(handle, CURLOPT_POSTFIELDS, body.data());
    curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT_MS, 1000L);
    curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS, 5000L);
    curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, &discard_body);

    const CURLcode rc = curl_easy_perform(handle);
    long status = 0;  // NOLINT(runtime/int) — libcurl's getinfo writes a long
    if (rc == CURLE_OK) {
      curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);
    }
    curl_slist_free_all(headers);

    if (rc != CURLE_OK) {
      detail::log_debug("OTLP POST transport error; batch dropped");
      return false;
    }
    if (status < 200 || status >= 300) {
      detail::log_debug("OTLP POST non-2xx response; batch dropped");
      return false;
    }
    return true;
  } catch (...) {
    return false;
  }
}

}  // namespace robotops
