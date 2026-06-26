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

#ifndef ROBOTOPS_TRACE__GLOBAL_HPP_
#define ROBOTOPS_TRACE__GLOBAL_HPP_

#include "robotops_trace/exporter.hpp"

namespace robotops
{
namespace global
{

/// True when the tracer is initialized AND enabled. The SpanGuard hot path
/// checks this first; when false every span operation is a cheap no-op.
bool is_active() noexcept;

/// Hand a finalized span to the background processor. No-op (span dropped) when
/// inactive. Never throws.
void submit(SpanData span) noexcept;

}  // namespace global
}  // namespace robotops

#endif  // ROBOTOPS_TRACE__GLOBAL_HPP_
