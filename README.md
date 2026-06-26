# robotops_trace_cpp

The **C++17 tracing SDK core** for RobotOps distributed tracing — the "SDK + carrier" model: RobotOps ships a thin tracing **SDK** you link into your nodes, and the trace data rides an OTLP **carrier** to the agent, instead of correlating all traffic passively from the middleware.

> **Status: v0.1.0 (first core cut, ROB-419).** The SDK core is implemented: span machinery, thread-local context, async capture/restore, the bounded-queue batch processor, and the OTLP/HTTP-JSON exporter. The public API is consolidated under the single lowercase `robotops` namespace and is intended to be stable; integrations compile against it.

## What it is

`robotops_trace_cpp` provides the SDK core (everything in namespace `robotops`):

- `ROBOTOPS_TRACE("name")` — drop-in RAII span macro for the enclosing scope
- `robotops::SpanGuard` — the underlying RAII span guard (attributes, status, events)
- thread-local trace context with deterministic parent/child nesting (no wire format)
- async context carry across threads: `capture_context()` + `ScopedContext`
- a swappable `SpanExporter` interface with a default **OTLP/HTTP + JSON** exporter over libcurl
- `robotops::init()` / `robotops::shutdown()` / `robotops::force_flush()` — explicit lifecycle (the override path for env-default auto-init)
- W3C `traceparent` `inject()` / `extract()` for cross-process propagation

It is **transport-agnostic** and deliberately **buildable without ROS** (a plain-CMake fallback), so the core survives beyond any single middleware. The only third-party runtime dependency is **libcurl**, confined to the exporter implementation. ROS framework hooks (rclcpp, BehaviorTree.CPP, ros2_control, MoveIt, …) live in the separate `robotops-trace-integrations` monorepo.

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

In the env-default auto-init model (`LD_PRELOAD` constructor lib, ROB-421) you don't call `init()` yourself — it runs on load.

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
cfg.exporter = mem;                       // null => default OTLP/HTTP-JSON exporter
robotops::init(cfg);
// ... open spans ...
robotops::force_flush(std::chrono::seconds(2));
auto spans = mem->spans();                // inspect exported SpanData
robotops::shutdown();
```

### Environment variables

All `Config` fields have an env override; env always wins, so a fleet can retune
or kill-switch without a redeploy.

| Variable | Effect |
| --- | --- |
| `ROBOTOPS_SERVICE_NAME` | `service.name` resource attribute |
| `ROBOTOPS_OTLP_ENDPOINT` | OTLP base URL; `/v1/traces` is appended (default `http://127.0.0.1:4318`) |
| `ROBOTOPS_TRACE_ENABLED` | `0`/`false`/`off` hard-disables tracing (the runtime kill switch) |
| `ROBOTOPS_TRACE_MAX_QUEUE` | bounded queue capacity (drop-newest when full) |
| `ROBOTOPS_TRACE_MAX_BATCH` | max spans per export call |
| `ROBOTOPS_TRACE_SCHEDULE_DELAY_MS` | periodic flush interval |
| `ROBOTOPS_TRACE_DEBUG` | when set, log dropped/failed exports to stderr |

```sh
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
./build/robotops_trace_tests           # the full SDK-core test suite (10 cases)
# or: just standalone
```

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
