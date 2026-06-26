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
    test/otlp_protobuf_test.cpp)
  target_include_directories(robotops_trace_tests
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/test
      ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(robotops_trace_tests PRIVATE ${PROJECT_NAME})
  add_test(NAME robotops_trace_tests COMMAND robotops_trace_tests)

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
    test/otlp_protobuf_test.cpp)
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

ament_package()
