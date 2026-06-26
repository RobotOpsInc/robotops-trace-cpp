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

#include "robotops_trace/context.hpp"

#include "detail/span_record.hpp"
#include "detail/thread_context.hpp"
#include "global.hpp"

namespace robotops
{

Span current_span() noexcept
{
  return Span(detail::current_record());
}

SpanContext current_context() noexcept
{
  return detail::current_context_value();
}

Context capture_context() noexcept
{
  return Context(detail::current_context_value());
}

ScopedContext::ScopedContext(const Context & context) noexcept
{
  // Only carry a *valid* context, and only when the tracer is active. Otherwise
  // this is a no-op so disabled builds stay cheap.
  if (context.valid() && global::is_active()) {
    detail::push_context(context.span_context());
    active_ = true;
  }
}

ScopedContext::~ScopedContext()
{
  if (active_) {
    detail::pop_frame();
  }
}

}  // namespace robotops
