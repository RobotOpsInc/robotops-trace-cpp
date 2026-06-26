^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
Changelog for package robotops_trace_cpp
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Forthcoming
-----------

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
  * Default OTLP/HTTP + JSON exporter over libcurl (hand-rolled JSON writer, no JSON library) POSTing ``ExportTraceServiceRequest`` to ``<endpoint>/v1/traces``; export is behind the swappable ``SpanExporter`` interface.
  * Zero-robot-impact contract: the public API never throws, a disabled tracer (``ROBOTOPS_TRACE_ENABLED=0`` kill switch, or uninitialized) makes every operation a cheap no-op, and failed exports are best-effort (logged, dropped).
  * Env configuration: ``ROBOTOPS_SERVICE_NAME``, ``ROBOTOPS_OTLP_ENDPOINT``, ``ROBOTOPS_TRACE_ENABLED``, ``ROBOTOPS_TRACE_MAX_QUEUE``, ``ROBOTOPS_TRACE_MAX_BATCH``, ``ROBOTOPS_TRACE_SCHEDULE_DELAY_MS``.
  * New runtime dependency: ``libcurl`` (``<depend>libcurl-dev</depend>``), confined to the exporter implementation. ``CMakeLists.txt`` links ``CURL::libcurl`` on both the ament and standalone paths.
  * A 10-case test suite (``test/robotops_trace_tests.cpp``) runs via a header-only harness on the standalone path and under ament/ctest.
