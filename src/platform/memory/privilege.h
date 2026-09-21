#pragma once

#include <QString>

#include "core/result.h"
#include "platform/api/iprivilege.h"

namespace baniphelper::core {

/// 提权状态的内存实现。
///
/// 提权状态可由测试直接设置，因此它能覆盖「已提权」与「未提权」两条分支；
/// 真实后端做不到这一点 —— 测试进程的权限是外部给定的，测试里无法既当已提权
/// 又当未提权。这正是「同一套契约测试」需要内存实现的原因。
class MemoryPrivilege final : public IPrivilege {
 public:
  explicit MemoryPrivilege(bool elevated = false);

  [[nodiscard]] Result<bool> isElevated() const override;
  [[nodiscard]] Result<void> relaunchElevated() override;
  [[nodiscard]] QString elevationRequirementText() const override;

  /// 直接设置提权状态，供测试造场景。
  void setElevated(bool elevated);

  /// `relaunchElevated` 被调用过几次。测试用它确认没有重复拉起自己。
  [[nodiscard]] int relaunchCount() const;

 private:
  bool elevated_ = false;
  int relaunchCount_ = 0;
};

}  // namespace baniphelper::core
