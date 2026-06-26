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

#include <string>
#include <string_view>

/// \file trace.hpp
/// \brief Public API surface for the RobotOps C++ tracing SDK core.
///
/// SCAFFOLD ONLY (ROB-433). The real implementation lands in ROB-419. This
/// header defines the *shape* of the API described in the build-out spec so
/// downstream integrations (robotops-trace-integrations) can compile against a
/// stable surface, but the bodies are deliberately trivial placeholders.
///
/// The SDK core is transport-agnostic and MUST remain buildable without ROS
/// (see ROBOTOPS_TRACE_STANDALONE in CMakeLists.txt) — the "survives beyond
/// ROS" guardrail.

namespace robotops_trace
{

/// Returns the SDK core version string (matches package.xml).
const char * version() noexcept;

}  // namespace robotops_trace

namespace RobotOps
{

/// Initialize the tracing SDK.
///
/// Idempotent. In the env-default auto-init model this is invoked from the
/// LD_PRELOAD constructor library (ROB-421); nodes launched outside that env
/// call it explicitly (the override path).
///
/// TODO(ROB-419): wire up the OTLP exporter, read ROBOTOPS_OTLP_ENDPOINT, set
/// up the background flush thread, and honour the runtime kill switch.
void init() noexcept;

/// Shut the SDK down and flush any pending spans. Idempotent.
///
/// TODO(ROB-419): drain the exporter queue and join the background thread.
void shutdown() noexcept;

/// RAII span guard. Opens a span on construction and closes it on destruction,
/// restoring the previous thread-local context. Move-only.
///
/// TODO(ROB-419): back this with the real span/context machinery (thread-local
/// stack, async capture/restore, OTLP span emission). For the scaffold it is a
/// no-op shell so the macro below and downstream code compile and link.
class SpanGuard
{
public:
  explicit SpanGuard(std::string_view name) noexcept;
  ~SpanGuard();

  SpanGuard(const SpanGuard &) = delete;
  SpanGuard & operator=(const SpanGuard &) = delete;
  SpanGuard(SpanGuard &&) noexcept;
  SpanGuard & operator=(SpanGuard &&) noexcept;

private:
  bool active_;
};

}  // namespace RobotOps

/// Concatenation helpers so ROBOTOPS_TRACE() can mint a unique guard variable.
#define ROBOTOPS_TRACE_CONCAT_(a, b) a##b
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
  ::RobotOps::SpanGuard ROBOTOPS_TRACE_CONCAT(robotops_span_, __LINE__) {name}

#endif  // ROBOTOPS_TRACE__TRACE_HPP_
