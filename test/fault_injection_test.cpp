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

// ROB-440: empirical proof of the Zero-Robot-Impact Invariant (ROB-418).
//
// A tracing failure must NEVER crash, throw into, or BLOCK the host process.
// These tests point the real libcurl OTLP exporter at a black-hole TCP endpoint
// (a localhost socket that accepts the connection but never reads or responds,
// so every POST hangs to its CURL total-timeout) and then prove, with REAL
// wall-clock measurements, that:
//
//   (a) the app's span-mint + enqueue path stays fast and is NOT gated on the
//       stalled export I/O (the worker releases the queue lock before the POST);
//   (b) excess spans are DROPPED (bounded queue), never block the caller;
//   (c) nothing crashes or deadlocks;
//   (d) force_flush(timeout) and shutdown() return in bounded time — no infinite
//       hang against a dead agent;
//   (e) the disabled kill switch (ROBOTOPS_TRACE_ENABLED=0) is a pure no-op even
//       when the configured endpoint is dead, and the public API never throws.
//
// Timing bounds are deliberately generous so the assertions are robust in CI;
// the measured values are PRINTED so the "app stayed fast while export stalled"
// evidence is visible in the test output.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "robotops_trace/trace.hpp"
#include "detail/batch_processor.hpp"
#include "otlp_http_exporter.hpp"
#include "test_harness.hpp"

namespace
{

using std::chrono::duration_cast;
using std::chrono::milliseconds;
using std::chrono::steady_clock;

// A black-hole TCP endpoint: bind+listen on a loopback port but NEVER accept().
// The kernel completes the TCP handshake into the listen backlog, so libcurl's
// connect() succeeds and it sends the POST, but no response ever comes back, so
// the request hangs until the exporter's CURLOPT_TIMEOUT_MS (5s) fires. This is
// the realistic "agent up but wedged / not draining" failure that the invariant
// must absorb without touching the host's hot path.
class BlackHoleEndpoint
{
public:
  BlackHoleEndpoint()
  {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) {
      return;
    }
    int one = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;  // let the kernel pick a free port
    if (::bind(fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
      ::close(fd_);
      fd_ = -1;
      return;
    }
    if (::listen(fd_, 128) != 0) {
      ::close(fd_);
      fd_ = -1;
      return;
    }
    socklen_t len = sizeof(addr);
    if (::getsockname(fd_, reinterpret_cast<sockaddr *>(&addr), &len) != 0) {
      ::close(fd_);
      fd_ = -1;
      return;
    }
    port_ = ntohs(addr.sin_port);
  }

  ~BlackHoleEndpoint()
  {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }

  BlackHoleEndpoint(const BlackHoleEndpoint &) = delete;
  BlackHoleEndpoint & operator=(const BlackHoleEndpoint &) = delete;

  bool ok() const {return fd_ >= 0;}
  std::uint16_t port() const {return port_;}
  std::string endpoint() const {return "http://127.0.0.1:" + std::to_string(port_);}

private:
  int fd_{-1};
  std::uint16_t port_{0};
};

robotops::SpanData make_span(const std::string & name)
{
  robotops::SpanData span;
  span.name = name;
  span.context.trace_id[0] = 0x11;
  span.context.span_id[0] = 0x22;
  span.context.trace_flags = 0x01;
  span.start_unix_nano = 1;
  span.end_unix_nano = 2;
  return span;
}

std::int64_t ms_since(steady_clock::time_point t0)
{
  return duration_cast<milliseconds>(steady_clock::now() - t0).count();
}

// Thread body: hammer the processor's enqueue path `count` times. Named (rather
// than an inline lambda) so the thread workers stay simple to read and format.
void flood_enqueue(robotops::detail::BatchProcessor * processor, int count)
{
  for (int i = 0; i < count; ++i) {
    processor->enqueue(make_span("flood"));
  }
}

// Thread body: mint `count` RAII spans through the public SpanGuard API.
void flood_span_guards(int count)
{
  for (int i = 0; i < count; ++i) {
    robotops::SpanGuard guard("flood");  // RAII: submits on scope exit
  }
}

}  // namespace

// 1. The exporter's POST to a wedged agent is TIME-BOUNDED by the CURL timeouts,
//    not infinite. A single export on the calling thread returns failure within
//    the total timeout (+margin), proving the bounded connect/total timeouts.
TEST_CASE(fault_export_to_black_hole_is_time_bounded)
{
  BlackHoleEndpoint bh;
  CHECK(bh.ok());
  if (!bh.ok()) {
    return;
  }

  robotops::OtlpHttpExporter exporter(bh.endpoint());
  robotops::Resource resource;
  std::vector<robotops::SpanData> batch;
  batch.push_back(make_span("probe"));

  const auto t0 = steady_clock::now();
  const bool ok = exporter.export_spans(resource, batch);
  const std::int64_t elapsed = ms_since(t0);
  std::printf(
    "[fault] single export to black-hole: ok=%d, elapsed=%" PRId64 " ms "
    "(bounded by CURLOPT_TIMEOUT_MS=5000)\n", ok ? 1 : 0, elapsed);

  CHECK(!ok);                  // dead endpoint => best-effort failure, not a hang
  const bool bounded = elapsed < 9000;   // bounded by the 5s total timeout + margin
  CHECK(bounded);
  exporter.shutdown();
}

// 2. THE HEADLINE TEST. While the background worker is wedged inside a 5s POST to
//    the black-hole agent, several app threads mint a flood of spans. Assert the
//    enqueue path stays fast (NOT gated on the export I/O), excess spans are
//    dropped (bounded queue), and force_flush/shutdown stay bounded.
TEST_CASE(fault_enqueue_stays_fast_while_export_stalls)
{
  BlackHoleEndpoint bh;
  CHECK(bh.ok());
  if (!bh.ok()) {
    return;
  }

  auto exporter = std::make_shared<robotops::OtlpHttpExporter>(bh.endpoint());
  robotops::Resource resource;

  constexpr std::size_t kMaxQueue = 128;
  // max_batch >= max_queue so the residual drain on shutdown is a single bounded
  // export rather than a loop of them.
  constexpr std::size_t kMaxBatch = 512;
  robotops::detail::BatchProcessor processor(
    exporter, resource, kMaxQueue, kMaxBatch, milliseconds(60000));

  // Prime: enqueue one span and give the worker a moment to pick it up and BLOCK
  // inside the libcurl POST. The entire measured window below overlaps this 5s
  // stall, so we are genuinely measuring enqueue latency *while export is hung*.
  processor.enqueue(make_span("prime"));
  std::this_thread::sleep_for(milliseconds(250));

  constexpr int kThreads = 4;
  constexpr int kPerThread = 50000;
  std::vector<std::thread> threads;
  threads.reserve(kThreads);

  const auto t0 = steady_clock::now();
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back(flood_enqueue, &processor, kPerThread);
  }
  for (auto & th : threads) {
    th.join();
  }
  const std::int64_t mint_ms = ms_since(t0);

  const std::size_t total = static_cast<std::size_t>(kThreads) * kPerThread;
  const std::size_t dropped = processor.dropped_count();
  const double per_enqueue_us =
    mint_ms > 0 ? (static_cast<double>(mint_ms) * 1000.0 / static_cast<double>(total)) : 0.0;
  std::printf(
    "[fault] %zu spans minted across %d threads in %" PRId64 " ms (~%.3f us/enqueue) "
    "WHILE the export thread was wedged in a 5s POST; dropped=%zu\n",
    total, kThreads, mint_ms, per_enqueue_us, dropped);

  // (a) App threads were NOT blocked on the 5s export: a flood of enqueues
  //     finished in a tiny fraction of a single export timeout.
  const bool mint_was_fast = mint_ms < 3000;
  CHECK(mint_was_fast);
  // (b) Excess spans were dropped (bounded queue, drop-when-full), not blocked.
  const bool some_dropped = dropped > 0;
  const bool nearly_all_dropped = dropped > total - kMaxQueue - 1000;
  CHECK(some_dropped);
  CHECK(nearly_all_dropped);

  // (d) force_flush against the wedged agent returns within ~its timeout.
  const auto f0 = steady_clock::now();
  const bool flushed = processor.force_flush(milliseconds(300));
  const std::int64_t flush_ms = ms_since(f0);
  std::printf(
    "[fault] force_flush(300ms) against wedged agent: returned=%d in %" PRId64 " ms\n",
    flushed ? 1 : 0, flush_ms);
  CHECK(!flushed);             // export is hung => cannot fully drain in 300ms
  const bool flush_bounded = flush_ms < 2000;   // ...but it RETURNED, bounded
  CHECK(flush_bounded);

  // (d) shutdown joins the worker in bounded time: it finishes the in-flight POST
  //     and drains the residual queue in one more bounded export, then stops.
  const auto s0 = steady_clock::now();
  processor.shutdown();
  const std::int64_t shutdown_ms = ms_since(s0);
  std::printf(
    "[fault] shutdown() against wedged agent returned in %" PRId64 " ms "
    "(bounded by in-flight + one residual export)\n", shutdown_ms);
  const bool shutdown_bounded = shutdown_ms < 15000;   // bounded, no infinite hang
  CHECK(shutdown_bounded);
}

// 3. The same invariant via the PUBLIC API: init() pointed at a dead agent, spans
//    minted across threads with SpanGuard, then force_flush + shutdown — all
//    bounded, nothing throws, nothing blocks on the export I/O.
TEST_CASE(fault_public_api_non_blocking_under_dead_agent)
{
  ::unsetenv("ROBOTOPS_TRACE_ENABLED");
  ::unsetenv("ROBOTOPS_OTLP_ENDPOINT");
  robotops::shutdown();  // start from a clean global state

  BlackHoleEndpoint bh;
  CHECK(bh.ok());
  if (!bh.ok()) {
    return;
  }

  robotops::Config config;
  config.service_name = "fault_injection";
  config.endpoint = bh.endpoint();    // default OTLP exporter -> dead agent
  config.max_queue = 128;
  config.max_batch = 512;
  config.schedule_delay = milliseconds(60000);
  robotops::init(config);

  // Prime + let the worker wedge inside the POST.
  {robotops::SpanGuard prime("prime");}
  std::this_thread::sleep_for(milliseconds(250));

  constexpr int kThreads = 4;
  constexpr int kPerThread = 25000;
  std::vector<std::thread> threads;
  threads.reserve(kThreads);

  const auto t0 = steady_clock::now();
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back(flood_span_guards, kPerThread);
  }
  for (auto & th : threads) {
    th.join();
  }
  const std::int64_t mint_ms = ms_since(t0);
  const std::size_t total = static_cast<std::size_t>(kThreads) * kPerThread;
  std::printf(
    "[fault] PUBLIC API: %zu SpanGuards across %d threads in %" PRId64 " ms "
    "while the OTLP export thread was wedged on a dead agent\n",
    total, kThreads, mint_ms);
  const bool mint_was_fast = mint_ms < 4000;   // not gated on the 5s export
  CHECK(mint_was_fast);

  const auto f0 = steady_clock::now();
  const bool flushed = robotops::force_flush(milliseconds(300));
  const std::int64_t flush_ms = ms_since(f0);
  std::printf(
    "[fault] PUBLIC API force_flush(300ms): returned=%d in %" PRId64 " ms\n",
    flushed ? 1 : 0, flush_ms);
  const bool flush_bounded = flush_ms < 2000;
  CHECK(flush_bounded);

  const auto s0 = steady_clock::now();
  robotops::shutdown();
  const std::int64_t shutdown_ms = ms_since(s0);
  std::printf(
    "[fault] PUBLIC API shutdown() returned in %" PRId64 " ms\n", shutdown_ms);
  const bool shutdown_bounded = shutdown_ms < 15000;
  CHECK(shutdown_bounded);
}

// 4. Disabled kill switch is a pure, fast no-op even with a dead endpoint
//    configured, and the public API never throws.
TEST_CASE(fault_disabled_is_pure_no_op_even_with_dead_endpoint)
{
  ::unsetenv("ROBOTOPS_OTLP_ENDPOINT");
  robotops::shutdown();
  ::setenv("ROBOTOPS_TRACE_ENABLED", "0", 1);

  robotops::Config config;
  config.endpoint = "http://127.0.0.1:9";  // discard port: nothing listens
  robotops::init(config);                  // kill switch => stays inactive

  constexpr int kSpans = 200000;
  const auto t0 = steady_clock::now();
  for (int i = 0; i < kSpans; ++i) {
    robotops::SpanGuard guard("noop");
    CHECK(!guard.span().valid());
  }
  const std::int64_t elapsed = ms_since(t0);
  std::printf(
    "[fault] disabled kill switch: %d no-op SpanGuards in %" PRId64 " ms\n",
    kSpans, elapsed);

  CHECK(!robotops::current_span().valid());
  CHECK(!robotops::current_context().valid());
  CHECK(robotops::force_flush(milliseconds(0)));   // nothing to flush => instant true
  robotops::shutdown();
  const bool noop_was_fast = elapsed < 2000;   // a disabled tracer never touches the net
  CHECK(noop_was_fast);

  ::unsetenv("ROBOTOPS_TRACE_ENABLED");
}
