# ROB-419 — C++ SDK Core: implementation spec

Authoritative design for the first cut of the C++ tracing SDK core (the franchise
spine). The implementer follows this exactly; deviations need a note in the PR.

## Non-negotiables (from the locked decisions)

1. **Own types only in the public API.** No `opentelemetry-cpp` types anywhere in
   public headers. The only third-party runtime dep is **libcurl** (and only inside
   the exporter .cpp, never in a public header).
2. **Survives beyond ROS.** The core must build + test with plain CMake, no ROS
   sourced (`-DROBOTOPS_TRACE_STANDALONE=ON`, the existing guardrail). No ROS
   includes in the core.
3. **Export behind an interface.** `SpanExporter` is abstract; the default impl is a
   hand-rolled **OTLP/HTTP + JSON** exporter over libcurl. Swappable.
4. **Deterministic intra-process propagation via thread-local.** This is the whole
   point — parent/child nesting comes from a thread-local context stack, no wire.
5. **Zero-robot-impact:** never throw out of public API (mark `noexcept` where
   sensible, swallow + log internally); when disabled, all ops are cheap no-ops; a
   failed export never blocks or crashes the caller.

## Namespace + public API (LOCK THIS — integrations compile against it)

Single namespace **`robotops`** (lowercase — matches the Python import name and
modern C++ convention). This supersedes the scaffold stub's `RobotOps::` /
`robotops_trace::` split; update `trace.hpp` accordingly. Macro stays
`ROBOTOPS_TRACE(...)`.

### Public headers

```
include/robotops_trace/
  trace.hpp        # umbrella include — pulls in the rest; the macro
  span.hpp         # SpanContext, Span handle, SpanKind, StatusCode, AttributeValue
  context.hpp      # Context token + capture/attach (async carry), current_context
  exporter.hpp     # SpanData, SpanExporter interface, span lifecycle structs
  config.hpp       # robotops::Config, init(), shutdown()
  w3c.hpp          # traceparent inject/extract
```

### Types

```cpp
namespace robotops {

const char* version() noexcept;                 // "0.1.0", matches package.xml

// --- identity ---
struct SpanContext {
  std::array<std::uint8_t,16> trace_id{};       // all-zero == invalid
  std::array<std::uint8_t,8>  span_id{};
  std::uint8_t trace_flags{0};                  // bit0 = sampled
  bool remote{false};                           // extracted from the wire?
  bool valid() const noexcept;                  // trace_id != 0 && span_id != 0
  bool sampled() const noexcept;                // trace_flags & 0x01
  std::string trace_id_hex() const;             // 32 lowercase hex chars
  std::string span_id_hex() const;              // 16 lowercase hex chars
};

enum class SpanKind  : std::uint8_t { Internal=1, Server=2, Client=3, Producer=4, Consumer=5 };
enum class StatusCode: std::uint8_t { Unset=0, Ok=1, Error=2 };

// Attribute value: string | bool | int64 | double (v0 set; arrays later).
class AttributeValue { /* variant-backed; ctors for the 4 types + const char* */ };

// --- Span handle (non-owning view onto the currently-open span on this thread) ---
class Span {
 public:
  bool valid() const noexcept;
  const SpanContext& context() const noexcept;
  void set_attribute(std::string_view key, AttributeValue value) noexcept;
  void add_event(std::string_view name) noexcept;
  void add_event(std::string_view name,
                 std::initializer_list<std::pair<std::string_view, AttributeValue>> attrs) noexcept;
  void set_status(StatusCode code, std::string_view message = {}) noexcept;
  // ...impl holds a pointer to the live span record (or null when invalid/disabled)
};

// --- RAII span guard (what the macro expands to; also usable directly) ---
struct SpanOptions {
  SpanKind kind{SpanKind::Internal};
  const SpanContext* parent{nullptr};           // null => use thread-local current
  std::initializer_list<std::pair<std::string_view, AttributeValue>> attributes{};
};

class SpanGuard {
 public:
  explicit SpanGuard(std::string_view name, SpanOptions opts = {}) noexcept;
  ~SpanGuard();                                 // closes span, pops thread-local, enqueues to processor
  SpanGuard(SpanGuard&&) noexcept;
  SpanGuard& operator=(SpanGuard&&) noexcept;
  SpanGuard(const SpanGuard&) = delete;
  SpanGuard& operator=(const SpanGuard&) = delete;
  Span span() const noexcept;                   // handle to set attrs/status/events
};

Span current_span() noexcept;                   // handle to thread-local active span (invalid if none)
SpanContext current_context() noexcept;         // invalid if none

// --- async context carry (capture-on-submit / restore-on-run) ---
class Context {                                 // value type, copyable, holds a SpanContext snapshot
 public:
  bool valid() const noexcept;
  const SpanContext& span_context() const noexcept;
};
Context capture_context() noexcept;             // snapshot the thread-local active context

class ScopedContext {                           // RAII: restore captured context on THIS thread for the scope
 public:
  explicit ScopedContext(const Context&) noexcept;
  ~ScopedContext();                             // restores previous
  ScopedContext(const ScopedContext&) = delete;
  ScopedContext& operator=(const ScopedContext&) = delete;
};

// --- lifecycle ---
struct Config {
  std::string service_name{"unknown_service"};  // env ROBOTOPS_SERVICE_NAME overrides
  std::string endpoint{"http://127.0.0.1:4318"};// env ROBOTOPS_OTLP_ENDPOINT; /v1/traces appended
  bool enabled{true};                           // env ROBOTOPS_TRACE_ENABLED=0 hard-disables (kill switch)
  std::vector<std::pair<std::string,std::string>> resource_attributes{}; // extra resource attrs
  std::size_t max_queue{2048};                  // env ROBOTOPS_TRACE_MAX_QUEUE
  std::size_t max_batch{512};                   // env ROBOTOPS_TRACE_MAX_BATCH
  std::chrono::milliseconds schedule_delay{5000};// env ROBOTOPS_TRACE_SCHEDULE_DELAY_MS, periodic flush
  std::shared_ptr<class SpanExporter> exporter{};// null => default OTLP/HTTP-JSON exporter built from endpoint
};

void init() noexcept;                           // reads env, builds default config
void init(Config) noexcept;                     // explicit override; idempotent (second call is a no-op + warn)
void shutdown() noexcept;                        // flush + join the processor; idempotent
bool force_flush(std::chrono::milliseconds timeout) noexcept;  // block until queue drained or timeout

}  // namespace robotops
```

### Exporter interface (public, for testability + custom backends)

```cpp
namespace robotops {

struct EventData { std::uint64_t time_unix_nano; std::string name;
                   std::vector<std::pair<std::string,AttributeValue>> attributes; };
struct SpanData {                               // the closed-span record handed to exporters
  SpanContext context;
  std::array<std::uint8_t,8> parent_span_id{};  // zero == root
  std::string name;
  SpanKind kind{SpanKind::Internal};
  std::uint64_t start_unix_nano{};
  std::uint64_t end_unix_nano{};
  StatusCode status_code{StatusCode::Unset};
  std::string status_message;
  std::vector<std::pair<std::string,AttributeValue>> attributes;
  std::vector<EventData> events;
};

struct Resource { std::vector<std::pair<std::string,std::string>> attributes; };

class SpanExporter {
 public:
  virtual ~SpanExporter() = default;
  // Export a batch. MUST NOT throw. Return false on failure (logged, dropped — best-effort).
  virtual bool export_spans(const Resource& resource, const std::vector<SpanData>& spans) noexcept = 0;
  virtual bool force_flush(std::chrono::milliseconds timeout) noexcept { (void)timeout; return true; }
  virtual void shutdown() noexcept {}
};

// In-memory exporter for tests.
class InMemorySpanExporter : public SpanExporter { /* stores batches; thread-safe accessor */ };

}  // namespace robotops
```

### The macro

```cpp
#define ROBOTOPS_TRACE(name) \
  ::robotops::SpanGuard ROBOTOPS_TRACE_CONCAT(robotops_span_, __LINE__){name}
// keep the CONCAT helpers from the stub
```

## Internal architecture (`src/` + `src/detail/` — not installed)

- **`detail/id_generator`** — cryptographically-uninteresting but well-distributed
  random 16/8-byte IDs. Use `std::random_device`-seeded `std::mt19937_64` in a
  `thread_local` engine (no lock). Never emit all-zero IDs.
- **`detail/clock`** — `now_unix_nano()` from `std::chrono::system_clock` (wall clock,
  for OTLP timestamps).
- **`detail/thread_context`** — `thread_local` stack of active span records
  (pointer + previous-context). `push(record)`, `pop()`, `current()`. This is the
  deterministic intra-process propagation. A span record owns its mutable
  attributes/events while open; on `SpanGuard` dtor it's finalized into a `SpanData`
  and enqueued, then the record is freed.
  - Span record stores: SpanContext, parent_span_id, name, kind, start ns, status,
    attrs, events. `current_span()`/`current_context()` read the top.
  - Parent resolution in `SpanGuard` ctor: if `opts.parent` non-null use it; else if
    thread-local stack non-empty, parent = top's span_id and **inherit its trace_id**;
    else this is a **root** — mint a new trace_id, parent = zero.
- **`detail/batch_processor`** — bounded queue + one background thread. On `SpanGuard`
  dtor the finalized `SpanData` is pushed (drop-newest + count a dropped-spans stat if
  full — never block the caller). Background thread drains in batches of `max_batch`,
  calls `exporter->export_spans(resource, batch)`, and also flushes every
  `schedule_delay`. `force_flush`/`shutdown` signal + join.
- **`src/otlp_http_json_exporter`** — implements `SpanExporter`. Serializes the batch
  into the OTLP/JSON `ExportTraceServiceRequest` shape (below) with a **hand-rolled
  JSON writer** (no JSON lib dep — the schema is small + fixed) and POSTs via libcurl
  to `<endpoint>/v1/traces` with `Content-Type: application/json`. Short connect +
  total timeout (e.g. 1s/5s). Reuse a single `CURL*` handle behind a mutex on the
  background thread. Non-2xx or transport error => return false (logged at debug,
  batch dropped). A single global `curl_global_init` guarded by `std::once_flag`.
- **`src/global`** — the process-wide tracer state (config, resource, processor,
  exporter) behind an atomic-initialized singleton guarded by `std::once_flag` /
  `std::mutex`. `init()` builds it; `shutdown()` tears down. When `enabled==false` or
  not initialized, `SpanGuard`/`Span`/`capture` are no-ops returning invalid handles.
- **`src/w3c`** — `inject(SpanContext) -> "00-<32hex>-<16hex>-<02hex flags>"`;
  `extract(string_view) -> SpanContext` (validate version 00, lengths, hex; set
  `remote=true`; invalid => invalid context).

## OTLP/JSON wire format (the contract ROB-428's receiver must match)

POST `<endpoint>/v1/traces`, body = OTLP `ExportTraceServiceRequest` JSON:

```json
{ "resourceSpans": [ {
  "resource": { "attributes": [ {"key":"service.name","value":{"stringValue":"grasp_node"}} ] },
  "scopeSpans": [ {
    "scope": { "name": "robotops-trace-cpp", "version": "0.1.0" },
    "spans": [ {
      "traceId": "<32 lowercase hex>",
      "spanId": "<16 lowercase hex>",
      "parentSpanId": "<16 hex>",            // omit field entirely when root (all-zero)
      "name": "plan",
      "kind": 1,
      "startTimeUnixNano": "1719421200000000000",   // uint64 as decimal STRING
      "endTimeUnixNano": "1719421200005000000",
      "attributes": [ {"key":"robot.action.result","value":{"stringValue":"SUCCEEDED"}},
                      {"key":"retry","value":{"boolValue":true}},
                      {"key":"count","value":{"intValue":"7"}},          // int64 as STRING
                      {"key":"dur","value":{"doubleValue":1.5}} ],
      "events": [ {"timeUnixNano":"...","name":"grasp aborted","attributes":[...]} ],
      "status": { "code": 2, "message": "..." }   // omit message when empty; code: 0 unset,1 ok,2 error
    } ]
  } ]
} ] }
```

Notes: OTLP/JSON encodes trace/span IDs as **hex strings** and all 64-bit ints
(timestamps, intValue) as **decimal strings**. JSON-escape attribute string values
properly. Omit empty/zero optional fields.

## CMake / build wiring

- Add `find_package(CURL REQUIRED)` and link `CURL::libcurl` to the core lib (both
  ament and standalone paths). libcurl is the only new runtime dep.
- `package.xml`: add `<depend>libcurl-dev</depend>` (rosdep key `libcurl-dev`).
- Keep the standalone path building the lib + the existing version_check smoke exe,
  **plus** the new test executable (below) so `cmake-standalone` CI exercises real
  logic.
- Source files compiled into the existing `robotops_trace_cpp` target.

## Tests (must pass on the standalone path — that's what CI runs locally + in cmake-standalone)

Use a tiny header-only assertion harness (no gtest dep on the standalone path — gtest
is only available under ament). Put tests behind `if(ROBOTOPS_TRACE_STANDALONE OR BUILD_TESTING)`.
Test executable `robotops_trace_tests`. Cover:

1. **version()** returns "0.1.0".
2. **Root span** mints a valid, sampled trace; parent_span_id is zero.
3. **Nesting** — a SpanGuard opened inside another inherits the parent's trace_id and
   sets parent_span_id to the outer's span_id (read via an InMemorySpanExporter after
   force_flush).
4. **Sibling spans** under the same parent share trace_id, differ in span_id.
5. **Thread-local isolation** — spans on two std::threads get independent trace trees.
6. **Async carry** — capture_context() on thread A, ScopedContext on thread B makes a
   span on B nest under A's span (same trace_id, parent = A's span_id).
7. **Attributes/status/events** survive into the exported SpanData.
8. **W3C round-trip** — inject(ctx) then extract() yields equal ids/flags, remote=true.
9. **Disabled** (`ROBOTOPS_TRACE_ENABLED=0` or no init) — SpanGuard is a no-op,
   current_span() invalid, nothing exported, no crash.
10. **InMemorySpanExporter** wiring via Config.exporter; force_flush drains.

(Do NOT unit-test the live HTTP POST in CI — no agent listening. Optionally a guarded
manual test. The InMemorySpanExporter covers the pipeline.)

## Out of scope for this issue (later)
- Sampling / tail-sampling (everything sampled now).
- protobuf exporter (interface allows it later).
- LD_PRELOAD auto-init constructor (ROB-421).
- ROS / rclcpp anything (ROB-422 + integrations repo).
- Semantic-convention key constants (ROB-430, lives in integrations repo).

## Deliverable
Implement on branch `feature/rob-419-sdk-core-cpp`. Build + run tests via the
standalone path locally and report results. Update `CHANGELOG.rst` + `README.md` (real
API examples). Commit (disclose AI authorship per org policy). Push. Open a PR into
`development` (this is the first real CI exercise for the repo — report the CI result).
