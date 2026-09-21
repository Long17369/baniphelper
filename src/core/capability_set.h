#pragma once

#include <QList>
#include <QString>

#include <cstdint>

#include "core/capabilities.h"

namespace baniphelper::core {

/// 一组能力位。
///
/// 用途是把「这个后端会什么」变成一个可传递、可比较的值，而不是散落在若干处布尔判断里。
/// 用 32 位掩码表示：能力位一共十余个，掩码足够，且复制与比较都廉价。
class CapabilitySet {
 public:
  CapabilitySet() = default;

  [[nodiscard]] static CapabilitySet fromList(const QList<Capability>& capabilities);

  void add(Capability capability) noexcept;
  void remove(Capability capability) noexcept;

  [[nodiscard]] bool contains(Capability capability) const noexcept;

  /// 已声明支持的能力。
  ///
  /// 顺序恒定为 `kAllCapabilities` 的次序，**与添加顺序无关**：
  /// 日志、界面与测试都依赖这个稳定性，否则同样的集合会有多种表示。
  [[nodiscard]] QList<Capability> list() const;

  [[nodiscard]] int size() const noexcept;

  [[nodiscard]] bool isEmpty() const noexcept {
    return mask_ == 0;
  }

  /// 供日志使用的紧凑形式，例如 `FilterIPv4|KillTcpV4`。空集合返回 `(无)`。
  [[nodiscard]] QString toLogString() const;

 private:
  static std::uint32_t bitOf(Capability capability) noexcept;

  std::uint32_t mask_ = 0;
};

[[nodiscard]] bool operator==(const CapabilitySet& lhs, const CapabilitySet& rhs);
[[nodiscard]] bool operator!=(const CapabilitySet& lhs, const CapabilitySet& rhs);

}  // namespace baniphelper::core
