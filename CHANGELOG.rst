^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
Changelog for package robotops_trace_cpp
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

0.1.0 (2026-06-26)
-------------------

* Initial scaffold of the RobotOps C++ tracing SDK core for the "SDK + carrier" distributed-tracing pivot (ROB-433). Provides a minimal, buildable ament_cmake package skeleton plus the full CI/CD scaffold (``ci.yml`` with the ``cmake-standalone`` guardrail job, ``branch-name-validation.yml``, ``version-check.yml``, ``release.yml``, ``release-dev.yml``, and the shared ``publish-debian-s3`` action), a multi-stage ``Dockerfile`` (jazzy/humble), a ``justfile`` (``bump-version`` + CI recipes), and the ``ROBOTOPS_TRACE_STANDALONE`` plain-CMake path that builds the core without ROS.
* Public API surface stub (``include/robotops_trace/trace.hpp``): the ``ROBOTOPS_TRACE()`` macro, the RAII ``RobotOps::SpanGuard``, and ``RobotOps::init()`` / ``shutdown()``, plus ``robotops_trace::version()``. Bodies are placeholders — the real SDK tracing logic lands in ROB-419.
