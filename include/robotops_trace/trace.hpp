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

#ifndef ROBOTOPS_TRACE__TRACE_HPP_
#define ROBOTOPS_TRACE__TRACE_HPP_

/// \file trace.hpp
/// \brief Umbrella include for the RobotOps C++ tracing SDK core.
///
/// Pull in just this header for the common case. Everything lives in the single
/// lowercase `robotops` namespace (matching the Python import name + modern C++
/// convention). The SDK core is transport-agnostic and MUST remain buildable
/// without ROS (see ROBOTOPS_TRACE_STANDALONE in CMakeLists.txt) — the "survives
/// beyond ROS" guardrail. The only third-party runtime dep is libcurl, confined
/// to the exporter implementation.

#include "robotops_trace/config.hpp"     // Config, init/shutdown/force_flush
#include "robotops_trace/context.hpp"    // current_span/context, capture/attach
#include "robotops_trace/exporter.hpp"   // SpanData, SpanExporter, InMemory...
#include "robotops_trace/span.hpp"       // SpanContext, Span, SpanGuard, version
#include "robotops_trace/w3c.hpp"        // traceparent inject/extract

/// Concatenation helpers so ROBOTOPS_TRACE() can mint a unique guard variable.
#define ROBOTOPS_TRACE_CONCAT_(a, b) a ## b
#define ROBOTOPS_TRACE_CONCAT(a, b) ROBOTOPS_TRACE_CONCAT_(a, b)

/// \brief Open an RAII span for the enclosing scope.
///
/// Usage:
/// \code
///   void plan() {
///     ROBOTOPS_TRACE("plan");
///     // ... span closes automatically at end of scope ...
///   }
/// \endcode
#define ROBOTOPS_TRACE(name) \
  ::robotops::SpanGuard ROBOTOPS_TRACE_CONCAT(robotops_span_, __LINE__) {name}

#endif  // ROBOTOPS_TRACE__TRACE_HPP_
