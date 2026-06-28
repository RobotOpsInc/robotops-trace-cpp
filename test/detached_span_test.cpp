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

// ROB-443: tests for the detached (non-RAII) span API. These register into the
// shared harness registry alongside robotops_trace_tests.cpp (which owns main()).

#include <array>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "robotops_trace/trace.hpp"
#include "test_harness.hpp"

namespace
{

using robotops::Config;
using robotops::InMemorySpanExporter;
using robotops::SpanData;

constexpr std::chrono::milliseconds kFlushTimeout{2000};

void reset_env()
{
  ::unsetenv("ROBOTOPS_TRACE_ENABLED");
  ::unsetenv("ROBOTOPS_SERVICE_NAME");
  ::unsetenv("ROBOTOPS_OTLP_ENDPOINT");
  ::unsetenv("ROBOTOPS_TRACE_MAX_QUEUE");
  ::unsetenv("ROBOTOPS_TRACE_MAX_BATCH");
  ::unsetenv("ROBOTOPS_TRACE_SCHEDULE_DELAY_MS");
}

std::shared_ptr<InMemorySpanExporter> init_in_memory()
{
  reset_env();
  robotops::shutdown();  // ensure no prior state lingers
  auto mem = std::make_shared<InMemorySpanExporter>();
  Config config;
  config.service_name = "test_service";
  config.exporter = mem;
  config.schedule_delay = std::chrono::milliseconds(50);
  robotops::init(config);
  return mem;
}

const SpanData * find_span(const std::vector<SpanData> & spans, const std::string & name)
{
  for (const auto & span : spans) {
    if (span.name == name) {
      return &span;
    }
  }
  return nullptr;
}

std::size_t count_span(const std::vector<SpanData> & spans, const std::string & name)
{
  std::size_t n = 0;
  for (const auto & span : spans) {
    if (span.name == name) {
      ++n;
    }
  }
  return n;
}

// Build a synthetic, valid, sampled remote-ish parent context.
robotops::SpanContext make_parent()
{
  robotops::SpanContext ctx;
  for (std::size_t i = 0; i < ctx.trace_id.size(); ++i) {
    ctx.trace_id[i] = static_cast<std::uint8_t>(i + 1);
  }
  for (std::size_t i = 0; i < ctx.span_id.size(); ++i) {
    ctx.span_id[i] = static_cast<std::uint8_t>(0xB0 + i);
  }
  ctx.trace_flags = 0x01;
  return ctx;
}

}  // namespace

// ROB-443 #1 (the core regression BT.CPP flagged): opening — or ending — a
// detached span must NOT change current_span()/current_context() on the calling
// thread, whether or not a SpanGuard is active.
TEST_CASE(detached_open_does_not_change_thread_local_current)
{
  auto mem = init_in_memory();

  // (a) No active span: current is invalid, and stays invalid across a detached
  //     span's whole open/end lifetime.
  CHECK(!robotops::current_span().valid());
  CHECK(!robotops::current_context().valid());
  {
    robotops::DetachedSpan d = robotops::start_detached_span("detached_root");
    CHECK(d.valid());
    CHECK(d.context().valid());
    // The invariant: opening a detached span did NOT make it current.
    CHECK(!robotops::current_span().valid());
    CHECK(!robotops::current_context().valid());
    d.end();
    // Ending it also leaves the thread-local stack untouched.
    CHECK(!robotops::current_span().valid());
    CHECK(!robotops::current_context().valid());
  }

  // (b) With a SpanGuard active, current is the guard; a detached span opened
  //     alongside must NOT replace it as current (the mis-nesting BT.CPP saw).
  {
    robotops::SpanGuard guard("guard_parent");
    const auto guard_span_id = guard.span().context().span_id;
    CHECK_EQ(robotops::current_context().span_id, guard_span_id);

    robotops::DetachedSpan d = robotops::start_detached_span("detached_sibling");
    CHECK(d.valid());
    // current is STILL the guard, not the detached span.
    CHECK_EQ(robotops::current_context().span_id, guard_span_id);
    CHECK_EQ(robotops::current_span().context().span_id, guard_span_id);
    CHECK(d.context().span_id != guard_span_id);

    d.end();
    // ... and still the guard after the detached span ends.
    CHECK_EQ(robotops::current_context().span_id, guard_span_id);
  }
  robotops::shutdown();
}

// ROB-443 #2: a detached span with an explicit SpanOptions.parent nests under it
// (child trace_id == parent trace_id, child parent_span_id == parent span_id).
TEST_CASE(detached_explicit_parent_nests)
{
  auto mem = init_in_memory();
  const robotops::SpanContext parent = make_parent();

  {
    robotops::SpanOptions opts;
    opts.parent = &parent;
    robotops::DetachedSpan d = robotops::start_detached_span("detached_child", opts);
    CHECK(d.valid());
    CHECK_EQ(d.context().trace_id, parent.trace_id);
    d.end();
  }
  CHECK(robotops::force_flush(kFlushTimeout));

  const auto spans = mem->spans();
  const SpanData * child = find_span(spans, "detached_child");
  CHECK(child != nullptr);
  if (child != nullptr) {
    CHECK_EQ(child->context.trace_id, parent.trace_id);
    CHECK_EQ(child->parent_span_id, parent.span_id);
    CHECK(child->context.span_id != parent.span_id);
  }
  robotops::shutdown();
}

// ROB-443 #3: a detached span opened with NO explicit parent while a SpanGuard is
// active picks up the current context as its parent — but does NOT itself become
// current.
TEST_CASE(detached_default_parent_uses_current_but_not_become_current)
{
  auto mem = init_in_memory();
  {
    robotops::SpanGuard guard("guard_outer");
    const auto guard_span_id = guard.span().context().span_id;
    const auto guard_trace_id = guard.span().context().trace_id;

    robotops::DetachedSpan d = robotops::start_detached_span("detached_under_guard");
    CHECK(d.valid());
    // Inherited the active trace + parented under the guard...
    CHECK_EQ(d.context().trace_id, guard_trace_id);
    // ... but did NOT become current.
    CHECK_EQ(robotops::current_context().span_id, guard_span_id);
    d.end();
  }
  CHECK(robotops::force_flush(kFlushTimeout));

  const auto spans = mem->spans();
  const SpanData * child = find_span(spans, "detached_under_guard");
  const SpanData * outer = find_span(spans, "guard_outer");
  CHECK(child != nullptr && outer != nullptr);
  if (child != nullptr && outer != nullptr) {
    CHECK_EQ(child->context.trace_id, outer->context.trace_id);
    CHECK_EQ(child->parent_span_id, outer->context.span_id);
  }
  robotops::shutdown();
}

// ROB-443 #4: attributes/status/events survive to the exported SpanData; end()
// finalizes; double-end (and end-then-dtor) export exactly once; disabled is a
// no-op.
TEST_CASE(detached_attrs_status_events_double_end_and_disabled)
{
  auto mem = init_in_memory();
  {
    robotops::DetachedSpan d = robotops::start_detached_span("detached_rich");
    d.set_attribute("k.str", "v");
    d.set_attribute("k.int", static_cast<std::int64_t>(42));
    d.add_event("evt", {{"reason", "x"}});
    d.set_status(robotops::StatusCode::Error, "bad");
    d.end();
    d.end();  // double-end MUST be safe and MUST NOT export a second copy.
  }           // dtor runs after an explicit end() — also a safe no-op.
  CHECK(robotops::force_flush(kFlushTimeout));

  const auto spans = mem->spans();
  CHECK_EQ(count_span(spans, "detached_rich"), static_cast<std::size_t>(1));
  const SpanData * rich = find_span(spans, "detached_rich");
  CHECK(rich != nullptr);
  if (rich != nullptr) {
    CHECK_EQ(rich->attributes.size(), static_cast<std::size_t>(2));
    CHECK_EQ(rich->status_code, robotops::StatusCode::Error);
    CHECK_EQ(rich->status_message, std::string("bad"));
    CHECK_EQ(rich->events.size(), static_cast<std::size_t>(1));
    if (!rich->events.empty()) {
      CHECK_EQ(rich->events[0].name, std::string("evt"));
    }
  }
  robotops::shutdown();

  // Disabled (no init) => an invalid handle whose every operation is a no-op.
  reset_env();
  robotops::shutdown();
  {
    robotops::DetachedSpan d = robotops::start_detached_span("detached_disabled");
    CHECK(!d.valid());
    CHECK(!d.context().valid());
    d.set_attribute("x", "y");   // safe no-op
    d.add_event("noop");         // safe no-op
    d.set_status(robotops::StatusCode::Ok);
    d.end();                     // safe no-op
  }
  CHECK(robotops::force_flush(kFlushTimeout));
}
