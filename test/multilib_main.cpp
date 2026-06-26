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

// Multi-lib nesting proof (ROB-439).
//
// One executable, two separate shared libraries (libmultilib_a.so /
// libmultilib_b.so), each independently linking the SDK core .so. We open a span
// inside lib A, then — while it is still live — open a span inside lib B, and
// assert B nested under A: same trace_id, B.parent_span_id == A.span_id, A is a
// root. That can only hold if all three DSOs share ONE copy of the core's
// thread-local active-span stack, i.e. the core is a single SHARED object. Prints
// the captured ids and a machine-checkable PASS line; exits non-zero on failure
// so ctest fails loudly.

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "robotops_trace/trace.hpp"
#include "multilib.hpp"

namespace
{

bool is_zero_span_id(const std::array<std::uint8_t, 8> & id)
{
  for (std::uint8_t byte : id) {
    if (byte != 0) {
      return false;
    }
  }
  return true;
}

std::string span_id_hex(const std::array<std::uint8_t, 8> & id)
{
  static const char * digits = "0123456789abcdef";
  std::string out;
  out.reserve(16);
  for (std::uint8_t byte : id) {
    out.push_back(digits[byte >> 4]);
    out.push_back(digits[byte & 0x0F]);
  }
  return out;
}

const robotops::SpanData * find_span(
  const std::vector<robotops::SpanData> & spans, const std::string & name)
{
  for (const auto & span : spans) {
    if (span.name == name) {
      return &span;
    }
  }
  return nullptr;
}

}  // namespace

int main()
{
  auto mem = std::make_shared<robotops::InMemorySpanExporter>();
  robotops::Config cfg;
  cfg.service_name = "multilib_test";
  cfg.exporter = mem;
  cfg.schedule_delay = std::chrono::milliseconds(50);
  robotops::init(cfg);

  // Open A (kept live), then B while A is still open, then close B then A.
  const robotops::SpanContext ctx_a = robotops_multilib::lib_a_open();
  const robotops::SpanContext ctx_b = robotops_multilib::lib_b_open();
  robotops_multilib::lib_b_close();
  robotops_multilib::lib_a_close();

  robotops::force_flush(std::chrono::milliseconds(2000));
  const auto spans = mem->spans();
  robotops::shutdown();

  const robotops::SpanData * a = find_span(spans, "lib_a_span");
  const robotops::SpanData * b = find_span(spans, "lib_b_span");

  std::printf(
    "lib A: trace_id=%s span_id=%s\n",
    ctx_a.trace_id_hex().c_str(), ctx_a.span_id_hex().c_str());
  std::printf(
    "lib B: trace_id=%s span_id=%s\n",
    ctx_b.trace_id_hex().c_str(), ctx_b.span_id_hex().c_str());

  bool ok = true;
  auto fail = [&ok](const char * why) {
      ok = false;
      std::printf("    FAIL: %s\n", why);
    };

  if (a == nullptr) {fail("lib_a_span was not exported");}
  if (b == nullptr) {fail("lib_b_span was not exported");}
  if (!ctx_a.valid()) {fail("lib A context invalid");}
  if (!ctx_b.valid()) {fail("lib B context invalid");}

  // The crux: same trace, B parents under A, A is a root.
  if (ctx_a.trace_id != ctx_b.trace_id) {
    fail("trace_id differs across libs (separate tracer state — STATIC core?)");
  }
  if (b != nullptr) {
    std::printf(
      "lib B parent_span_id=%s (expect A span_id=%s)\n",
      span_id_hex(b->parent_span_id).c_str(), ctx_a.span_id_hex().c_str());
    if (b->parent_span_id != ctx_a.span_id) {
      fail("lib B did not parent under lib A's span");
    }
  }
  if (a != nullptr && !is_zero_span_id(a->parent_span_id)) {
    fail("lib A span is not a root");
  }

  if (ok) {
    std::printf("MULTILIB SHARED-STATE OK: B nested under A across two .so + core\n");
    return 0;
  }
  std::printf("MULTILIB SHARED-STATE FAILED\n");
  return 1;
}
