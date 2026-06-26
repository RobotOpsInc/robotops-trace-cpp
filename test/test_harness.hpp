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

#ifndef TEST_HARNESS_HPP_
#define TEST_HARNESS_HPP_

// A deliberately tiny, header-only assertion + runner harness. The standalone
// (no-ROS) CI path has no gtest available, and we want the same test binary to
// run there and under ament, so we roll our own.

#include <cstdio>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace robotops_test
{

struct TestCase
{
  std::string name;
  std::function<void()> fn;
};

inline std::vector<TestCase> & registry()
{
  static std::vector<TestCase> cases;
  return cases;
}

// Failures for the test currently running. Reset by the runner per case.
inline int & current_failures()
{
  static int failures = 0;
  return failures;
}

struct Registrar
{
  Registrar(const std::string & name, std::function<void()> fn)
  {
    registry().push_back(TestCase{name, std::move(fn)});
  }
};

inline int run_all()
{
  int failed_cases = 0;
  for (auto & test : registry()) {
    current_failures() = 0;
    std::printf("[ RUN      ] %s\n", test.name.c_str());
    test.fn();
    if (current_failures() == 0) {
      std::printf("[       OK ] %s\n", test.name.c_str());
    } else {
      std::printf("[  FAILED  ] %s (%d check(s) failed)\n",
        test.name.c_str(), current_failures());
      ++failed_cases;
    }
  }
  std::printf("\n%zu test(s) run, %d failed.\n", registry().size(), failed_cases);
  return failed_cases == 0 ? 0 : 1;
}

}  // namespace robotops_test

#define ROBOTOPS_TEST_CONCAT_(a, b) a ## b
#define ROBOTOPS_TEST_CONCAT(a, b) ROBOTOPS_TEST_CONCAT_(a, b)

// Define + auto-register a test case.
#define TEST_CASE(name) \
  static void name(); \
  static ::robotops_test::Registrar ROBOTOPS_TEST_CONCAT(name, _registrar_)( \
    #name, &name); \
  static void name()

#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      ++::robotops_test::current_failures(); \
      std::printf("    CHECK failed: %s\n      at %s:%d\n", \
        #cond, __FILE__, __LINE__); \
    } \
  } while (0)

#define CHECK_EQ(a, b) \
  do { \
    if (!((a) == (b))) { \
      ++::robotops_test::current_failures(); \
      std::printf("    CHECK_EQ failed: %s == %s\n      at %s:%d\n", \
        #a, #b, __FILE__, __LINE__); \
    } \
  } while (0)

#endif  // TEST_HARNESS_HPP_
