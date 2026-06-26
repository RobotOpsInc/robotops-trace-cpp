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

#ifndef ROBOTOPS_TRACE__W3C_HPP_
#define ROBOTOPS_TRACE__W3C_HPP_

#include <string>
#include <string_view>

#include "robotops_trace/span.hpp"

/// \file w3c.hpp
/// \brief W3C Trace Context `traceparent` inject/extract. This is the *cross
/// process* carrier (the intra-process path is thread-local, not the wire).

namespace robotops
{

/// Serialize a context to a W3C `traceparent`:
/// "00-<32 hex trace id>-<16 hex span id>-<2 hex flags>".
/// Returns empty for an invalid context.
std::string inject(const SpanContext & context);

/// Parse a W3C `traceparent`. Validates version 00, field lengths and hex.
/// On success returns a context with remote=true; on failure an invalid one.
SpanContext extract(std::string_view traceparent);

}  // namespace robotops

#endif  // ROBOTOPS_TRACE__W3C_HPP_
