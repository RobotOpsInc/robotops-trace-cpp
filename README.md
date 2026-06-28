# robotops_trace_cpp

The **C++17 tracing SDK core** for RobotOps distributed tracing — the "SDK + carrier" model: RobotOps ships a thin tracing **SDK** you link into your nodes, and the trace data rides an OTLP **carrier** to the agent, instead of correlating all traffic passively from the middleware.

> **Status: v0.1.0 (first core cut, ROB-419; OTLP wire unified on protobuf, ROB-438).** The SDK core is implemented: span machinery, thread-local context, async capture/restore, the bounded-queue batch processor, and the default OTLP/HTTP + protobuf exporter (plus a JSON console debug exporter). The public API is consolidated under the single lowercase `robotops` namespace and is intended to be stable; integrations compile against it.

## What it is

`robotops_trace_cpp` provides the SDK core (everything in namespace `robotops`):

- `ROBOTOPS_TRACE("name")` — drop-in RAII span macro for the enclosing scope
- `robotops::SpanGuard` — the underlying RAII span guard (attributes, status, events)
- `robotops::start_detached_span()` / `robotops::DetachedSpan` — a **detached, non-RAII** span for spans held open *across async boundaries* (BehaviorTree.CPP, MoveIt, ros2_control); decoupled from the thread-local current-context, ended explicitly
- thread-local trace context with deterministic parent/child nesting (no wire format)
- async context carry across threads: `capture_context()` + `ScopedContext`
- a swappable `SpanExporter` interface with a default **OTLP/HTTP + protobuf** exporter over libcurl (hand-rolled protobuf — no protobuf library, no otel-cpp), plus a **JSON console** debug exporter (`ROBOTOPS_TRACE_EXPORTER=console`)
- `robotops::init()` / `robotops::shutdown()` / `robotops::force_flush()` — explicit lifecycle (the override path for env-default auto-init)
- W3C `traceparent` `inject()` / `extract()` for cross-process propagation

It is **transport-agnostic** and deliberately **buildable without ROS** (a plain-CMake fallback), so the core survives beyond any single middleware. The only third-party runtime dependency is **libcurl**, confined to the exporter implementation. ROS framework hooks (rclcpp, BehaviorTree.CPP, ros2_control, MoveIt, …) live in the separate `robotops-trace-integrations` monorepo.

### Wire format & exporters

The default exporter POSTs the OTLP `ExportTraceServiceRequest` to `<endpoint>/v1/traces` as **protobuf** (`Content-Type: application/x-protobuf`), unifying the OTLP wire on protobuf to match the ROB-428 receiver contract (protobuf-only on `/v1/traces`). The protobuf is **hand-rolled** — there is **no protobuf library and no opentelemetry-cpp** dependency; libcurl remains the only third-party runtime dep. For local debugging, set `ROBOTOPS_TRACE_EXPORTER=console` (or `Config::exporter_kind = "console"`) to swap in the `ConsoleSpanExporter`, which serializes each batch to OTLP/JSON and prints it to stdout instead of POSTing (no network). The `InMemorySpanExporter` remains the exporter for tests.

### Transport: Unix-domain socket (default) + TCP fallback

The exporter speaks the **same OTLP/HTTP+protobuf** request (`POST /v1/traces`, `Content-Type: application/x-protobuf`) over either of two transports, selected by the **scheme** of `ROBOTOPS_OTLP_ENDPOINT` (or `Config::endpoint`):

| Endpoint | Transport |
| --- | --- |
| `unix:///abs/path/to.sock` | OTLP/HTTP+protobuf over a **Unix-domain socket** (the **default**) |
| `http://host:port` | OTLP/HTTP+protobuf over **TCP loopback** (the fallback) |

The **default endpoint is `unix:///run/robotops/trace.sock`**, matching the RobotOps Python exporter and the local agent receiver — no port, no loopback TCP, the on-host agent owns the socket. For the UDS scheme the HTTP request itself is entirely normal; libcurl's `CURLOPT_UNIX_SOCKET_PATH` routes the POST over the socket while a dummy `http://localhost/v1/traces` authority supplies the request line and `Host`. The UDS path is measurably cheaper per batch than TCP loopback (roughly 3× faster round-trip in the bundled `transport_test`).

Set `ROBOTOPS_OTLP_ENDPOINT=http://host:port` to fall back to TCP — useful inside a **container** that reaches a collector over the network rather than a bind-mounted host socket (e.g. mount `/run/robotops` into the container to keep the UDS default, or point at `http://otel-collector:4318` to use TCP). A connect failure on **either** transport (socket absent, agent down, collector unreachable) is a best-effort drop bounded by the curl timeouts — it never blocks or crashes the host (see the zero-robot-impact guarantee below).

## Install (apt)

The SDK ships as a Debian package to `apt.robotops.com`. Add the repo once:

```sh
curl -fsSL https://apt.robotops.com/robotops-public-key.asc | sudo gpg --dearmor -o /usr/share/keyrings/robotops.gpg
echo "deb [signed-by=/usr/share/keyrings/robotops.gpg] https://apt.robotops.com $(. /etc/os-release; echo $UBUNTU_CODENAME) main" \
  | sudo tee /etc/apt/sources.list.d/robotops.list
sudo apt update
```

Then install the SDK core (and any integrations you use):

```sh
sudo apt install ros-${ROS_DISTRO}-robotops-trace-cpp
```

## Usage

### Lifecycle + the macro

```cpp
#include <robotops_trace/trace.hpp>

void plan_path()
{
  ROBOTOPS_TRACE("plan_path");        // RAII span; closes at end of scope
  // ... work ...
}

int main()
{
  robotops::init();                   // reads env, builds the default OTLP exporter
  plan_path();
  robotops::shutdown();               // flushes + joins the background thread
}
```

### Env-default auto-init (`LD_PRELOAD`, the Datadog `-javaagent` model)

For a managed fleet you don't want to touch every node's `main()`. The SDK ships
a small companion shared library, **`librobotops_trace_cpp_autoinit.so`**, whose
ELF constructor calls `robotops::init()` the moment it is mapped into a process.
Set it **once** in the launch environment (a systemd unit, a launch wrapper, the
container `ENV`) and every node in that environment auto-initializes tracing with
**zero per-node code**:

```sh
# Set once in the launch env / systemd unit / container.
export LD_PRELOAD=/usr/lib/librobotops_trace_cpp_autoinit.so
# Default endpoint is unix:///run/robotops/trace.sock; override only to change it:
# export ROBOTOPS_OTLP_ENDPOINT=http://127.0.0.1:4318   # TCP-loopback fallback
```

Presence in `LD_PRELOAD` is the **opt-in**. The constructor is `noexcept` and
best-effort — `init()` is already `noexcept`, so a failed init can never throw or
crash the host process. The shim depends **only** on the SDK core (no ROS, no
extra deps), preserving the "survives beyond ROS" guarantee.

**Opt out at runtime** without changing the launch config:

```sh
export ROBOTOPS_TRACE_AUTOINIT=0    # disable just the auto-init shim
export ROBOTOPS_TRACE_ENABLED=0     # the global kill switch (also stops manual init)
```

Nodes launched **outside** the managed environment are unaffected and keep
calling `robotops::init()` explicitly — that remains the override path, and it is
idempotent with auto-init (a second `init()` is a logged no-op, never a
double-init).

> Built on both the ament and standalone paths; installs to `lib/`. With the apt
> package the absolute path is `/usr/lib/<triplet>/librobotops_trace_cpp_autoinit.so`
> (or `/opt/ros/${ROS_DISTRO}/lib/...`); use the path emitted by your install.

> **The core ships as a shared library** (`librobotops_trace_cpp.so`), and it
> **must** be — this is a hard requirement, not a packaging convenience (ROB-439).
> The core holds *process-global* tracer state: the thread-local active-span stack
> and the global span processor/exporter that `init()` publishes. The auto-init
> shim above and every integration library link the core; only a single shared
> `.so` gives them **one** shared copy of that state at runtime. If the core were
> linked statically into each, each would get its *own* tracer — auto-init's
> `init()` would be invisible to the app, and spans opened in different libraries
> would not nest into one trace. `BUILD_SHARED_LIBS` therefore defaults **ON** on
> both build paths. The public API keeps default ELF visibility so the whole
> `robotops::` surface (and that single global-state definition) is exported from
> the `.so`.

### Manual `SpanGuard` with attributes, status, and events

```cpp
#include <robotops_trace/trace.hpp>

void grasp()
{
  robotops::SpanOptions opts;
  opts.kind = robotops::SpanKind::Client;
  robotops::SpanGuard guard("grasp", opts);
  robotops::Span span = guard.span();

  span.set_attribute("robot.action.result", "SUCCEEDED");
  span.set_attribute("retry", true);
  span.set_attribute("count", static_cast<std::int64_t>(7));
  span.set_attribute("dur_s", 1.5);
  span.add_event("contact", {{"force_n", 12.0}});

  if (/* failure */ false) {
    span.set_status(robotops::StatusCode::Error, "tip slipped");
  }
}                                     // span closes + is enqueued here
```

Nesting is automatic and deterministic: a `SpanGuard` opened while another is
live (on the same thread) inherits its `trace_id` and parents under it.

### Async context carry (capture on submit, restore on run)

Intra-process propagation is thread-local, so context does **not** automatically
cross a thread/executor boundary. Capture it on the producer and re-attach it on
the worker:

```cpp
robotops::Context ctx = robotops::capture_context();   // on the calling thread

pool.submit([ctx] {
  robotops::ScopedContext scope(ctx);                  // restore for this scope
  ROBOTOPS_TRACE("async_work");                        // nests under the captured span
});
```

### Detached spans held across async boundaries (`start_detached_span`)

`SpanGuard` is RAII: it pushes itself onto the thread-local current-context stack
on construction and pops on destruction. That is exactly right for a span scoped
to a function, but **wrong** for a span you hold open *across* an async boundary
with explicit parentage — e.g. a BehaviorTree.CPP node that ticks, returns
`RUNNING`, and is resumed on a later tick (and, next, MoveIt and ros2_control).
A held-open `SpanGuard` leaves the worker thread's current-context pointing at a
mid-execution node, so an **unrelated** span opened on that thread between async
steps would mis-nest under it.

`robotops::start_detached_span(name, opts)` is the primitive for that case. It
returns an owning, movable `robotops::DetachedSpan` that you end **explicitly**,
and — the key property — **opening or ending it never touches the thread-local
current-context stack**: `current_span()` / `current_context()` on the calling
thread are left completely untouched. Its parent is set explicitly via
`SpanOptions.parent`; if none is given it snapshots the current context *at open
time* but still does not make itself current. On `end()` it finalizes and
enqueues through the same batch-processor + exporter plumbing as `SpanGuard`.

```cpp
// On the thread that starts the async operation:
robotops::SpanOptions opts;
opts.parent = &parent_ctx;                 // explicit parentage (or omit to snapshot current)
robotops::DetachedSpan op = robotops::start_detached_span("bt.node.GraspObject", opts);
op.set_attribute("bt.node.type", "Action");
// ... current_span()/current_context() on this thread are UNCHANGED ...

// Later, possibly on a different tick / thread, when the operation completes:
op.set_status(robotops::StatusCode::Ok);
op.end();                                  // finalize + enqueue; thread-local still untouched
// (the destructor ends it as a safety net if you forget)
```

This is the **recommended primitive for spans held across async boundaries**
(BehaviorTree.CPP, MoveIt, ros2_control). For ordinary scoped spans, keep using
`ROBOTOPS_TRACE` / `SpanGuard`.

### Cross-process propagation (W3C `traceparent`)

```cpp
std::string header = robotops::inject(robotops::current_context());  // "00-...-...-01"
// ... send `header` over the wire ...
robotops::SpanContext parent = robotops::extract(received_header);   // remote=true
robotops::SpanOptions opts;
opts.parent = &parent;
robotops::SpanGuard guard("handle_request", opts);
```

### Custom exporter (e.g. for tests)

```cpp
auto mem = std::make_shared<robotops::InMemorySpanExporter>();
robotops::Config cfg;
cfg.service_name = "grasp_node";
cfg.exporter = mem;                       // null => default OTLP/HTTP + protobuf exporter
robotops::init(cfg);
// ... open spans ...
robotops::force_flush(std::chrono::seconds(2));
auto spans = mem->spans();                // inspect exported SpanData
robotops::shutdown();
```

## Zero-robot-impact guarantee

Tracing is observability for robots, and a robot must keep moving even when its
telemetry is broken. The SDK core upholds a hard **Zero-Robot-Impact Invariant**
(ROB-418): a tracing failure must never crash, throw into, or **block** the host
process. Concretely:

- **Export is async and never back-pressures the caller.** Closing a span is a
  non-blocking enqueue onto a **bounded** queue; a single background thread drains
  it in batches. When the queue is full, the span is **dropped and counted**
  (drop-when-full) — the robot's hot path is never blocked waiting for export.
- **No lock is held across network I/O.** The worker swaps a batch *out* of the
  queue under the lock, **releases** the lock, *then* POSTs. A wedged or slow agent
  cannot stall callers trying to enqueue.
- **Every network call is time-bounded.** The libcurl exporter sets bounded
  connect (1 s) and total (5 s) timeouts and `CURLOPT_NOSIGNAL`, so a dead/slow
  agent costs at most one bounded timeout, never an unbounded hang.
- **Best-effort, fail-quiet.** A failed export is logged at debug and the batch
  dropped; the public API is `noexcept` and never throws into your code.
- **Kill switch.** `ROBOTOPS_TRACE_ENABLED=0` (or an uninitialized tracer) makes
  every operation a cheap no-op that never touches the network.

This is proven empirically, not just by inspection: `test/fault_injection_test.cpp`
points the real exporter at a black-hole endpoint (accepts the connection, never
responds) and measures that — while the export thread is wedged in a 5 s POST —
app threads still mint ~200k spans in ~15 ms (≈0.075 µs/enqueue), excess spans are
dropped, and `force_flush()`/`shutdown()` return in bounded time. It runs on both
the standalone and ament test paths (see **Building** below).

### Environment variables

All `Config` fields have an env override; env always wins, so a fleet can retune
or kill-switch without a redeploy.

| Variable | Effect |
| --- | --- |
| `ROBOTOPS_SERVICE_NAME` | `service.name` resource attribute |
| `ROBOTOPS_OTLP_ENDPOINT` | OTLP endpoint; scheme selects transport — `unix:///abs/path` (UDS, the default `unix:///run/robotops/trace.sock`) or `http://host:port` (TCP fallback). `/v1/traces` is the request path on either |
| `ROBOTOPS_TRACE_EXPORTER` | default exporter: `otlp` (OTLP/HTTP + protobuf, the default) or `console` (JSON debug sink to stdout) |
| `ROBOTOPS_TRACE_ENABLED` | `0`/`false`/`off` hard-disables tracing (the runtime kill switch); also suppresses the `LD_PRELOAD` auto-init shim |
| `ROBOTOPS_TRACE_AUTOINIT` | `0`/`false`/`off` suppresses **only** the `LD_PRELOAD` auto-init shim (ROB-421); explicit `init()` still works |
| `ROBOTOPS_TRACE_MAX_QUEUE` | bounded queue capacity (drop-newest when full) |
| `ROBOTOPS_TRACE_MAX_BATCH` | max spans per export call |
| `ROBOTOPS_TRACE_SCHEDULE_DELAY_MS` | periodic flush interval |
| `ROBOTOPS_TRACE_DEBUG` | when set, log dropped/failed exports to stderr |

```sh
# UDS (default) — the on-host agent owns /run/robotops/trace.sock:
export ROBOTOPS_OTLP_ENDPOINT=unix:///run/robotops/trace.sock
# ...or the TCP-loopback fallback:
export ROBOTOPS_OTLP_ENDPOINT=http://127.0.0.1:4318
```

## Building

### ament / ROS (the shipped path)

This package builds inside the ROS toolchain (Docker, since ROS isn't installed on macOS hosts):

```sh
just build      # build the dev image
just compile    # colcon build inside the container
just test       # build + test inside the container
```

### Standalone (plain CMake, no ROS) — the "survives beyond ROS" guardrail

The core must keep building without ROS. CI enforces this with the `cmake-standalone` job; reproduce it locally:

```sh
cmake -B build -S . -DROBOTOPS_TRACE_STANDALONE=ON
cmake --build build
./build/robotops_trace_version_check   # link + run smoke check
./build/robotops_trace_tests           # the full SDK-core test suite
ctest --test-dir build --output-on-failure   # + the shared-lib sharing proofs
# or: just standalone
```

Because the core is shared by default (ROB-439), `ctest` also runs the
cross-DSO sharing proofs: the **`LD_PRELOAD`** auto-init round-trip
(`robotops_trace_autoinit_preload` / `_optout`, ROB-421) and the **multi-lib
nesting** test (`robotops_trace_multilib_nesting`) — two separate shared libs that
each link the core and prove a span opened in one nests under a span opened in the
other via the shared thread-local stack. (These two checks are Linux-only: they
rely on `LD_PRELOAD`/`ELF`; macOS uses `DYLD_INSERT_LIBRARIES`.) Configure with
`-DBUILD_SHARED_LIBS=OFF` to force a static core for niche embedding, but then the
cross-DSO sharing guarantee no longer holds and those tests are skipped.

The standalone path requires libcurl development headers (`libcurl4-openssl-dev`
on Debian/Ubuntu; `brew install curl` / the macOS SDK provides it locally).

## Versioning & releases

- **Source of truth:** `package.xml` (`<version>`). Changelog: `CHANGELOG.rst` (`X.Y.Z (YYYY-MM-DD)`).
- Bump with `just bump-version {patch|minor|major}`, then fill in `CHANGELOG.rst`.
- Branching: feature/fix/task/chore branches → `development` → `main`. Releases are cut by the `Release` / `Release Development` GitHub Actions (never manually).
- A core **major** bump fans out a coordinated `robotops-trace-integrations` bump (integrations pin the core `>=<minor>,<<next-major>`).

## License

Apache-2.0. See [LICENSE](LICENSE).

## AI contribution policy

Disclose substantial AI-generated content in the commit message and PR description, and review AI-assisted work before submission. This scaffold was AI-generated for developer review.
