#pragma once

#include <QList>
#include <QMap>
#include <QString>

#include "core/capabilities.h"
#include "core/capability_set.h"
#include "core/icapabilities.h"

namespace baniphelper::core {

/// 一项不可用的能力及其原因。
struct UnsupportedCapability {
  Capability capability = Capability::FilterIPv4;
  /// 中文说明，**保证非空**。
  QString reason;
};

/// 由后端声明的一份能力清单。
///
/// 这是 `ICapabilities` 的唯一实现，也是「能力协商」的落点：
/// 各平台后端在启动时构造一份它，声明自己会什么；核心层与界面层只通过
/// `ICapabilities` 问它，不自己猜。
///
/// 两条保证：
///
/// - **没有静默路径**：对任一能力位查询都会得到明确的是或否；
///   不可用时 `unsupportedReason` 一定是非空字符串（未登记具体原因时退回通用的后果说明）；
/// - **声明与实际必须一致**：本类只负责记录声明，做不到就不许写进来。
///   契约测试在 S8.1 会把声明与实际行为逐项比对，这里的记录是那份比对的输入。
class DeclaredCapabilities final : public ICapabilities {
 public:
  DeclaredCapabilities(QString backendName, CapabilitySet declared);

  [[nodiscard]] bool isSupported(Capability capability) const override;

  [[nodiscard]] QList<Capability> supported() const override;

  [[nodiscard]] QString unsupportedReason(Capability capability) const override;

  [[nodiscard]] QString backendName() const override;

  /// 登记某项能力不可用的具体原因，例如「本平台没有可用的系统接口，只能靠丢包卡死」。
  ///
  /// 不必逐项登记：没登记的会退回该能力通用的后果说明，
  /// 因此 `unsupportedReason` 在任何情况下都不会是空字符串。
  void setUnsupportedReason(Capability capability, QString reason);

  /// 该后端声明的能力集合。
  [[nodiscard]] CapabilitySet declared() const;

  /// 全部不可用的能力及其原因，供设置页一次性展示，避免逐项调用。
  [[nodiscard]] QList<UnsupportedCapability> unsupportedWithReasons() const;

 private:
  QString backendName_;
  CapabilitySet declared_;
  QMap<Capability, QString> unsupportedReasons_;
};

}  // namespace baniphelper::core
