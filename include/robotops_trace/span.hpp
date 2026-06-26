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

#ifndef ROBOTOPS_TRACE__SPAN_HPP_
#define ROBOTOPS_TRACE__SPAN_HPP_

#include <array>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

/// \file span.hpp
/// \brief Core span identity + handle types (SpanContext, Span, SpanGuard).
///
/// These are RobotOps-owned value types. No third-party (e.g. opentelemetry-cpp)
/// types leak into the public API — the only runtime dep is libcurl, and that
/// lives entirely inside the exporter .cpp.

namespace robotops
{

/// Returns the SDK core version string (matches package.xml).
const char * version() noexcept;

// ---------------------------------------------------------------------------
// Identity
// ---------------------------------------------------------------------------

/// W3C-style span identity: 16-byte trace id + 8-byte span id + flags.
struct SpanContext
{
  std::array<std::uint8_t, 16> trace_id{};   ///< all-zero == invalid
  std::array<std::uint8_t, 8> span_id{};
  std::uint8_t trace_flags{0};               ///< bit0 = sampled
  bool remote{false};                        ///< extracted from the wire?

  /// True when both ids are non-zero.
  bool valid() const noexcept;
  /// True when the sampled flag (bit0) is set.
  bool sampled() const noexcept;
  /// 32 lowercase hex chars.
  std::string trace_id_hex() const;
  /// 16 lowercase hex chars.
  std::string span_id_hex() const;
};

/// OTLP span kind. Values match the OTLP wire enum.
enum class SpanKind : std::uint8_t
{
  Internal = 1,
  Server = 2,
  Client = 3,
  Producer = 4,
  Consumer = 5
};

/// OTLP status code. Values match the OTLP wire enum.
enum class StatusCode : std::uint8_t
{
  Unset = 0,
  Ok = 1,
  Error = 2
};

// ---------------------------------------------------------------------------
// Attribute value (string | bool | int64 | double; arrays come later)
// ---------------------------------------------------------------------------

/// A tagged attribute value. Implicitly constructible from the 4 supported
/// scalar types (plus const char*), so call sites can write `{"key", 7}` etc.
class AttributeValue
{
public:
  enum class Type : std::uint8_t { String, Bool, Int, Double };

  AttributeValue() noexcept
  : type_(Type::String) {}
  AttributeValue(const char * value)            // NOLINT(runtime/explicit)
  : type_(Type::String), str_(value ? value : "") {}
  AttributeValue(std::string value)             // NOLINT(runtime/explicit)
  : type_(Type::String), str_(std::move(value)) {}
  AttributeValue(std::string_view value)        // NOLINT(runtime/explicit)
  : type_(Type::String), str_(value) {}
  AttributeValue(bool value) noexcept           // NOLINT(runtime/explicit)
  : type_(Type::Bool), bool_(value) {}
  AttributeValue(std::int64_t value) noexcept   // NOLINT(runtime/explicit)
  : type_(Type::Int), int_(value) {}
  AttributeValue(int value) noexcept            // NOLINT(runtime/explicit)
  : type_(Type::Int), int_(value) {}
  AttributeValue(double value) noexcept         // NOLINT(runtime/explicit)
  : type_(Type::Double), dbl_(value) {}

  Type type() const noexcept {return type_;}
  const std::string & string_value() const noexcept {return str_;}
  bool bool_value() const noexcept {return bool_;}
  std::int64_t int_value() const noexcept {return int_;}
  double double_value() const noexcept {return dbl_;}

private:
  Type type_;
  std::string str_;
  bool bool_{false};
  std::int64_t int_{0};
  double dbl_{0.0};
};

namespace detail
{
struct SpanRecord;  // internal open-span record (src/detail/span_record.hpp)
}  // namespace detail

/// Non-owning handle onto the live span record on the current thread. All
/// mutators are noexcept and become cheap no-ops when the handle is invalid
/// (disabled SDK, no active span, etc.).
class Span
{
public:
  Span() noexcept = default;

  bool valid() const noexcept;
  const SpanContext & context() const noexcept;

  void set_attribute(std::string_view key, AttributeValue value) noexcept;
  void add_event(std::string_view name) noexcept;
  void add_event(
    std::string_view name,
    std::initializer_list<std::pair<std::string_view, AttributeValue>> attrs) noexcept;
  void set_status(StatusCode code, std::string_view message = {}) noexcept;

private:
  explicit Span(detail::SpanRecord * record) noexcept
  : record_(record) {}

  detail::SpanRecord * record_{nullptr};

  friend class SpanGuard;
  friend Span current_span() noexcept;
};

// ---------------------------------------------------------------------------
// RAII span guard (what ROBOTOPS_TRACE expands to; also usable directly)
// ---------------------------------------------------------------------------

/// Options for opening a span.
struct SpanOptions
{
  SpanKind kind{SpanKind::Internal};
  /// Explicit parent; null => use the thread-local current context.
  const SpanContext * parent{nullptr};
  std::initializer_list<std::pair<std::string_view, AttributeValue>> attributes{};
};

/// Opens a span on construction and closes it (pop thread-local, enqueue to the
/// processor) on destruction. Move-only. Never throws.
class SpanGuard
{
public:
  explicit SpanGuard(std::string_view name, SpanOptions opts = {}) noexcept;
  ~SpanGuard();

  SpanGuard(SpanGuard &&) noexcept;
  SpanGuard & operator=(SpanGuard &&) noexcept;
  SpanGuard(const SpanGuard &) = delete;
  SpanGuard & operator=(const SpanGuard &) = delete;

  /// Handle to set attributes/status/events on the span this guard opened.
  Span span() const noexcept;

private:
  void close() noexcept;

  detail::SpanRecord * record_{nullptr};
};

}  // namespace robotops

#endif  // ROBOTOPS_TRACE__SPAN_HPP_
