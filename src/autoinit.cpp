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

// Env-default auto-init shim (ROB-421).
//
// This translation unit is compiled into a SMALL separate shared library,
// librobotops_trace_autoinit.so, that depends ONLY on the SDK core (no ROS, no
// extra third-party deps — the "survives beyond ROS" guarantee holds). Its sole
// job is an ELF constructor that calls robotops::init() the moment the library
// is mapped into a process.
//
// The deployment model mirrors Datadog's -javaagent: set it ONCE in the launch
// environment / systemd unit —
//
//     export LD_PRELOAD=/usr/lib/librobotops_trace_autoinit.so
//
// — and every node in that environment auto-initializes tracing with ZERO
// per-node code. Nodes launched outside the managed environment are unaffected
// and keep calling robotops::init() explicitly (the override path).
//
// Presence in LD_PRELOAD is opt-IN. Two env vars suppress it at runtime without
// touching the launch config:
//   * ROBOTOPS_TRACE_AUTOINIT=0  — disable just the auto-init shim.
//   * ROBOTOPS_TRACE_ENABLED=0   — the global tracing kill switch (also honored
//                                  inside init(), so a redundant guard here just
//                                  avoids spinning up the processor needlessly).

#include <cstdlib>
#include <cstring>

#include "robotops_trace/config.hpp"

namespace
{

// True when `key` is set to an explicit falsey value ("0"/"false"/"off"). An
// unset or empty var is NOT treated as opt-out: presence in LD_PRELOAD is the
// opt-in, and the env can only suppress, never silently invert.
bool env_is_off(const char * key) noexcept
{
  const char * value = std::getenv(key);
  if (value == nullptr || value[0] == '\0') {
    return false;
  }
  return std::strcmp(value, "0") == 0 || std::strcmp(value, "false") == 0 ||
         std::strcmp(value, "off") == 0 || std::strcmp(value, "FALSE") == 0;
}

// Runs when the shared library is loaded (before the host's main() when used via
// LD_PRELOAD). Marked noexcept and guarded so it can NEVER throw or crash the
// host process — robotops::init() is itself noexcept and best-effort.
__attribute__((constructor)) void robotops_trace_autoinit() noexcept
{
  if (env_is_off("ROBOTOPS_TRACE_AUTOINIT")) {
    return;  // opt-out: shim disabled.
  }
  if (env_is_off("ROBOTOPS_TRACE_ENABLED")) {
    return;  // kill switch: tracing disabled fleet-wide.
  }
  // Idempotent + noexcept: if the host also calls init() explicitly, the second
  // call is a logged no-op, so the explicit path stays a safe override.
  robotops::init();
}

}  // namespace
