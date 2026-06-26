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

// Integration lib B for the ROB-439 multi-lib nesting test. Built as its own
// shared object (libmultilib_b.so) that links the SDK core .so, separately from
// lib A. If the core were static, this lib would carry its own copy of the
// thread-local span stack and B would NOT nest under A.

#include <optional>

#include "robotops_trace/trace.hpp"
#include "multilib.hpp"

namespace robotops_multilib
{

namespace
{
std::optional<robotops::SpanGuard> g_b_guard;
}  // namespace

robotops::SpanContext lib_b_open()
{
  g_b_guard.emplace("lib_b_span");
  return g_b_guard->span().context();
}

void lib_b_close()
{
  g_b_guard.reset();
}

}  // namespace robotops_multilib
