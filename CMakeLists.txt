cmake_minimum_required(VERSION 3.16)
project(robotops_trace_cpp VERSION 0.1.0 LANGUAGES CXX)

# ============================================================================
# Build mode
# ============================================================================
# The SDK core is transport-agnostic and MUST survive beyond ROS. Two paths:
#
#   * ament path (default): a normal ament_cmake package, blooms to a
#     ros-<distro>-robotops-trace-cpp .deb.
#   * standalone path (-DROBOTOPS_TRACE_STANDALONE=ON): plain CMake, no ament,
#     no ROS sourced. This is the "survives beyond ROS" guardrail and is
#     exercised by the `cmake-standalone` CI job (spec §2.2). Keep the core lib
#     free of ROS includes so this path never bit-rots.
option(ROBOTOPS_TRACE_STANDALONE
  "Build the core library with plain CMake, without ament/ROS" OFF)

# C++17 required for std::string_view.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Compiler settings - strict mode.
if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# ============================================================================
# Third-party deps (identical in both build modes)
# ============================================================================
# libcurl is the ONLY new runtime dependency, and it is confined to the OTLP
# exporter .cpp — never a public header. Threads backs the batch processor.
find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

# ============================================================================
# Core library (identical in both build modes)
# ============================================================================
add_library(${PROJECT_NAME}
  src/version.cpp
  src/span.cpp
  src/context.cpp
  src/w3c.cpp
  src/exporter.cpp
  src/global.cpp
  src/otlp_http_exporter.cpp
  src/console_span_exporter.cpp
  src/detail/log.cpp
  src/detail/id_generator.cpp
  src/detail/thread_context.cpp
  src/detail/batch_processor.cpp
)

target_include_directories(${PROJECT_NAME}
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(${PROJECT_NAME} PUBLIC cxx_std_17)

# Position-independent so the static-core build can still be linked into the
# auto-init shared shim (ROB-421); a no-op when BUILD_SHARED_LIBS already makes
# the core a PIC shared object (the ament/.deb default).
set_target_properties(${PROJECT_NAME} PROPERTIES POSITION_INDEPENDENT_CODE ON)

# libcurl + threads are implementation details, so link them PRIVATE.
target_link_libraries(${PROJECT_NAME}
  PRIVATE
    CURL::libcurl
    Threads::Threads
)

install(
  TARGETS ${PROJECT_NAME}
  EXPORT export_${PROJECT_NAME}
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin
  INCLUDES DESTINATION include
)

install(
  DIRECTORY include/
  DESTINATION include
)

# ============================================================================
# Auto-init preload shim (ROB-421) — librobotops_trace_autoinit.so
# ============================================================================
# A SMALL separate shared library whose ELF constructor calls robotops::init()
# on load. Drop it into LD_PRELOAD once in the launch env (the Datadog
# -javaagent model) and every node auto-initializes tracing with zero per-node
# code; ROBOTOPS_TRACE_AUTOINIT=0 (or the ROBOTOPS_TRACE_ENABLED=0 kill switch)
# opts out at runtime. It depends ONLY on the core lib — no ROS, no extra deps —
# so the "survives beyond ROS" guarantee holds. Built + installed on BOTH the
# ament and standalone paths (this block precedes the standalone early-return).
add_library(${PROJECT_NAME}_autoinit SHARED src/autoinit.cpp)
target_link_libraries(${PROJECT_NAME}_autoinit PRIVATE ${PROJECT_NAME})

install(
  TARGETS ${PROJECT_NAME}_autoinit
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin
)

# ============================================================================
# Standalone (plain-CMake) path — no ament, no ROS
# ============================================================================
if(ROBOTOPS_TRACE_STANDALONE)
  message(STATUS "robotops_trace_cpp: STANDALONE build (no ament/ROS)")

  # A tiny smoke executable so the standalone path actually exercises a link +
  # run, not just a compile.
  add_executable(robotops_trace_version_check src/version_check_main.cpp)
  target_link_libraries(robotops_trace_version_check PRIVATE ${PROJECT_NAME})

  install(TARGETS robotops_trace_version_check RUNTIME DESTINATION bin)

  # The real SDK test suite, run via the header-only harness (no gtest on the
  # standalone path). This is what the `cmake-standalone` CI job exercises.
  enable_testing()
  add_executable(robotops_trace_tests
    test/robotops_trace_tests.cpp
    test/otlp_protobuf_test.cpp
    test/fault_injection_test.cpp)
  target_include_directories(robotops_trace_tests
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/test
      ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(robotops_trace_tests PRIVATE ${PROJECT_NAME})
  add_test(NAME robotops_trace_tests COMMAND robotops_trace_tests)

  # ROB-421: a host probe that never calls init() itself. It links the core
  # (for global::is_active()) and includes src/ for the internal global.hpp.
  # Compiling + linking it proves the auto-init surface stays buildable on the
  # standalone path even when the preload semantics aren't exercised.
  add_executable(robotops_trace_autoinit_probe test/autoinit_probe.cpp)
  target_include_directories(robotops_trace_autoinit_probe
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(robotops_trace_autoinit_probe PRIVATE ${PROJECT_NAME})

  # The LD_PRELOAD round-trip only shares the global tracer state across the
  # shim/probe boundary when the core is SHARED, so gate the preload ctests on
  # BUILD_SHARED_LIBS (configure with -DBUILD_SHARED_LIBS=ON to run them).
  if(BUILD_SHARED_LIBS)
    add_test(NAME robotops_trace_autoinit_preload
      COMMAND robotops_trace_autoinit_probe)
    set_tests_properties(robotops_trace_autoinit_preload PROPERTIES
      ENVIRONMENT "LD_PRELOAD=$<TARGET_FILE:${PROJECT_NAME}_autoinit>"
      PASS_REGULAR_EXPRESSION "active=1")

    add_test(NAME robotops_trace_autoinit_optout
      COMMAND robotops_trace_autoinit_probe)
    set_tests_properties(robotops_trace_autoinit_optout PROPERTIES
      ENVIRONMENT "LD_PRELOAD=$<TARGET_FILE:${PROJECT_NAME}_autoinit>;ROBOTOPS_TRACE_AUTOINIT=0"
      PASS_REGULAR_EXPRESSION "active=0")
  endif()

  return()
endif()

# ============================================================================
# ament path (default)
# ============================================================================
find_package(ament_cmake REQUIRED)

if(BUILD_TESTING)
  find_package(ament_lint_auto REQUIRED)
  # ament_copyright doesn't support custom copyright holders like "Robot Ops
  # Inc.", matching the rmw_robotops convention, so skip just that linter.
  set(ament_cmake_copyright_FOUND TRUE)
  ament_lint_auto_find_test_dependencies()

  # Run the same header-only test suite under ament/ctest (colcon test picks it
  # up). No gtest dependency — the harness is self-contained.
  add_executable(robotops_trace_tests
    test/robotops_trace_tests.cpp
    test/otlp_protobuf_test.cpp
    test/fault_injection_test.cpp)
  target_include_directories(robotops_trace_tests
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/test
      ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(robotops_trace_tests PRIVATE ${PROJECT_NAME})
  add_test(NAME robotops_trace_tests COMMAND robotops_trace_tests)
endif()

ament_export_include_directories(include)
ament_export_libraries(${PROJECT_NAME})
ament_export_targets(export_${PROJECT_NAME} HAS_LIBRARY_TARGET)
# The core links libcurl PRIVATE, but as a STATIC lib the curl dependency rides
# on the exported target's link interface ($<LINK_ONLY:CURL::libcurl>). Export
# CURL as a transitive dependency so downstream find_package(robotops_trace_cpp)
# resolves the CURL::libcurl IMPORTED target without each consumer having to call
# find_package(CURL) itself. The CONFIG_EXTRAS below re-finds CURL in the
# installed Config for the same reason (belt-and-suspenders for FindCURL-style
# system deps).
ament_export_dependencies(CURL)

ament_package(CONFIG_EXTRAS "cmake/robotops_trace_cpp-extras.cmake")
