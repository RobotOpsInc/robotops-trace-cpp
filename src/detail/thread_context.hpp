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

#ifndef ROBOTOPS_TRACE__DETAIL__THREAD_CONTEXT_HPP_
#define ROBOTOPS_TRACE__DETAIL__THREAD_CONTEXT_HPP_

#include "robotops_trace/span.hpp"
#include "detail/span_record.hpp"

namespace robotops
{
namespace detail
{

/// The deterministic intra-process propagation core: a thread-local stack of
/// context frames. A frame is either a live span (record != null) or a bare
/// captured context pushed by ScopedContext (record == null). The top frame's
/// context is "current" and is what a newly-opened child span parents under.
struct ContextFrame
{
  SpanRecord * record{nullptr};   ///< null for a ScopedContext frame
  SpanContext context;            ///< the effective context of this frame
};

/// Push a live-span frame. Called by SpanGuard's ctor.
void push_span(SpanRecord * record) noexcept;

/// Push a context-only frame. Called by ScopedContext's ctor.
void push_context(const SpanContext & context) noexcept;

/// Pop the top frame (LIFO; matches RAII nesting).
void pop_frame() noexcept;

/// The live span record at the top of the stack, or null.
SpanRecord * current_record() noexcept;

/// The current context (top frame), or an invalid context when the stack is
/// empty.
SpanContext current_context_value() noexcept;

}  // namespace detail
}  // namespace robotops

#endif  // ROBOTOPS_TRACE__DETAIL__THREAD_CONTEXT_HPP_
