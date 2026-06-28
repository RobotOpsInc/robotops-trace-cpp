^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
Changelog for package robotops_trace_cpp
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

0.4.0 (2026-06-28)
-------------------

* Array-valued ``AttributeValue`` (ROB-444). The core attribute type
  (``include/robotops_trace/span.hpp``) was scalar-only
  (string/bool/int64/double); it now also carries homogeneous **arrays** of each
  — ``Type::{StringArray,BoolArray,IntArray,DoubleArray}`` with
  ``std::vector<std::string>`` / ``std::vector<bool>`` /
  ``std::vector<std::int64_t>`` / ``std::vector<double>`` storage, owning-vector
  ctors plus braced-list conveniences (``{"a", "b"}`` / ``{1.0, 2.0}`` flow
  straight through ``set_attribute(key, AttributeValue)``), and matching
  ``*_array_value()`` accessors. **Why:** semconv array keys
  (``robot.joint.name`` string[], ``robot.target.position`` double[]) could not
  be represented, so the ros2_control integration had to lossily comma-join joint
  names. Arrays serialize to the canonical OTLP ``arrayValue`` — AnyValue field 5
  => ``ArrayValue { repeated AnyValue values = 1 }`` — in **both** exporters: the
  hand-rolled protobuf wire writer (``src/otlp_http_exporter.cpp``, each element
  recursing into the existing scalar AnyValue encoding) and the console/JSON debug
  sink (``src/console_span_exporter.cpp``, ``{"arrayValue":{"values":[...]}}``).
  ``Span`` / ``DetachedSpan`` / ``SpanGuard`` ``set_attribute`` are unchanged —
  the array ctors flow through the existing ``AttributeValue`` overload — and
  ``SpanData`` carries the array values end-to-end with no schema change. New
  tests prove string[]/bool[]/int[]/double[] attributes set on a live span survive
  into the exported ``SpanData`` with element types + values intact, and a small
  in-process protobuf reader decode-verifies the ``arrayValue`` wire
  element-for-element (cross-checked out-of-band against the canonical
  ``opentelemetry-proto`` decoder, the ROB-438 pattern). ``-Wall -Wextra
  -Wpedantic`` clean; the standalone (libcurl) suite + the full suite stay green.
  This unblocks real string[]/double[] semconv attributes for the ros2_control /
  MoveIt integrations (no more comma-join).
* Harden the ``Dockerfile`` ``just`` install against the recurring arm64 CI flake
  (ROB-444). The install was ``curl ... | bash ... || echo "Warning: just
  installation failed, but continuing..."`` — so a flaky download SILENTLY
  continued and the build cached a broken layer, then CI died much later with
  ``just: not found`` (the humble-arm64 flake on ROB-441/443). The ``|| echo
  continuing`` is dropped and the step now fails **hard** and **verifies** the
  binary: ``... | bash -s -- --to /usr/local/bin && command -v just && just
  --version``. A failed or empty install now fails the image build immediately
  with a clear error (and never caches a broken layer), so the retry is clean. The
  install method is otherwise unchanged.

0.3.0 (2026-06-27)
-------------------

* Detached (non-RAII) span API for spans held open ACROSS async boundaries
  (ROB-443). Adds ``robotops::start_detached_span(name, SpanOptions = {})``
  returning an owning, movable ``robotops::DetachedSpan`` handle
  (``set_attribute`` / ``add_event`` / ``set_status`` / ``context()`` / ``span()``
  + an explicit ``end()``, with end-on-destruct as a safety net). The new
  primitive sits ALONGSIDE the existing RAII ``SpanGuard`` whose behavior is
  unchanged. **Why** (ROB-424): the only span primitive was the RAII
  ``SpanGuard``, which on construct/destruct pushes/pops the thread-local
  current-context. Integrations that hold a span open across async boundaries and
  set parentage EXPLICITLY (BehaviorTree.CPP now; MoveIt + ros2_control next)
  don't want that coupling — a held-open span leaves the worker thread's
  current-context pointing at a mid-execution node, so an unrelated span opened on
  that thread between async steps could mis-nest. **Key invariant:** opening *or*
  ending a ``DetachedSpan`` NEVER modifies the thread-local current-context stack
  — ``current_span()`` / ``current_context()`` on the calling thread are
  untouched (proven by ``test/detached_span_test.cpp``). It still mints a real
  span (trace_id/span_id; parent from ``SpanOptions.parent`` if valid, else the
  thread-local current context AT OPEN TIME, else a new root) and on ``end()``
  finalizes → enqueues to the exporter through the SAME span-record +
  batch-processor + exporter plumbing as ``SpanGuard`` — only the thread-local
  push/pop is omitted. ``noexcept`` / best-effort / no-op when disabled, like the
  rest of the public API. New tests prove: a detached open does not change the
  calling thread's current context (with and without an active ``SpanGuard``); an
  explicit ``SpanOptions.parent`` nests correctly; a detached span with no
  explicit parent picks up the current context as parent but does not itself
  become current; and attributes/status/events survive to the exported
  ``SpanData`` with idempotent ``end()`` (double-end is safe) and a disabled-SDK
  no-op. ``-Wall -Wextra -Wpedantic`` clean; all existing tests stay green. This
  is the recommended primitive for held-across-async spans and unblocks clean
  MoveIt / ros2_control integration.

0.2.0 (2026-06-26)
-------------------

* Unix-domain-socket transport (default) + TCP-loopback fallback for the OTLP/HTTP
  exporter (ROB-441). The ``ROBOTOPS_OTLP_ENDPOINT`` (``Config::endpoint``) scheme now
  selects the transport: ``unix:///abs/path`` rides a **Unix-domain socket**,
  ``http://host:port`` rides **TCP loopback**. The default endpoint changes from
  ``http://127.0.0.1:4318`` to **``unix:///run/robotops/trace.sock``**, matching the
  RobotOps Python exporter and the on-host agent receiver (no port, the agent owns the
  socket). The HTTP request itself is unchanged on either transport — ``POST /v1/traces``
  with ``Content-Type: application/x-protobuf``; for the UDS scheme the exporter sets
  libcurl's ``CURLOPT_UNIX_SOCKET_PATH`` to route an otherwise-normal POST over the socket
  while a dummy ``http://localhost/v1/traces`` authority supplies the request line + Host.
  libcurl stays the only third-party runtime dependency. A new ``test/transport_test.cpp``
  stands up throwaway in-process HTTP servers — one bound to a temp AF_UNIX ``.sock``, one
  to a loopback AF_INET port — and proves a REAL round-trip: the server receives the
  **exact** serialized protobuf bytes with request line ``POST /v1/traces HTTP/1.1`` and
  ``Content-Type: application/x-protobuf`` over both transports, the measured per-batch
  round-trip is ~3× cheaper over UDS than TCP loopback, and a **socket-absent** UDS
  endpoint is a best-effort drop bounded by the curl timeouts (no hang, no throw) —
  preserving the Zero-Robot-Impact Invariant identically to the TCP dead-agent path
  (ROB-440 / ROB-418). ``-Wall -Wextra -Wpedantic`` clean; all existing tests stay green.
* Ship the C++ core as a **shared library** (ROB-439). ``librobotops_trace_cpp``
  now builds and installs as ``librobotops_trace_cpp.so`` (``BUILD_SHARED_LIBS``
  defaults **ON** on both the ament/.deb and standalone paths) instead of the
  former static ``.a``. **Why:** the core holds *process-global* tracer state —
  the thread-local active-span stack (``src/detail/thread_context.cpp``) and the
  global span processor/exporter published by ``init()`` (``src/global.cpp``). The
  ``LD_PRELOAD`` auto-init shim (ROB-421) and every integration lib link the core;
  if each *statically* linked it, each would get its **own** copy of that state, so
  auto-init could not share context and spans could not nest across libraries. A
  single shared ``.so`` gives every DSO in the process **one** copy of the global
  state, resolved at runtime. Public API keeps **default ELF visibility** (no
  ``-fvisibility=hidden``), so the whole ``robotops::`` surface — and the single
  definition of the global state — is exported from the ``.so``; the auto-init shim
  carries ``robotops::init`` as an **undefined** symbol bound to the core ``.so`` at
  load time (verified with ``nm``/``ldd``). Two new empirical proofs on the
  standalone path, now that shared is the default: the ROB-421 preload ctests are
  **ungated** (an ``LD_PRELOAD``ed shim ``init()``s the core and a probe that never
  calls ``init()`` observes the *same* active tracer — its minted span is exported
  by the shim-owned processor), and a new **multi-lib nesting** test
  (``test/multilib_{a,b,main}.cpp``) loads two *separate* shared libs that each link
  the core and proves a span opened in lib B nests under one opened in lib A (same
  ``trace_id``, ``B.parent_span_id == A.span_id``) via the shared thread-local
  stack. ``-Wall -Wextra -Wpedantic`` clean; all existing tests stay green. This
  unblocks ROB-421 auto-init and multi-integration span nesting.
* Empirical proof of the Zero-Robot-Impact Invariant (ROB-440 / ROB-418). A
  fault-injection test suite (``test/fault_injection_test.cpp``) points the real
  libcurl OTLP exporter at a **black-hole** loopback endpoint — a socket that
  accepts the TCP connection but never reads or responds, so every POST hangs to
  its ``CURLOPT_TIMEOUT_MS`` — and proves, with measured wall-clock, that a wedged
  agent never reaches the host: the queue-lock is released before the POST (the
  worker swaps a batch OUT under the lock, then exports OUTSIDE it), so span-mint +
  enqueue stays fast and is **not** gated on the stalled export (≈0.075 µs/enqueue;
  200k spans minted in ~15 ms while the export thread was blocked in a 5 s POST),
  excess spans are **dropped** (bounded queue, drop-when-full), and
  ``force_flush(timeout)`` / ``shutdown()`` return in bounded time with no infinite
  hang. The exporter's bounded connect/total timeouts (1 s / 5 s, ``CURLOPT_NOSIGNAL``)
  and the disabled-kill-switch no-op are asserted on the same dead-endpoint path.
  Wired into both the standalone and ament(``BUILD_TESTING``) test runners.
* Env-default auto-init via ``LD_PRELOAD`` (ROB-421). A new small, separate
  shared library ``librobotops_trace_cpp_autoinit.so`` (target
  ``robotops_trace_cpp_autoinit``) carries an ``__attribute__((constructor))``
  that calls ``robotops::init()`` on load — the Datadog ``-javaagent`` model:
  set ``LD_PRELOAD`` once in the launch env / systemd unit and every node
  auto-initializes tracing with zero per-node code. Presence in ``LD_PRELOAD``
  is the opt-in; ``ROBOTOPS_TRACE_AUTOINIT=0`` (or the ``ROBOTOPS_TRACE_ENABLED=0``
  kill switch) suppresses it at runtime. Explicit ``robotops::init()`` remains the
  override and stays idempotent with auto-init. The shim depends ONLY on the core
  (no ROS, no extra deps) and is ``noexcept``/best-effort so a failed init can
  never crash the host. Built + installed to ``lib/`` on both the ament and
  standalone paths; a standalone preload probe (``test/autoinit_probe.cpp``) plus
  two ctests assert the shim activates the tracer and honors the opt-out.

0.1.0 (2026-06-26)
-------------------

* Initial scaffold of the RobotOps C++ tracing SDK core for the "SDK + carrier" distributed-tracing pivot (ROB-433). Provides a minimal, buildable ament_cmake package skeleton plus the full CI/CD scaffold (``ci.yml`` with the ``cmake-standalone`` guardrail job, ``branch-name-validation.yml``, ``version-check.yml``, ``release.yml``, ``release-dev.yml``, and the shared ``publish-debian-s3`` action), a multi-stage ``Dockerfile`` (jazzy/humble), a ``justfile`` (``bump-version`` + CI recipes), and the ``ROBOTOPS_TRACE_STANDALONE`` plain-CMake path that builds the core without ROS.
* SDK core implementation (ROB-419). The scaffold's placeholder bodies are replaced with the real tracing machinery, and the public API is consolidated under the single lowercase ``robotops`` namespace (superseding the ``RobotOps::`` / ``robotops_trace::`` split). The ``ROBOTOPS_TRACE()`` macro is unchanged.

  * Public headers split into ``span.hpp`` (``SpanContext``, ``Span``, ``SpanGuard``, ``SpanKind``, ``StatusCode``, ``AttributeValue``), ``context.hpp`` (``current_span``/``current_context``, async ``capture_context`` + ``ScopedContext``), ``exporter.hpp`` (``SpanData``, the abstract ``SpanExporter``, ``InMemorySpanExporter``), ``config.hpp`` (``Config`` + ``init``/``shutdown``/``force_flush``) and ``w3c.hpp`` (``traceparent`` inject/extract), with ``trace.hpp`` as the umbrella include.
  * Deterministic intra-process parent/child propagation via a thread-local context stack (no wire format); async carry across threads with ``capture_context()`` / ``ScopedContext``.
  * Bounded-queue + single-background-thread batch processor that drops (and counts) spans when full and never blocks the caller; periodic + ``force_flush`` draining.
  * Default OTLP/HTTP exporter over libcurl POSTing ``ExportTraceServiceRequest`` to ``<endpoint>/v1/traces``; export is behind the swappable ``SpanExporter`` interface.
  * Zero-robot-impact contract: the public API never throws, a disabled tracer (``ROBOTOPS_TRACE_ENABLED=0`` kill switch, or uninitialized) makes every operation a cheap no-op, and failed exports are best-effort (logged, dropped).
  * Env configuration: ``ROBOTOPS_SERVICE_NAME``, ``ROBOTOPS_OTLP_ENDPOINT``, ``ROBOTOPS_TRACE_ENABLED``, ``ROBOTOPS_TRACE_MAX_QUEUE``, ``ROBOTOPS_TRACE_MAX_BATCH``, ``ROBOTOPS_TRACE_SCHEDULE_DELAY_MS``.
  * New runtime dependency: ``libcurl`` (``<depend>libcurl-dev</depend>``), confined to the exporter implementation. ``CMakeLists.txt`` links ``CURL::libcurl`` on both the ament and standalone paths.
  * A 10-case test suite (``test/robotops_trace_tests.cpp``) runs via a header-only harness on the standalone path and under ament/ctest.
* Unify the OTLP wire on protobuf (ROB-438). The default exporter now serializes ``ExportTraceServiceRequest`` to **OTLP protobuf** (``Content-Type: application/x-protobuf``) instead of JSON, matching the ROB-428 receiver contract (protobuf-only on ``/v1/traces``).

  * New default ``OtlpHttpExporter`` (``src/otlp_http_exporter.*``) hand-rolls the protobuf wire with a tiny varint/tag/length-delimited/fixed64 writer — **no protobuf library and no opentelemetry-cpp**; libcurl stays the only third-party runtime dependency. trace_id/span_id/parent_span_id go on the wire as RAW bytes (not hex), timestamps as ``fixed64``, ``int64`` attributes as plain (non-zigzag) varints, doubles as IEEE754 ``fixed64``; root ``parent_span_id`` and Unset+empty ``Status`` are omitted.
  * The previous hand-rolled JSON serializer is **demoted to a debug sink**, the new ``ConsoleSpanExporter`` (``src/console_span_exporter.*``), which prints OTLP/JSON to stdout instead of POSTing. Select the default exporter with ``ROBOTOPS_TRACE_EXPORTER=otlp|console`` (or ``Config::exporter_kind``); ``InMemorySpanExporter`` is unchanged for tests.
  * Wire correctness is cross-verified against the canonical schema: a test serializes a known span tree and a ``python:3.12-slim`` + ``opentelemetry-proto`` decoder asserts every field (raw ids, name, nanos, kind, string/bool/int/negative-int/double attributes, events, status) round-trips.
