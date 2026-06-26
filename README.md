# robotops_trace_cpp

The **C++17 tracing SDK core** for RobotOps distributed tracing — the "SDK + carrier" model: RobotOps ships a thin tracing **SDK** you link into your nodes, and the trace data rides an OTLP **carrier** to the agent, instead of correlating all traffic passively from the middleware.

> **Status: scaffold (v0.1.0).** This repo currently contains the package skeleton, the public API *shape*, and the full CI/CD scaffold. The real SDK tracing logic (span machinery, thread-local context, async capture/restore, OTLP exporter) lands in **ROB-419**. API and behaviour will change before the first feature release.

## What it is

`robotops_trace_cpp` provides the SDK core:

- `ROBOTOPS_TRACE()` — drop-in RAII span macro for the enclosing scope
- `RobotOps::SpanGuard` — the underlying RAII span guard
- thread-local trace context with async capture/restore *(ROB-419)*
- an OTLP exporter *(ROB-419)*
- `RobotOps::init()` / `RobotOps::shutdown()` — explicit lifecycle (the override path for env-default auto-init)

It is **transport-agnostic** and deliberately **buildable without ROS** (a plain-CMake fallback), so the core survives beyond any single middleware. ROS framework hooks (rclcpp, BehaviorTree.CPP, ros2_control, MoveIt, …) live in the separate `robotops-trace-integrations` monorepo.

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

## API sketch

```cpp
#include <robotops_trace/trace.hpp>

void plan_path()
{
  ROBOTOPS_TRACE("plan_path");        // RAII span; closes at end of scope
  // ... work ...
}

int main()
{
  RobotOps::init();                   // explicit init (override path)
  plan_path();
  RobotOps::shutdown();
}
```

In the env-default auto-init model (`LD_PRELOAD` constructor lib, ROB-421) you don't call `init()` yourself — it runs on load. Point the exporter at the local OTLP endpoint:

```sh
export ROBOTOPS_OTLP_ENDPOINT=127.0.0.1:4317
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
./build/robotops_trace_version_check
# or: just standalone
```

## Versioning & releases

- **Source of truth:** `package.xml` (`<version>`). Changelog: `CHANGELOG.rst` (`X.Y.Z (YYYY-MM-DD)`).
- Bump with `just bump-version {patch|minor|major}`, then fill in `CHANGELOG.rst`.
- Branching: feature/fix/task/chore branches → `development` → `main`. Releases are cut by the `Release` / `Release Development` GitHub Actions (never manually).
- A core **major** bump fans out a coordinated `robotops-trace-integrations` bump (integrations pin the core `>=<minor>,<<next-major>`).

## License

Apache-2.0. See [LICENSE](LICENSE).

## AI contribution policy

Disclose substantial AI-generated content in the commit message and PR description, and review AI-assisted work before submission. This scaffold was AI-generated for developer review.
