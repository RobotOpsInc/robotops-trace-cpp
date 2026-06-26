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

#ifndef ROBOTOPS_TRACE__CONTEXT_HPP_
#define ROBOTOPS_TRACE__CONTEXT_HPP_

#include "robotops_trace/span.hpp"

/// \file context.hpp
/// \brief Thread-local current context + async capture/attach.
///
/// Intra-process parent/child propagation is deterministic and lives entirely
/// in a thread-local stack — no wire format is involved. `Context` +
/// `ScopedContext` carry that context across thread boundaries (capture on
/// submit, restore on run) for executors, thread pools, callbacks, etc.

namespace robotops
{

/// Handle to the live span on the calling thread (invalid if none / disabled).
Span current_span() noexcept;

/// Snapshot of the calling thread's active SpanContext (invalid if none).
SpanContext current_context() noexcept;

/// Copyable snapshot of a SpanContext, used to carry context across threads.
class Context
{
public:
  Context() noexcept = default;
  explicit Context(const SpanContext & ctx) noexcept
  : ctx_(ctx) {}

  bool valid() const noexcept {return ctx_.valid();}
  const SpanContext & span_context() const noexcept {return ctx_;}

private:
  SpanContext ctx_{};
};

/// Snapshot the thread-local active context (capture-on-submit half).
Context capture_context() noexcept;

/// RAII: install a captured context as the current context on THIS thread for
/// the lifetime of the object (restore-on-run half). Spans opened while it is
/// active nest under the captured context.
class ScopedContext
{
public:
  explicit ScopedContext(const Context & context) noexcept;
  ~ScopedContext();

  ScopedContext(const ScopedContext &) = delete;
  ScopedContext & operator=(const ScopedContext &) = delete;

private:
  bool active_{false};
};

}  // namespace robotops

#endif  // ROBOTOPS_TRACE__CONTEXT_HPP_
