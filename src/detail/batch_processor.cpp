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

#include "detail/batch_processor.hpp"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "detail/log.hpp"

namespace robotops
{
namespace detail
{

BatchProcessor::BatchProcessor(
  std::shared_ptr<SpanExporter> exporter,
  Resource resource,
  std::size_t max_queue,
  std::size_t max_batch,
  std::chrono::milliseconds schedule_delay)
: exporter_(std::move(exporter)),
  resource_(std::move(resource)),
  max_queue_(max_queue == 0 ? 1 : max_queue),
  max_batch_(max_batch == 0 ? 1 : max_batch),
  schedule_delay_(
    schedule_delay.count() <= 0 ? std::chrono::milliseconds(1000) : schedule_delay)
{
  worker_ = std::thread([this] {run();});
}

BatchProcessor::~BatchProcessor()
{
  shutdown();
}

void BatchProcessor::enqueue(SpanData span) noexcept
{
  try {
    std::unique_lock<std::mutex> lock(mutex_);
    if (queue_.size() >= max_queue_) {
      // Drop-newest: never block the robot's hot path on a full queue.
      dropped_.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    queue_.push_back(std::move(span));
    lock.unlock();
    work_cv_.notify_one();
  } catch (...) {
    // Even allocation failure must not propagate out of tracing.
    dropped_.fetch_add(1, std::memory_order_relaxed);
  }
}

bool BatchProcessor::force_flush(std::chrono::milliseconds timeout) noexcept
{
  if (shut_down_.load(std::memory_order_acquire)) {
    return true;
  }
  try {
    std::unique_lock<std::mutex> lock(mutex_);
    const std::uint64_t target = ++flush_requested_;
    work_cv_.notify_one();
    if (timeout.count() <= 0) {
      flush_cv_.wait(lock, [&] {return flush_completed_ >= target;});
      return true;
    }
    return flush_cv_.wait_for(
      lock, timeout, [&] {return flush_completed_ >= target;});
  } catch (...) {
    return false;
  }
}

void BatchProcessor::shutdown() noexcept
{
  if (shut_down_.exchange(true)) {
    return;  // already shut down
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_ = true;
  }
  work_cv_.notify_all();
  if (worker_.joinable()) {
    worker_.join();
  }
  if (exporter_) {
    exporter_->shutdown();
  }
}

std::size_t BatchProcessor::dropped_count() const noexcept
{
  return dropped_.load(std::memory_order_relaxed);
}

void BatchProcessor::run() noexcept
{
  std::unique_lock<std::mutex> lock(mutex_);
  for (;; ) {
    work_cv_.wait_for(
      lock, schedule_delay_,
      [&] {return stop_ || !queue_.empty() || flush_requested_ > flush_completed_;});

    const std::uint64_t serving = flush_requested_;
    const bool flush_pending = serving > flush_completed_;

    // Drain everything currently queued, in batches.
    while (!queue_.empty()) {
      std::vector<SpanData> batch;
      const std::size_t take =
        queue_.size() < max_batch_ ? queue_.size() : max_batch_;
      batch.reserve(take);
      for (std::size_t i = 0; i < take && !queue_.empty(); ++i) {
        batch.push_back(std::move(queue_.front()));
        queue_.pop_front();
      }

      lock.unlock();
      bool ok = false;
      try {
        ok = exporter_->export_spans(resource_, batch);
      } catch (...) {
        ok = false;  // an exporter MUST NOT throw, but be defensive.
      }
      if (!ok) {
        log_debug("export_spans failed; batch dropped (best-effort)");
      }
      lock.lock();
    }

    if (flush_pending) {
      lock.unlock();
      try {
        exporter_->force_flush(schedule_delay_);
      } catch (...) {
        // best-effort
      }
      lock.lock();
      if (flush_completed_ < serving) {
        flush_completed_ = serving;
      }
      flush_cv_.notify_all();
    }

    if (stop_ && queue_.empty()) {
      // Honor any flush requests that arrived during the final drain.
      if (flush_requested_ > flush_completed_) {
        flush_completed_ = flush_requested_;
        flush_cv_.notify_all();
      }
      break;
    }
  }
}

}  // namespace detail
}  // namespace robotops
