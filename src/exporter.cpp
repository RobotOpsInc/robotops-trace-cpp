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

#include "robotops_trace/exporter.hpp"

#include <mutex>
#include <vector>

namespace robotops
{

bool InMemorySpanExporter::export_spans(
  const Resource & resource,
  const std::vector<SpanData> & spans) noexcept
{
  try {
    std::lock_guard<std::mutex> lock(mutex_);
    resource_ = resource;
    spans_.insert(spans_.end(), spans.begin(), spans.end());
    return true;
  } catch (...) {
    return false;
  }
}

std::vector<SpanData> InMemorySpanExporter::spans() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return spans_;
}

std::size_t InMemorySpanExporter::size() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return spans_.size();
}

void InMemorySpanExporter::clear()
{
  std::lock_guard<std::mutex> lock(mutex_);
  spans_.clear();
}

}  // namespace robotops
