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

// ROB-441: REAL round-trip proof of the OTLP/HTTP exporter transports.
//
// The default endpoint is a Unix-domain socket (unix:///run/robotops/trace.sock);
// http://host:port is the TCP-loopback fallback. The exporter sets libcurl's
// CURLOPT_UNIX_SOCKET_PATH for the UDS scheme so an otherwise-normal HTTP POST
// (path /v1/traces, Content-Type: application/x-protobuf) rides the socket.
//
// These tests stand up throwaway in-process HTTP servers — one bound to a temp
// AF_UNIX .sock, one to a loopback AF_INET port — that accept POST /v1/traces and
// return 200, and prove that the SAME exporter:
//   (1) delivers the protobuf body over a real UDS (server receives the exact
//       serialized bytes, correct request line + content-type);
//   (2) delivers the protobuf body over real TCP loopback (the fallback);
//   (3) on a socket-absent UDS endpoint, drops the batch (best-effort failure)
//       within the bounded curl timeouts — no hang, no crash, app unaffected.
// It also reports the measured per-batch cost of UDS vs TCP.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "robotops_trace/exporter.hpp"
#include "otlp_http_exporter.hpp"
#include "test_harness.hpp"

namespace
{

using std::chrono::duration_cast;
using std::chrono::microseconds;
using std::chrono::milliseconds;
using std::chrono::steady_clock;

// A minimal in-process HTTP/1.1 capture server. It loops accept() on a listen
// socket (either AF_UNIX or AF_INET loopback) in a background thread, reads each
// request fully (headers + Content-Length body, handling Expect: 100-continue so
// the request is "normal" HTTP), records the last request, and replies 200. One
// request per connection is sufficient — the exporter resets its CURL handle per
// batch, so each export is its own connection.
class HttpCaptureServer
{
public:
  // Create an AF_UNIX server bound to `path` (any stale file is removed first).
  static HttpCaptureServer * make_uds(const std::string & path)
  {
    auto * s = new HttpCaptureServer();
    s->uds_path_ = path;
    ::unlink(path.c_str());
    s->listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (s->listen_fd_ < 0) {
      return s;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    if (::bind(s->listen_fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
      ::close(s->listen_fd_);
      s->listen_fd_ = -1;
      return s;
    }
    if (::listen(s->listen_fd_, 16) != 0) {
      ::close(s->listen_fd_);
      s->listen_fd_ = -1;
      return s;
    }
    s->endpoint_ = "unix://" + path;
    return s;
  }

  // Create an AF_INET loopback server on a kernel-assigned free port.
  static HttpCaptureServer * make_tcp()
  {
    auto * s = new HttpCaptureServer();
    s->listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s->listen_fd_ < 0) {
      return s;
    }
    int one = 1;
    ::setsockopt(s->listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(s->listen_fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 ||
      ::listen(s->listen_fd_, 16) != 0)
    {
      ::close(s->listen_fd_);
      s->listen_fd_ = -1;
      return s;
    }
    socklen_t len = sizeof(addr);
    ::getsockname(s->listen_fd_, reinterpret_cast<sockaddr *>(&addr), &len);
    s->endpoint_ = "http://127.0.0.1:" + std::to_string(ntohs(addr.sin_port));
    return s;
  }

  ~HttpCaptureServer()
  {
    stop();
    if (!uds_path_.empty()) {
      ::unlink(uds_path_.c_str());
    }
  }

  HttpCaptureServer(const HttpCaptureServer &) = delete;
  HttpCaptureServer & operator=(const HttpCaptureServer &) = delete;

  bool ok() const {return listen_fd_ >= 0;}
  const std::string & endpoint() const {return endpoint_;}

  void start()
  {
    running_.store(true);
    thread_ = std::thread([this] {accept_loop();});
  }

  void stop()
  {
    running_.store(false);
    if (thread_.joinable()) {
      thread_.join();
    }
    if (listen_fd_ >= 0) {
      ::close(listen_fd_);
      listen_fd_ = -1;
    }
  }

  int request_count() const {return request_count_.load();}
  std::string last_body() const {return last_body_;}
  std::string last_content_type() const {return last_content_type_;}
  std::string last_request_line() const {return last_request_line_;}

private:
  HttpCaptureServer() = default;

  void accept_loop()
  {
    while (running_.load()) {
      pollfd pfd{};
      pfd.fd = listen_fd_;
      pfd.events = POLLIN;
      const int pr = ::poll(&pfd, 1, 100);   // 100 ms tick so we notice stop()
      if (pr <= 0) {
        continue;
      }
      const int conn = ::accept(listen_fd_, nullptr, nullptr);
      if (conn < 0) {
        continue;
      }
      handle_connection(conn);
      ::close(conn);
    }
  }

  // Read one full HTTP request and reply 200. Tolerant, blocking reads guarded by
  // a recv timeout so a misbehaving client can never wedge the test.
  void handle_connection(int conn)
  {
    timeval tv{};
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    ::setsockopt(conn, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    std::string buf;
    char tmp[4096];

    // 1) Read until the end of the header block.
    std::size_t header_end = std::string::npos;
    while (header_end == std::string::npos) {
      const ssize_t n = ::recv(conn, tmp, sizeof(tmp), 0);
      if (n <= 0) {
        return;
      }
      buf.append(tmp, static_cast<std::size_t>(n));
      header_end = buf.find("\r\n\r\n");
    }

    const std::string headers = buf.substr(0, header_end);
    last_request_line_ = headers.substr(0, headers.find("\r\n"));
    last_content_type_ = header_value(headers, "content-type");

    // Honor Expect: 100-continue so the POST is a normal HTTP exchange.
    if (!header_value(headers, "expect").empty()) {
      const char * cont = "HTTP/1.1 100 Continue\r\n\r\n";
      (void)::send(conn, cont, std::strlen(cont), 0);
    }

    const std::size_t content_length = static_cast<std::size_t>(
      std::strtoul(header_value(headers, "content-length").c_str(), nullptr, 10));

    // 2) Read the body by length (it is binary protobuf — may contain NULs).
    std::string body = buf.substr(header_end + 4);
    while (body.size() < content_length) {
      const ssize_t n = ::recv(conn, tmp, sizeof(tmp), 0);
      if (n <= 0) {
        break;
      }
      body.append(tmp, static_cast<std::size_t>(n));
    }

    last_body_ = body;
    request_count_.fetch_add(1);

    const char * resp = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
    (void)::send(conn, resp, std::strlen(resp), 0);
  }

  // Case-insensitive lookup of a single header value in the header block.
  static std::string header_value(const std::string & headers, const std::string & name)
  {
    std::string lower = headers;
    for (char & c : lower) {
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    const std::string key = "\r\n" + name + ":";
    std::size_t pos = lower.find(key);
    if (pos == std::string::npos) {
      // The very first header line is not preceded by CRLF.
      if (lower.compare(0, name.size() + 1, name + ":") == 0) {
        pos = 0;
      } else {
        return std::string();
      }
    } else {
      pos += 2;   // skip the leading CRLF
    }
    const std::size_t colon = headers.find(':', pos);
    std::size_t end = headers.find("\r\n", colon);
    if (end == std::string::npos) {
      end = headers.size();
    }
    std::string value = headers.substr(colon + 1, end - colon - 1);
    const std::size_t first = value.find_first_not_of(" \t");
    const std::size_t last = value.find_last_not_of(" \t");
    if (first == std::string::npos) {
      return std::string();
    }
    return value.substr(first, last - first + 1);
  }

  int listen_fd_{-1};
  std::string endpoint_;
  std::string uds_path_;
  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<int> request_count_{0};
  std::string last_body_;
  std::string last_content_type_;
  std::string last_request_line_;
};

robotops::SpanData make_span(const std::string & name)
{
  robotops::SpanData span;
  span.name = name;
  span.context.trace_id[0] = 0x11;
  span.context.span_id[0] = 0x22;
  span.context.trace_flags = 0x01;
  span.start_unix_nano = 1700000000000000000ULL;
  span.end_unix_nano = 1700000000500000000ULL;
  return span;
}

std::string temp_sock_path(const std::string & tag)
{
  return "/tmp/robotops_uds_" + tag + "_" + std::to_string(::getpid()) + ".sock";
}

std::int64_t ms_since(steady_clock::time_point t0)
{
  return duration_cast<milliseconds>(steady_clock::now() - t0).count();
}

}  // namespace

// 1. HEADLINE PROOF: a span batch is delivered over a real Unix-domain socket.
//    The exporter is pointed at unix:///tmp/<test>.sock; the server receives the
//    EXACT serialized protobuf body, with request line "POST /v1/traces HTTP/1.1"
//    and Content-Type: application/x-protobuf.
TEST_CASE(transport_uds_round_trip_delivers_protobuf)
{
  const std::string path = temp_sock_path("rt");
  HttpCaptureServer * server = HttpCaptureServer::make_uds(path);
  CHECK(server->ok());
  if (!server->ok()) {
    delete server;
    return;
  }
  server->start();

  robotops::Resource resource;
  resource.attributes.emplace_back("service.name", "rob441-uds");
  std::vector<robotops::SpanData> batch;
  batch.push_back(make_span("uds-span"));
  const std::string expected = robotops::OtlpHttpExporter::serialize(resource, batch);

  robotops::OtlpHttpExporter exporter("unix://" + path);
  const bool ok = exporter.export_spans(resource, batch);
  exporter.shutdown();
  server->stop();

  std::printf(
    "[transport] UDS export: ok=%d, server_requests=%d, body=%zu bytes, "
    "request_line=\"%s\", content_type=\"%s\"\n",
    ok ? 1 : 0, server->request_count(), server->last_body().size(),
    server->last_request_line().c_str(), server->last_content_type().c_str());

  CHECK(ok);                                       // 2xx over the UDS
  CHECK_EQ(server->request_count(), 1);            // exactly one batch delivered
  CHECK_EQ(server->last_body(), expected);         // EXACT protobuf bytes received
  CHECK_EQ(server->last_request_line(), std::string("POST /v1/traces HTTP/1.1"));
  CHECK_EQ(server->last_content_type(), std::string("application/x-protobuf"));
  delete server;
}

// 2. TCP fallback still works: same exporter, http://127.0.0.1:<port>.
TEST_CASE(transport_tcp_fallback_delivers_protobuf)
{
  HttpCaptureServer * server = HttpCaptureServer::make_tcp();
  CHECK(server->ok());
  if (!server->ok()) {
    delete server;
    return;
  }
  server->start();

  robotops::Resource resource;
  resource.attributes.emplace_back("service.name", "rob441-tcp");
  std::vector<robotops::SpanData> batch;
  batch.push_back(make_span("tcp-span"));
  const std::string expected = robotops::OtlpHttpExporter::serialize(resource, batch);

  robotops::OtlpHttpExporter exporter(server->endpoint());
  const bool ok = exporter.export_spans(resource, batch);
  exporter.shutdown();
  server->stop();

  std::printf(
    "[transport] TCP export: ok=%d, server_requests=%d, body=%zu bytes, "
    "request_line=\"%s\", content_type=\"%s\"\n",
    ok ? 1 : 0, server->request_count(), server->last_body().size(),
    server->last_request_line().c_str(), server->last_content_type().c_str());

  CHECK(ok);
  CHECK_EQ(server->request_count(), 1);
  CHECK_EQ(server->last_body(), expected);
  CHECK_EQ(server->last_request_line(), std::string("POST /v1/traces HTTP/1.1"));
  CHECK_EQ(server->last_content_type(), std::string("application/x-protobuf"));
  delete server;
}

// 3. Zero-robot-impact on a socket-absent UDS endpoint: no listener at the path
//    => the export fails best-effort within the bounded curl timeouts. No hang,
//    no throw, the batch is simply dropped (identical to the TCP dead-agent path).
TEST_CASE(transport_uds_socket_absent_is_bounded_drop)
{
  const std::string path = temp_sock_path("absent");
  ::unlink(path.c_str());   // ensure nothing is listening there

  robotops::OtlpHttpExporter exporter("unix://" + path);
  robotops::Resource resource;
  std::vector<robotops::SpanData> batch;
  batch.push_back(make_span("dropped"));

  const auto t0 = steady_clock::now();
  const bool ok = exporter.export_spans(resource, batch);
  const std::int64_t elapsed = ms_since(t0);
  exporter.shutdown();

  std::printf(
    "[transport] UDS socket-absent export: ok=%d, elapsed=%" PRId64 " ms "
    "(best-effort drop, bounded by curl timeouts)\n", ok ? 1 : 0, elapsed);

  CHECK(!ok);                          // missing socket => best-effort failure
  const bool bounded = elapsed < 9000;  // bounded; connect fails ~immediately
  CHECK(bounded);
}

// 4. Per-batch cost: deliver the same batch N times over UDS and over TCP and
//    report the average wall-clock per round-trip. Informational (no hard
//    assertion on the delta — just proof both transports are live + cheap).
TEST_CASE(transport_per_batch_cost_uds_vs_tcp)
{
  constexpr int kIters = 200;

  robotops::Resource resource;
  resource.attributes.emplace_back("service.name", "rob441-cost");
  std::vector<robotops::SpanData> batch;
  for (int i = 0; i < 10; ++i) {
    batch.push_back(make_span("cost"));
  }

  // UDS
  const std::string path = temp_sock_path("cost");
  HttpCaptureServer * uds = HttpCaptureServer::make_uds(path);
  CHECK(uds->ok());
  double uds_us = 0.0;
  if (uds->ok()) {
    uds->start();
    robotops::OtlpHttpExporter exporter("unix://" + path);
    const auto t0 = steady_clock::now();
    for (int i = 0; i < kIters; ++i) {
      (void)exporter.export_spans(resource, batch);
    }
    uds_us = static_cast<double>(
      duration_cast<microseconds>(steady_clock::now() - t0).count()) / kIters;
    exporter.shutdown();
    uds->stop();
  }
  delete uds;

  // TCP
  HttpCaptureServer * tcp = HttpCaptureServer::make_tcp();
  CHECK(tcp->ok());
  double tcp_us = 0.0;
  if (tcp->ok()) {
    tcp->start();
    robotops::OtlpHttpExporter exporter(tcp->endpoint());
    const auto t0 = steady_clock::now();
    for (int i = 0; i < kIters; ++i) {
      (void)exporter.export_spans(resource, batch);
    }
    tcp_us = static_cast<double>(
      duration_cast<microseconds>(steady_clock::now() - t0).count()) / kIters;
    exporter.shutdown();
    tcp->stop();
  }
  delete tcp;

  std::printf(
    "[transport] per-batch round-trip (%d-span batch, %d iters): "
    "UDS=%.1f us, TCP=%.1f us\n", 10, kIters, uds_us, tcp_us);
  CHECK(uds_us > 0.0);
  CHECK(tcp_us > 0.0);
}
