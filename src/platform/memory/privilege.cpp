#include "platform/memory/privilege.h"

namespace baniphelper::core {

MemoryPrivilege::MemoryPrivilege(bool elevated) : elevated_(elevated) {}

Result<bool> MemoryPrivilege::isElevated() const {
  return Result<bool>::ok(elevated_);
}

Result<void> MemoryPrivilege::relaunchElevated() {
  if (elevated_) {
    // 已经提权却仍然去拉起新实例，会留下两个进程各自下发一套过滤器。
    return Result<void>::fail(makeError(
        ErrorCode::AlreadyExists, QStringLiteral("当前进程已经是提权状态，不需要再拉起一个自己")));
  }

  ++relaunchCount_;
  // 内存实现把「提权重启」模拟为状态切换：真实实现会拉起新进程并让本进程退出。
  elevated_ = true;
  return Result<void>::ok();
}

QString MemoryPrivilege::elevationRequirementText() const {
  return QStringLiteral("未提权时无法下发封禁规则，也无法中断已建立的连接，只能查看。");
}

void MemoryPrivilege::setElevated(bool elevated) {
  elevated_ = elevated;
}

int MemoryPrivilege::relaunchCount() const {
  return relaunchCount_;
}

}  // namespace baniphelper::core
