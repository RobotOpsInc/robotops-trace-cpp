# Copyright 2026 Robot Ops Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# CONFIG_EXTRAS injected into the installed robotops_trace_cppConfig.cmake (wired
# via ament_package(CONFIG_EXTRAS ...) in CMakeLists.txt). It runs whenever a
# downstream package does find_package(robotops_trace_cpp).
#
# Why this exists: the core links libcurl PRIVATE, but because the core ships as
# a STATIC library, CMake propagates the curl dependency into the exported
# target's INTERFACE_LINK_LIBRARIES (as $<LINK_ONLY:CURL::libcurl>) so the
# downstream final link can resolve it. That means the CURL::libcurl IMPORTED
# target must exist when a consumer pulls in robotops_trace_cpp's exported
# targets — otherwise find_package(robotops_trace_cpp) fails to configure. We
# re-find CURL here so consumers DON'T have to call find_package(CURL) themselves
# (this is the find_dependency-style transitive resolution; see also
# ament_export_dependencies(CURL)).
find_package(CURL REQUIRED)
