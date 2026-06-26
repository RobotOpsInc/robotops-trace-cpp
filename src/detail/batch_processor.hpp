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

#ifndef DETAIL__BATCH_PROCESSOR_HPP_
#define DETAIL__BATCH_PROCESSOR_HPP_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

#include "robotops_trace/exporter.hpp"

namespace robotops
{
namespace detail
{

/// Bounded queue + a single background thread. Span finalization on the caller's
/// thread is a non-blocking enqueue: when the queue is full the newest span is
/// dropped and counted (the robot is NEVER blocked or crashed by tracing). The
/// worker drains in batches of `max_batch`, hands each to the exporter, and also
/// flushes every `schedule_delay`.
class BatchProcessor
{
public:
  BatchProcessor(
    std::shared_ptr<SpanExporter> exporter,
    Resource resource,
    std::size_t max_queue,
    std::size_t max_batch,
    std::chrono::milliseconds schedule_delay);

  ~BatchProcessor();

  BatchProcessor(const BatchProcessor &) = delete;
  BatchProcessor & operator=(const BatchProcessor &) = delete;

  /// Non-blocking enqueue. Drops (and counts) when full. Never throws.
  void enqueue(SpanData span) noexcept;

  /// Block until the queue is drained + exporter flushed, or timeout. Returns
  /// true on full drain.
  bool force_flush(std::chrono::milliseconds timeout) noexcept;

  /// Signal the worker to drain + stop, join it, and shut the exporter down.
  /// Idempotent.
  void shutdown() noexcept;

  /// Spans dropped because the queue was full (diagnostics).
  std::size_t dropped_count() const noexcept;

private:
  void run() noexcept;

  std::shared_ptr<SpanExporter> exporter_;
  Resource resource_;
  const std::size_t max_queue_;
  const std::size_t max_batch_;
  const std::chrono::milliseconds schedule_delay_;

  std::mutex mutex_;
  std::condition_variable work_cv_;
  std::condition_variable flush_cv_;
  std::deque<SpanData> queue_;
  std::uint64_t flush_requested_{0};
  std::uint64_t flush_completed_{0};
  bool stop_{false};
  std::atomic<std::size_t> dropped_{0};
  std::atomic<bool> shut_down_{false};

  std::thread worker_;
};

}  // namespace detail
}  // namespace robotops

#endif  // DETAIL__BATCH_PROCESSOR_HPP_
