#pragma once

#include <QString>

#include "core/result.h"

namespace baniphelper::core {

/// 提权检测与申请。
///
/// 判据是「**能不能做成这件事**」，不是「是不是管理员」：
/// 各平台的权限模型不同，把平台概念写进判据会让核心层跟着歪。
///
/// 契约要点：
///
/// - `isElevated` 为假时，封禁与断连必定失败，因此启动时必须尽早查询并给出引导，
///   不要等到用户点了封禁才报「拒绝访问」；
/// - `relaunchElevated` 成功后，**本进程随后应当退出**：留下两个实例会重复下发过滤器；
/// - 已经是提权状态时调用 `relaunchElevated` 返回 `AlreadyExists`，
///   不要真的再拉起一个自己。
class IPrivilege {
 public:
  IPrivilege() = default;
  virtual ~IPrivilege() = default;

  IPrivilege(const IPrivilege&) = delete;
  IPrivilege& operator=(const IPrivilege&) = delete;
  IPrivilege(IPrivilege&&) = delete;
  IPrivilege& operator=(IPrivilege&&) = delete;

  [[nodiscard]] virtual Result<bool> isElevated() const = 0;

  /// 以提权方式重新启动自身。
  [[nodiscard]] virtual Result<void> relaunchElevated() = 0;

  /// 权限不足时给用户看的说明：**缺了权限会失去哪些功能**，而不是一句「请以管理员身份运行」。
  /// 界面与 WebUI 都用它，因此不允许返回空字符串。
  [[nodiscard]] virtual QString elevationRequirementText() const = 0;
};

}  // namespace baniphelper::core
