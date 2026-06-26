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

// Auto-init preload probe (ROB-421).
//
// A deliberately tiny host executable that NEVER calls robotops::init() itself.
// When librobotops_trace_autoinit.so is LD_PRELOADed, its ELF constructor runs
// init() before main(), so the probe observes an *active* tracer and mints a
// span without any per-process code. With the opt-out env set (or no preload)
// the tracer stays inactive and the span is a cheap no-op.
//
// Prints `active=1` / `active=0` for the CMake/ctest PASS_REGULAR_EXPRESSION
// checks and always exits 0 (the regex, not the exit code, is the assertion).
//
// NOTE: this only reflects the preload's init() across the process boundary when
// the core is a SHARED library, so the shim and the probe share one copy of the
// global tracer state (see BUILD_SHARED_LIBS guard in CMakeLists.txt). With a
// static core each translation unit gets its own state and the preload's init()
// is invisible here — which is exactly the deployed reality: the .deb ships a
// shared librobotops_trace_cpp.so.

#include <cstdio>

#include "global.hpp"  // internal: robotops::global::is_active()
#include "robotops_trace/trace.hpp"

int main()
{
  // Intentionally NO robotops::init() here — the preload shim is the only path
  // that can have initialized the tracer.
  {
    ROBOTOPS_TRACE("autoinit-probe-span");  // captured iff the shim ran; else no-op.
  }

  const bool active = robotops::global::is_active();
  std::printf("active=%d\n", active ? 1 : 0);

  robotops::shutdown();  // idempotent; flushes the minted span if active.
  return 0;
}
