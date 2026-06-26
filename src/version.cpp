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

#include "robotops_trace/trace.hpp"

namespace robotops_trace
{

const char * version() noexcept
{
  // Keep in sync with package.xml (source of truth, bumped via the justfile).
  return "0.1.0";
}

}  // namespace robotops_trace

namespace RobotOps
{

// SCAFFOLD placeholders (ROB-433). Real bodies land in ROB-419.

void init() noexcept
{
  // TODO(ROB-419): start the OTLP exporter + background flush thread.
}

void shutdown() noexcept
{
  // TODO(ROB-419): flush pending spans and join the background thread.
}

SpanGuard::SpanGuard(std::string_view /*name*/) noexcept
: active_(true)
{
  // TODO(ROB-419): push a span onto the thread-local context stack.
}

SpanGuard::~SpanGuard()
{
  // TODO(ROB-419): close the span and restore the previous context.
}

SpanGuard::SpanGuard(SpanGuard && other) noexcept
: active_(other.active_)
{
  other.active_ = false;
}

SpanGuard & SpanGuard::operator=(SpanGuard && other) noexcept
{
  if (this != &other) {
    active_ = other.active_;
    other.active_ = false;
  }
  return *this;
}

}  // namespace RobotOps
