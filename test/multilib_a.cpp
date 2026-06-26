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

// Integration lib A for the ROB-439 multi-lib nesting test. Built as its own
// shared object (libmultilib_a.so) that links the SDK core .so.

#include <optional>

#include "robotops_trace/trace.hpp"
#include "multilib.hpp"

namespace robotops_multilib
{

namespace
{
// Holds A's span open across the call boundary (SpanGuard is move-only, so wrap
// it in optional rather than returning it).
std::optional<robotops::SpanGuard> g_a_guard;
}  // namespace

robotops::SpanContext lib_a_open()
{
  g_a_guard.emplace("lib_a_span");
  return g_a_guard->span().context();
}

void lib_a_close()
{
  g_a_guard.reset();
}

}  // namespace robotops_multilib
