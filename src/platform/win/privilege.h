#pragma once

#include <QString>

#include "core/result.h"
#include "platform/api/iprivilege.h"

namespace baniphelper::core {

/// 提权检测与申请的 Windows 实现。
///
/// 判据是**进程令牌的提权状态**（`TokenElevation`），不是「当前用户是否属于管理员组」。
/// 后者在 UAC 之下会给出错误答案：管理员组的用户在不提权运行时，令牌里没有管理员权限，
/// 依旧写不进过滤器表。用「是不是管理员」当判据，会出现「检查通过但下发失败」。
class WinPrivilege final : public IPrivilege {
 public:
  WinPrivilege() = default;

  WinPrivilege(const WinPrivilege&) = delete;
  WinPrivilege& operator=(const WinPrivilege&) = delete;

  [[nodiscard]] Result<bool> isElevated() const override;
  [[nodiscard]] Result<void> relaunchElevated() override;
  [[nodiscard]] QString elevationRequirementText() const override;
};

}  // namespace baniphelper::core
