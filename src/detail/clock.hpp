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

#ifndef ROBOTOPS_TRACE__DETAIL__CLOCK_HPP_
#define ROBOTOPS_TRACE__DETAIL__CLOCK_HPP_

#include <chrono>
#include <cstdint>

namespace robotops
{
namespace detail
{

/// Wall-clock time since the Unix epoch in nanoseconds (for OTLP timestamps).
inline std::uint64_t now_unix_nano() noexcept
{
  const auto since_epoch = std::chrono::system_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(
    std::chrono::duration_cast<std::chrono::nanoseconds>(since_epoch).count());
}

}  // namespace detail
}  // namespace robotops

#endif  // ROBOTOPS_TRACE__DETAIL__CLOCK_HPP_
