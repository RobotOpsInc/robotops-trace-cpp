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

// Smoke check for the standalone (plain-CMake, no-ROS) build path. Exercises
// the public API surface so the `cmake-standalone` CI guardrail catches any
// accidental ROS coupling at link time, not just compile time.

#include <cstring>
#include <iostream>

#include "robotops_trace/trace.hpp"

int main()
{
  RobotOps::init();
  {
    ROBOTOPS_TRACE("standalone-smoke");
  }
  RobotOps::shutdown();

  const char * v = robotops_trace::version();
  std::cout << "robotops_trace_cpp version: " << v << '\n';

  return std::strcmp(v, "0.1.0") == 0 ? 0 : 1;
}
