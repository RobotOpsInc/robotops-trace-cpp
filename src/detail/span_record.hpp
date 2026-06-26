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

#ifndef DETAIL__SPAN_RECORD_HPP_
#define DETAIL__SPAN_RECORD_HPP_

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "robotops_trace/exporter.hpp"
#include "robotops_trace/span.hpp"

namespace robotops
{
namespace detail
{

/// The mutable, thread-confined record for an *open* span. Owned by its
/// SpanGuard; a raw pointer lives on the thread-local stack while it is the
/// current span. Finalized into a SpanData on close. Because a record is only
/// ever touched on the thread that opened it, no locking is required.
struct SpanRecord
{
  SpanContext context;
  std::array<std::uint8_t, 8> parent_span_id{};
  std::string name;
  SpanKind kind{SpanKind::Internal};
  std::uint64_t start_unix_nano{0};
  StatusCode status_code{StatusCode::Unset};
  std::string status_message;
  std::vector<std::pair<std::string, AttributeValue>> attributes;
  std::vector<EventData> events;
};

}  // namespace detail
}  // namespace robotops

#endif  // DETAIL__SPAN_RECORD_HPP_
