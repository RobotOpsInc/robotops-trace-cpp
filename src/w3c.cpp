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

#include "robotops_trace/w3c.hpp"

#include <cstdint>
#include <string>

namespace robotops
{

namespace
{

const char * kHexDigits = "0123456789abcdef";

void append_hex(std::string & out, const std::uint8_t * bytes, std::size_t count)
{
  for (std::size_t i = 0; i < count; ++i) {
    out.push_back(kHexDigits[(bytes[i] >> 4) & 0xF]);
    out.push_back(kHexDigits[bytes[i] & 0xF]);
  }
}

// Decode a single lowercase/uppercase hex nibble; -1 on a non-hex char.
int hex_nibble(char ch) noexcept
{
  if (ch >= '0' && ch <= '9') {
    return ch - '0';
  }
  if (ch >= 'a' && ch <= 'f') {
    return ch - 'a' + 10;
  }
  if (ch >= 'A' && ch <= 'F') {
    return ch - 'A' + 10;
  }
  return -1;
}

// Decode `count` bytes (2*count hex chars) starting at text[offset] into out.
// Returns false on any non-hex char.
bool decode_hex(
  std::string_view text, std::size_t offset, std::uint8_t * out, std::size_t count) noexcept
{
  for (std::size_t i = 0; i < count; ++i) {
    const int hi = hex_nibble(text[offset + 2 * i]);
    const int lo = hex_nibble(text[offset + 2 * i + 1]);
    if (hi < 0 || lo < 0) {
      return false;
    }
    out[i] = static_cast<std::uint8_t>((hi << 4) | lo);
  }
  return true;
}

}  // namespace

std::string inject(const SpanContext & context)
{
  if (!context.valid()) {
    return std::string();
  }
  std::string out;
  out.reserve(55);  // "00-" + 32 + "-" + 16 + "-" + 2
  out += "00-";
  append_hex(out, context.trace_id.data(), context.trace_id.size());
  out.push_back('-');
  append_hex(out, context.span_id.data(), context.span_id.size());
  out.push_back('-');
  out.push_back(kHexDigits[(context.trace_flags >> 4) & 0xF]);
  out.push_back(kHexDigits[context.trace_flags & 0xF]);
  return out;
}

SpanContext extract(std::string_view traceparent)
{
  // Layout: "00-<32 hex>-<16 hex>-<2 hex>" == 55 chars, dashes at 2,35,52.
  SpanContext ctx;
  if (traceparent.size() != 55) {
    return ctx;
  }
  if (traceparent[2] != '-' || traceparent[35] != '-' || traceparent[52] != '-') {
    return ctx;
  }
  // Only version "00" is supported.
  if (traceparent[0] != '0' || traceparent[1] != '0') {
    return ctx;
  }

  SpanContext parsed;
  if (!decode_hex(traceparent, 3, parsed.trace_id.data(), parsed.trace_id.size())) {
    return ctx;
  }
  if (!decode_hex(traceparent, 36, parsed.span_id.data(), parsed.span_id.size())) {
    return ctx;
  }
  const int flags_hi = hex_nibble(traceparent[53]);
  const int flags_lo = hex_nibble(traceparent[54]);
  if (flags_hi < 0 || flags_lo < 0) {
    return ctx;
  }
  parsed.trace_flags = static_cast<std::uint8_t>((flags_hi << 4) | flags_lo);
  parsed.remote = true;

  // All-zero ids are invalid per the W3C spec.
  if (!parsed.valid()) {
    return ctx;
  }
  return parsed;
}

}  // namespace robotops
