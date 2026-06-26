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

#ifndef DETAIL__LOG_HPP_
#define DETAIL__LOG_HPP_

#include <string_view>

namespace robotops
{
namespace detail
{

/// Internal best-effort logging. The SDK never throws and never spams a robot's
/// logs: messages go to stderr only when ROBOTOPS_TRACE_DEBUG is set in the
/// environment. Used to surface dropped batches / failed exports for debugging.
void log_debug(std::string_view message) noexcept;

/// Always-on warning (init misuse, etc.). Single line to stderr.
void log_warn(std::string_view message) noexcept;

}  // namespace detail
}  // namespace robotops

#endif  // DETAIL__LOG_HPP_
