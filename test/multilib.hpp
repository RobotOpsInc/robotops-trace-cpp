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

#ifndef MULTILIB_HPP_
#define MULTILIB_HPP_

// Multi-lib nesting fixture (ROB-439).
//
// Each of these two functions lives in its OWN separate shared library
// (libmultilib_a.so / libmultilib_b.so), and each of those libs links the SDK
// core (librobotops_trace_cpp.so) independently. A single test executable loads
// both. lib_a_open() opens a span and keeps it live; lib_b_open() — called while
// A's span is still open — opens a second span. The ONLY way B can nest under A
// is if both libs, plus the core, share ONE copy of the core's thread-local
// active-span stack. They do that precisely because the core is a single SHARED
// .so. With a static core each lib would carry its own stack and B would mint a
// disconnected root — which is exactly the breakage ROB-439 fixes.

#include "robotops_trace/span.hpp"

namespace robotops_multilib
{

/// Open "lib_a_span" inside libmultilib_a.so and keep it open. Returns its
/// SpanContext so the test can compare ids across the lib boundary.
robotops::SpanContext lib_a_open();

/// Close the span libmultilib_a.so opened.
void lib_a_close();

/// Open "lib_b_span" inside libmultilib_b.so (while A's span is live) and keep it
/// open. Returns its SpanContext.
robotops::SpanContext lib_b_open();

/// Close the span libmultilib_b.so opened.
void lib_b_close();

}  // namespace robotops_multilib

#endif  // MULTILIB_HPP_
