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

#include "detail/id_generator.hpp"

#include <random>

namespace robotops
{
namespace detail
{

namespace
{

// A per-thread, lock-free PRNG. The ids are correlation tokens, not secrets, so
// a well-distributed mt19937_64 seeded from random_device is plenty.
std::mt19937_64 & engine() noexcept
{
  static thread_local std::mt19937_64 eng{std::random_device{}()};
  return eng;
}

std::uint64_t next_nonzero_word() noexcept
{
  auto & eng = engine();
  std::uint64_t value = eng();
  while (value == 0) {
    value = eng();
  }
  return value;
}

}  // namespace

std::array<std::uint8_t, 16> generate_trace_id() noexcept
{
  // Two non-zero 64-bit words guarantees the 16-byte id is never all-zero.
  const std::uint64_t hi = next_nonzero_word();
  const std::uint64_t lo = next_nonzero_word();
  std::array<std::uint8_t, 16> out{};
  for (int i = 0; i < 8; ++i) {
    out[static_cast<std::size_t>(i)] =
      static_cast<std::uint8_t>((hi >> (8 * (7 - i))) & 0xFF);
    out[static_cast<std::size_t>(8 + i)] =
      static_cast<std::uint8_t>((lo >> (8 * (7 - i))) & 0xFF);
  }
  return out;
}

std::array<std::uint8_t, 8> generate_span_id() noexcept
{
  const std::uint64_t word = next_nonzero_word();
  std::array<std::uint8_t, 8> out{};
  for (int i = 0; i < 8; ++i) {
    out[static_cast<std::size_t>(i)] =
      static_cast<std::uint8_t>((word >> (8 * (7 - i))) & 0xFF);
  }
  return out;
}

}  // namespace detail
}  // namespace robotops
