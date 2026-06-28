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

#include "robotops_trace/span.hpp"

#include <string>
#include <utility>

#include "robotops_trace/exporter.hpp"
#include "detail/clock.hpp"
#include "detail/id_generator.hpp"
#include "detail/span_record.hpp"
#include "detail/thread_context.hpp"
#include "global.hpp"

namespace robotops
{

namespace
{
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

bool any_nonzero(const std::uint8_t * bytes, std::size_t count) noexcept
{
  for (std::size_t i = 0; i < count; ++i) {
    if (bytes[i] != 0) {
      return true;
    }
  }
  return false;
}

// Mint a fresh open-span record from a name + options. Resolves the parent the
// same way for every span primitive: an explicit, valid SpanOptions.parent wins;
// otherwise the thread-local current context at open time; otherwise this is a
// brand-new root trace. Shared by SpanGuard and the detached-span API so the two
// mint *identical* records — the ONLY difference between them is that SpanGuard
// additionally pushes/pops the thread-local stack. May throw (allocation); the
// callers are noexcept and translate any throw into an inactive handle.
detail::SpanRecord * open_span_record(std::string_view name, const SpanOptions & opts)
{
  SpanContext parent;
  if (opts.parent != nullptr && opts.parent->valid()) {
    parent = *opts.parent;
  } else {
    parent = detail::current_context_value();
  }

  auto * record = new detail::SpanRecord();
  if (parent.valid()) {
    record->context.trace_id = parent.trace_id;
    record->context.trace_flags = parent.trace_flags;  // inherit sampled bit
    record->parent_span_id = parent.span_id;
  } else {
    record->context.trace_id = detail::generate_trace_id();
    record->context.trace_flags = 0x01;  // everything is sampled (v0)
    record->parent_span_id = {};         // root => zero parent
  }
  record->context.span_id = detail::generate_span_id();
  record->context.remote = false;
  record->name = std::string(name);
  record->kind = opts.kind;
  record->start_unix_nano = detail::now_unix_nano();
  for (const auto & attr : opts.attributes) {
    record->attributes.emplace_back(std::string(attr.first), attr.second);
  }
  return record;
}

// Finalize an open record into an immutable SpanData, hand it to the processor,
// and delete the record. Best-effort and never throws. Does NOT touch the
// thread-local stack — callers manage that (SpanGuard pops first; detached spans
// never pushed in the first place).
void finalize_and_submit(detail::SpanRecord * record) noexcept
{
  try {
    SpanData data;
    data.context = record->context;
    data.parent_span_id = record->parent_span_id;
    data.name = std::move(record->name);
    data.kind = record->kind;
    data.start_unix_nano = record->start_unix_nano;
    data.end_unix_nano = detail::now_unix_nano();
    data.status_code = record->status_code;
    data.status_message = std::move(record->status_message);
    data.attributes = std::move(record->attributes);
    data.events = std::move(record->events);
    global::submit(std::move(data));
  } catch (...) {
    // best-effort — never throw out of a destructor / noexcept path.
  }
  delete record;
}
}  // namespace

// ---------------------------------------------------------------------------
// SpanContext
// ---------------------------------------------------------------------------

bool SpanContext::valid() const noexcept
{
  return any_nonzero(trace_id.data(), trace_id.size()) &&
         any_nonzero(span_id.data(), span_id.size());
}

bool SpanContext::sampled() const noexcept
{
  return (trace_flags & 0x01) != 0;
}

std::string SpanContext::trace_id_hex() const
{
  return to_hex(trace_id.data(), trace_id.size());
}

std::string SpanContext::span_id_hex() const
{
  return to_hex(span_id.data(), span_id.size());
}

// ---------------------------------------------------------------------------
// Span handle
// ---------------------------------------------------------------------------

bool Span::valid() const noexcept
{
  return record_ != nullptr;
}

const SpanContext & Span::context() const noexcept
{
  static const SpanContext kInvalid{};
  return record_ != nullptr ? record_->context : kInvalid;
}

void Span::set_attribute(std::string_view key, AttributeValue value) noexcept
{
  if (record_ == nullptr) {
    return;
  }
  try {
    record_->attributes.emplace_back(std::string(key), std::move(value));
  } catch (...) {
    // best-effort
  }
}

void Span::add_event(std::string_view name) noexcept
{
  add_event(name, {});
}

void Span::add_event(
  std::string_view name,
  std::initializer_list<std::pair<std::string_view, AttributeValue>> attrs) noexcept
{
  if (record_ == nullptr) {
    return;
  }
  try {
    EventData event;
    event.time_unix_nano = detail::now_unix_nano();
    event.name = std::string(name);
    for (const auto & attr : attrs) {
      event.attributes.emplace_back(std::string(attr.first), attr.second);
    }
    record_->events.push_back(std::move(event));
  } catch (...) {
    // best-effort
  }
}

void Span::set_status(StatusCode code, std::string_view message) noexcept
{
  if (record_ == nullptr) {
    return;
  }
  try {
    record_->status_code = code;
    record_->status_message = std::string(message);
  } catch (...) {
    // best-effort
  }
}

// ---------------------------------------------------------------------------
// SpanGuard
// ---------------------------------------------------------------------------

SpanGuard::SpanGuard(std::string_view name, SpanOptions opts) noexcept
{
  if (!global::is_active()) {
    return;  // disabled / not initialized => cheap no-op.
  }
  try {
    record_ = open_span_record(name, opts);
    detail::push_span(record_);  // SpanGuard becomes the thread-local current.
  } catch (...) {
    record_ = nullptr;  // never throw out of the public API
  }
}

void SpanGuard::close() noexcept
{
  if (record_ == nullptr) {
    return;
  }
  detail::SpanRecord * record = record_;
  record_ = nullptr;
  detail::pop_frame();
  finalize_and_submit(record);
}

SpanGuard::~SpanGuard()
{
  close();
}

SpanGuard::SpanGuard(SpanGuard && other) noexcept
: record_(other.record_)
{
  other.record_ = nullptr;
}

SpanGuard & SpanGuard::operator=(SpanGuard && other) noexcept
{
  if (this != &other) {
    close();
    record_ = other.record_;
    other.record_ = nullptr;
  }
  return *this;
}

Span SpanGuard::span() const noexcept
{
  return Span(record_);
}

// ---------------------------------------------------------------------------
// DetachedSpan + start_detached_span
// ---------------------------------------------------------------------------

DetachedSpan start_detached_span(std::string_view name, SpanOptions opts) noexcept
{
  if (!global::is_active()) {
    return DetachedSpan();  // disabled / not initialized => inactive handle.
  }
  try {
    detail::SpanRecord * record = open_span_record(name, opts);
    // KEY INVARIANT: a detached span does NOT push the thread-local stack, so
    // the calling thread's current_context()/current_span() are unchanged and it
    // never becomes the parent of an unrelated span opened on this thread later.
    return DetachedSpan(record);
  } catch (...) {
    return DetachedSpan();  // never throw out of the public API.
  }
}

DetachedSpan::~DetachedSpan()
{
  end();
}

DetachedSpan::DetachedSpan(DetachedSpan && other) noexcept
: record_(other.record_)
{
  other.record_ = nullptr;
}

DetachedSpan & DetachedSpan::operator=(DetachedSpan && other) noexcept
{
  if (this != &other) {
    end();
    record_ = other.record_;
    other.record_ = nullptr;
  }
  return *this;
}

bool DetachedSpan::valid() const noexcept
{
  return record_ != nullptr;
}

const SpanContext & DetachedSpan::context() const noexcept
{
  static const SpanContext kInvalid{};
  return record_ != nullptr ? record_->context : kInvalid;
}

Span DetachedSpan::span() const noexcept
{
  return Span(record_);
}

void DetachedSpan::set_attribute(std::string_view key, AttributeValue value) noexcept
{
  Span(record_).set_attribute(key, std::move(value));
}

void DetachedSpan::add_event(std::string_view name) noexcept
{
  Span(record_).add_event(name);
}

void DetachedSpan::add_event(
  std::string_view name,
  std::initializer_list<std::pair<std::string_view, AttributeValue>> attrs) noexcept
{
  Span(record_).add_event(name, attrs);
}

void DetachedSpan::set_status(StatusCode code, std::string_view message) noexcept
{
  Span(record_).set_status(code, message);
}

void DetachedSpan::end() noexcept
{
  if (record_ == nullptr) {
    return;
  }
  detail::SpanRecord * record = record_;
  record_ = nullptr;
  // KEY INVARIANT: no detail::pop_frame() — ending a detached span must not touch
  // the thread-local current-context stack (it was never pushed).
  finalize_and_submit(record);
}

}  // namespace robotops
