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

#ifndef ROBOTOPS_TRACE__DETAIL__ID_GENERATOR_HPP_
#define ROBOTOPS_TRACE__DETAIL__ID_GENERATOR_HPP_

#include <array>
#include <cstdint>

namespace robotops
{
namespace detail
{

/// Random 16-byte trace id. Never all-zero. Lock-free (thread_local engine).
std::array<std::uint8_t, 16> generate_trace_id() noexcept;

/// Random 8-byte span id. Never all-zero. Lock-free (thread_local engine).
std::array<std::uint8_t, 8> generate_span_id() noexcept;

}  // namespace detail
}  // namespace robotops

#endif  // ROBOTOPS_TRACE__DETAIL__ID_GENERATOR_HPP_
