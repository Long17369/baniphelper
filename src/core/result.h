#pragma once

#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

#include "core/error.h"

namespace baniphelper::core {

/// `std::expected` 的 C++17 替代品。
///
/// 三条立约定死的规则：
///
/// 1. 失败必须携带 `Error`，且 `message` 不得为空。禁止只返回一个布尔值。
/// 2. 整条链路不使用异常作为控制流；`Result` 的移动与析构都不抛。
/// 3. 不支持的能力一律返回 `ErrorCode::NotSupported`，**禁止用零值、空值或假数据冒充成功**。
///
/// 用法：
///
/// ```cpp
/// Result<int> parse(const QString& text);
/// auto parsed = parse(input);
/// if (!parsed) {
///     return Result<void>::fail(parsed.error());
/// }
/// use(parsed.value());
/// ```
///
/// 注意：`value()` 与 `error()` 都带前置条件，调用前必须先判断。这是刻意的，
/// 目的是让每个调用点显式选择「失败时怎么办」，而不是静默取到一个默认值。
template <typename T>
class Result {
  static_assert(!std::is_reference_v<T>, "Result 不接受引用类型，请用指针或值类型");
  static_assert(!std::is_same_v<T, Error>, "Result<Error> 没有意义，请用 Result<void>");
  static_assert(std::is_copy_constructible_v<T> || std::is_move_constructible_v<T>,
                "Result 的值类型至少需要可移动");

 public:
  /// 隐式从值构造，方便直接 `return value;`。
  Result(T value)  // NOLINT(google-explicit-constructor)
      : storage_(std::in_place_index<0>, std::move(value)) {}

  /// 隐式从错误构造，方便直接 `return makeError(...);`。
  Result(Error error)  // NOLINT(google-explicit-constructor)
      : storage_(std::in_place_index<1>, std::move(error)) {}

  [[nodiscard]] static Result ok(T value) {
    return Result(std::move(value));
  }

  [[nodiscard]] static Result fail(Error error) {
    return Result(std::move(error));
  }

  [[nodiscard]] bool hasValue() const noexcept {
    return storage_.index() == 0;
  }

  explicit operator bool() const noexcept {
    return hasValue();
  }

  /// 前置条件：hasValue()。违反时 std::get 会抛异常，属于缺陷而非可恢复错误。
  [[nodiscard]] T& value() & {
    return std::get<0>(storage_);
  }
  [[nodiscard]] const T& value() const& {
    return std::get<0>(storage_);
  }
  [[nodiscard]] T&& value() && {
    return std::get<0>(std::move(storage_));
  }

  /// 前置条件：!hasValue()。
  [[nodiscard]] const Error& error() const& {
    return std::get<1>(storage_);
  }

  /// 失败时返回错误指针，成功时返回 nullptr。用于「顺手记一条日志」这类场合。
  [[nodiscard]] const Error* errorOrNull() const noexcept {
    return hasValue() ? nullptr : &std::get<1>(storage_);
  }

 private:
  std::variant<T, Error> storage_;
};

/// `Result<void>` 的特化。
///
/// 项目里所有「成功没有返回值」的接口都用它，不要退化成返回 `bool` 或 `Error`，
/// 那两种写法都会让调用点漏掉检查。
template <>
class Result<void> {
 public:
  /// 默认构造即成功。
  Result() = default;

  /// 隐式从错误构造，方便直接 `return makeError(...);`。
  Result(Error error)  // NOLINT(google-explicit-constructor)
      : error_(std::move(error)) {}

  [[nodiscard]] static Result ok() {
    return Result();
  }

  [[nodiscard]] static Result fail(Error error) {
    return Result(std::move(error));
  }

  [[nodiscard]] bool hasValue() const noexcept {
    return !error_.has_value();
  }

  explicit operator bool() const noexcept {
    return hasValue();
  }

  /// 前置条件：!hasValue()。
  [[nodiscard]] const Error& error() const& {
    return *error_;
  }

  [[nodiscard]] const Error* errorOrNull() const noexcept {
    return error_.has_value() ? &*error_ : nullptr;
  }

 private:
  std::optional<Error> error_;
};

}  // namespace baniphelper::core
