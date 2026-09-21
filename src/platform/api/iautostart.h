#pragma once

#include <QString>

#include "core/result.h"

namespace baniphelper::core {

/// 开机自启的注册与撤销。
///
/// **关键约束：必须是「带提权启动」的自启。**
/// Windows 上注册表 `Run` 键无法携带 UAC 提权，用它实现出来的结果是
/// 「开机确实启动了，但没有权限，等于没启动」——而且这种失败是静默的。
/// 因此实现必须走计划任务并勾选「使用最高权限运行」。
///
/// 契约要点：
///
/// - `isEnabled` 必须在**外部状态被改动后依然准确**（例如用户手动删掉了计划任务），
///   不能只读一份自己维护的标志位；
/// - `enable` 幂等：已启用时重复调用返回成功；
/// - 撤销自启后不得留下空的计划任务或残留项。
class IAutoStart {
 public:
  IAutoStart() = default;
  virtual ~IAutoStart() = default;

  IAutoStart(const IAutoStart&) = delete;
  IAutoStart& operator=(const IAutoStart&) = delete;
  IAutoStart(IAutoStart&&) = delete;
  IAutoStart& operator=(IAutoStart&&) = delete;

  [[nodiscard]] virtual Result<bool> isEnabled() const = 0;

  [[nodiscard]] virtual Result<void> enable() = 0;

  [[nodiscard]] virtual Result<void> disable() = 0;

  /// 实现方式的说明，界面直接显示，例如「已注册计划任务 BanIPHelper，以最高权限运行」。
  ///
  /// 这一栏存在的意义是**让用户能自己去核对与撤销**：自启是写进系统的持久改动，
  /// 只说「已开启」而不说改了什么，用户出问题时无从下手。不允许返回空字符串。
  [[nodiscard]] virtual QString mechanismDescription() const = 0;
};

}  // namespace baniphelper::core
