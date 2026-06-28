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

// The 10 required tests for the SDK core (ROB-419 spec §Tests). These run on
// the standalone (no-ROS) path via a header-only harness — no gtest needed.

#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
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

// Init the tracer with an InMemory exporter and return it. Always starts from a
// clean global state so tests are order-independent.
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

bool is_zero(const std::array<std::uint8_t, 8> & id)
{
  for (std::uint8_t byte : id) {
    if (byte != 0) {
      return false;
    }
  }
  return true;
}

}  // namespace

// 1. version() returns "0.1.0".
TEST_CASE(version_string)
{
  CHECK_EQ(std::strcmp(robotops::version(), "0.1.0"), 0);
}

// 2. Root span mints a valid, sampled trace; parent_span_id is zero.
TEST_CASE(root_span_is_valid_sampled_and_parentless)
{
  auto mem = init_in_memory();
  {
    robotops::SpanGuard root("root");
    CHECK(root.span().valid());
    CHECK(root.span().context().valid());
    CHECK(root.span().context().sampled());
  }
  CHECK(robotops::force_flush(kFlushTimeout));

  const auto spans = mem->spans();
  const SpanData * root = find_span(spans, "root");
  CHECK(root != nullptr);
  if (root != nullptr) {
    CHECK(root->context.valid());
    CHECK(root->context.sampled());
    CHECK(is_zero(root->parent_span_id));
  }
  robotops::shutdown();
}

// 3. Nesting: an inner span inherits the parent's trace_id and sets
//    parent_span_id to the outer's span_id.
TEST_CASE(nested_span_inherits_trace_and_parent)
{
  auto mem = init_in_memory();
  {
    robotops::SpanGuard outer("outer");
    {
      robotops::SpanGuard inner("inner");
      CHECK_EQ(inner.span().context().trace_id, outer.span().context().trace_id);
    }
  }
  CHECK(robotops::force_flush(kFlushTimeout));

  const auto spans = mem->spans();
  const SpanData * outer = find_span(spans, "outer");
  const SpanData * inner = find_span(spans, "inner");
  CHECK(outer != nullptr);
  CHECK(inner != nullptr);
  if (outer != nullptr && inner != nullptr) {
    CHECK_EQ(inner->context.trace_id, outer->context.trace_id);
    CHECK_EQ(inner->parent_span_id, outer->context.span_id);
    CHECK(is_zero(outer->parent_span_id));
  }
  robotops::shutdown();
}

// 4. Sibling spans under the same parent share trace_id, differ in span_id.
TEST_CASE(sibling_spans_share_trace_differ_span)
{
  auto mem = init_in_memory();
  {
    robotops::SpanGuard outer("outer");
    {robotops::SpanGuard a("sib_a");}
    {robotops::SpanGuard b("sib_b");}
  }
  CHECK(robotops::force_flush(kFlushTimeout));

  const auto spans = mem->spans();
  const SpanData * outer = find_span(spans, "outer");
  const SpanData * a = find_span(spans, "sib_a");
  const SpanData * b = find_span(spans, "sib_b");
  CHECK(outer != nullptr && a != nullptr && b != nullptr);
  if (outer != nullptr && a != nullptr && b != nullptr) {
    CHECK_EQ(a->context.trace_id, outer->context.trace_id);
    CHECK_EQ(b->context.trace_id, outer->context.trace_id);
    CHECK_EQ(a->parent_span_id, outer->context.span_id);
    CHECK_EQ(b->parent_span_id, outer->context.span_id);
    CHECK(a->context.span_id != b->context.span_id);
  }
  robotops::shutdown();
}

// 5. Thread-local isolation: spans on two threads get independent trace trees.
TEST_CASE(thread_local_isolation)
{
  auto mem = init_in_memory();
  std::thread ta([] {robotops::SpanGuard g("thread_a");});
  std::thread tb([] {robotops::SpanGuard g("thread_b");});
  ta.join();
  tb.join();
  CHECK(robotops::force_flush(kFlushTimeout));

  const auto spans = mem->spans();
  const SpanData * a = find_span(spans, "thread_a");
  const SpanData * b = find_span(spans, "thread_b");
  CHECK(a != nullptr && b != nullptr);
  if (a != nullptr && b != nullptr) {
    CHECK(a->context.trace_id != b->context.trace_id);
    CHECK(is_zero(a->parent_span_id));
    CHECK(is_zero(b->parent_span_id));
  }
  robotops::shutdown();
}

// 6. Async carry: capture on thread A, attach on thread B => B nests under A.
TEST_CASE(async_context_carry_across_threads)
{
  auto mem = init_in_memory();
  robotops::Context captured;
  {
    robotops::SpanGuard outer("carry_outer");
    captured = robotops::capture_context();
    CHECK(captured.valid());
  }

  std::thread worker([&] {
      robotops::ScopedContext scope(captured);
      robotops::SpanGuard child("carry_child");
    });
  worker.join();
  CHECK(robotops::force_flush(kFlushTimeout));

  const auto spans = mem->spans();
  const SpanData * child = find_span(spans, "carry_child");
  CHECK(child != nullptr);
  if (child != nullptr) {
    CHECK_EQ(child->context.trace_id, captured.span_context().trace_id);
    CHECK_EQ(child->parent_span_id, captured.span_context().span_id);
  }
  robotops::shutdown();
}

// 7. Attributes / status / events survive into the exported SpanData.
TEST_CASE(attributes_status_events_survive_export)
{
  auto mem = init_in_memory();
  {
    robotops::SpanGuard g("rich");
    auto span = g.span();
    span.set_attribute("robot.action.result", "SUCCEEDED");
    span.set_attribute("retry", true);
    span.set_attribute("count", static_cast<std::int64_t>(7));
    span.set_attribute("dur", 1.5);
    span.add_event("grasp aborted", {{"reason", "slip"}});
    span.set_status(robotops::StatusCode::Error, "boom");
  }
  CHECK(robotops::force_flush(kFlushTimeout));

  const auto spans = mem->spans();
  const SpanData * rich = find_span(spans, "rich");
  CHECK(rich != nullptr);
  if (rich != nullptr) {
    CHECK_EQ(rich->attributes.size(), static_cast<std::size_t>(4));
    CHECK_EQ(rich->status_code, robotops::StatusCode::Error);
    CHECK_EQ(rich->status_message, std::string("boom"));
    CHECK_EQ(rich->events.size(), static_cast<std::size_t>(1));

    bool found_result = false;
    bool found_retry = false;
    bool found_count = false;
    bool found_dur = false;
    for (const auto & attr : rich->attributes) {
      if (attr.first == "robot.action.result") {
        found_result = true;
        CHECK_EQ(attr.second.type(), robotops::AttributeValue::Type::String);
        CHECK_EQ(attr.second.string_value(), std::string("SUCCEEDED"));
      } else if (attr.first == "retry") {
        found_retry = true;
        CHECK_EQ(attr.second.type(), robotops::AttributeValue::Type::Bool);
        CHECK(attr.second.bool_value());
      } else if (attr.first == "count") {
        found_count = true;
        CHECK_EQ(attr.second.type(), robotops::AttributeValue::Type::Int);
        CHECK_EQ(attr.second.int_value(), static_cast<std::int64_t>(7));
      } else if (attr.first == "dur") {
        found_dur = true;
        CHECK_EQ(attr.second.type(), robotops::AttributeValue::Type::Double);
      }
    }
    CHECK(found_result && found_retry && found_count && found_dur);
    if (!rich->events.empty()) {
      CHECK_EQ(rich->events[0].name, std::string("grasp aborted"));
      CHECK_EQ(rich->events[0].attributes.size(), static_cast<std::size_t>(1));
    }
  }
  robotops::shutdown();
}

// 7b. (ROB-444) Array-valued attributes set on a live span survive into the
//     exported SpanData with their element types + values intact — the case the
//     ros2_control integration needed (joint names as a real string[], target
//     positions as a real double[], no lossy comma-join).
TEST_CASE(array_attributes_survive_export)
{
  using robotops::AttributeValue;
  auto mem = init_in_memory();
  {
    robotops::SpanGuard g("arrays");
    auto span = g.span();
    // Owning-vector form...
    span.set_attribute(
      "robot.joint.name", std::vector<std::string>{"shoulder", "elbow", "wrist"});
    // ...and the braced-list convenience form (must resolve to a string[]).
    span.set_attribute("robot.joint.alias", AttributeValue({"j0", "j1"}));
    span.set_attribute(
      "robot.target.position", std::vector<double>{0.25, -1.5, 3.0});
    span.set_attribute(
      "robot.joint.index", std::vector<std::int64_t>{0, 1, 2, 3});
    span.set_attribute("robot.joint.enabled", std::vector<bool>{true, false, true});
  }
  CHECK(robotops::force_flush(kFlushTimeout));

  const auto spans = mem->spans();
  const SpanData * s = find_span(spans, "arrays");
  CHECK(s != nullptr);
  if (s != nullptr) {
    CHECK_EQ(s->attributes.size(), static_cast<std::size_t>(5));
    bool ok_names = false;
    bool ok_alias = false;
    bool ok_pos = false;
    bool ok_index = false;
    bool ok_enabled = false;
    for (const auto & attr : s->attributes) {
      if (attr.first == "robot.joint.name") {
        ok_names = true;
        CHECK_EQ(attr.second.type(), AttributeValue::Type::StringArray);
        const auto & v = attr.second.string_array_value();
        CHECK_EQ(v.size(), static_cast<std::size_t>(3));
        if (v.size() == 3) {
          CHECK_EQ(v[0], std::string("shoulder"));
          CHECK_EQ(v[2], std::string("wrist"));
        }
      } else if (attr.first == "robot.joint.alias") {
        ok_alias = true;
        CHECK_EQ(attr.second.type(), AttributeValue::Type::StringArray);
        CHECK_EQ(attr.second.string_array_value().size(), static_cast<std::size_t>(2));
      } else if (attr.first == "robot.target.position") {
        ok_pos = true;
        CHECK_EQ(attr.second.type(), AttributeValue::Type::DoubleArray);
        const auto & v = attr.second.double_array_value();
        CHECK_EQ(v.size(), static_cast<std::size_t>(3));
        if (v.size() == 3) {
          CHECK_EQ(v[1], -1.5);
        }
      } else if (attr.first == "robot.joint.index") {
        ok_index = true;
        CHECK_EQ(attr.second.type(), AttributeValue::Type::IntArray);
        CHECK_EQ(attr.second.int_array_value().size(), static_cast<std::size_t>(4));
      } else if (attr.first == "robot.joint.enabled") {
        ok_enabled = true;
        CHECK_EQ(attr.second.type(), AttributeValue::Type::BoolArray);
        const auto & v = attr.second.bool_array_value();
        CHECK_EQ(v.size(), static_cast<std::size_t>(3));
        if (v.size() == 3) {
          CHECK(v[0]);
          CHECK(!v[1]);
          CHECK(v[2]);
        }
      }
    }
    CHECK(ok_names && ok_alias && ok_pos && ok_index && ok_enabled);
  }
  robotops::shutdown();
}

// 8. W3C round-trip: inject(ctx) then extract() yields equal ids/flags,
//    remote=true.
TEST_CASE(w3c_traceparent_round_trip)
{
  robotops::SpanContext ctx;
  for (std::size_t i = 0; i < ctx.trace_id.size(); ++i) {
    ctx.trace_id[i] = static_cast<std::uint8_t>(i + 1);
  }
  for (std::size_t i = 0; i < ctx.span_id.size(); ++i) {
    ctx.span_id[i] = static_cast<std::uint8_t>(0xA0 + i);
  }
  ctx.trace_flags = 0x01;

  const std::string header = robotops::inject(ctx);
  CHECK_EQ(header.size(), static_cast<std::size_t>(55));
  CHECK_EQ(header.substr(0, 3), std::string("00-"));

  const robotops::SpanContext parsed = robotops::extract(header);
  CHECK(parsed.valid());
  CHECK(parsed.remote);
  CHECK_EQ(parsed.trace_id, ctx.trace_id);
  CHECK_EQ(parsed.span_id, ctx.span_id);
  CHECK_EQ(parsed.trace_flags, ctx.trace_flags);

  // Malformed inputs yield an invalid (non-remote) context.
  CHECK(!robotops::extract("garbage").valid());
  CHECK(!robotops::extract("00-zzzz").valid());
}

// 9. Disabled (kill switch / no init) => SpanGuard is a no-op, nothing exported.
TEST_CASE(disabled_is_a_no_op)
{
  // (a) No init at all.
  reset_env();
  robotops::shutdown();
  {
    robotops::SpanGuard g("should_not_exist");
    CHECK(!g.span().valid());
    CHECK(!robotops::current_span().valid());
    CHECK(!robotops::current_context().valid());
  }
  CHECK(robotops::force_flush(kFlushTimeout));

  // (b) ROBOTOPS_TRACE_ENABLED=0 hard-disables even with an explicit exporter.
  ::setenv("ROBOTOPS_TRACE_ENABLED", "0", 1);
  auto mem = std::make_shared<InMemorySpanExporter>();
  Config config;
  config.exporter = mem;
  robotops::init(config);
  {
    robotops::SpanGuard g("also_should_not_exist");
    CHECK(!g.span().valid());
    CHECK(!robotops::current_span().valid());
  }
  CHECK(robotops::force_flush(kFlushTimeout));
  CHECK_EQ(mem->size(), static_cast<std::size_t>(0));
  robotops::shutdown();
  ::unsetenv("ROBOTOPS_TRACE_ENABLED");
}

// 10. InMemorySpanExporter wiring via Config.exporter; force_flush drains.
TEST_CASE(in_memory_exporter_wiring_and_flush)
{
  auto mem = init_in_memory();
  constexpr int kSpanCount = 5;
  for (int i = 0; i < kSpanCount; ++i) {
    robotops::SpanGuard g("batched");
  }
  CHECK(robotops::force_flush(kFlushTimeout));
  CHECK_EQ(mem->size(), static_cast<std::size_t>(kSpanCount));
  robotops::shutdown();
}

int main()
{
  return robotops_test::run_all();
}
