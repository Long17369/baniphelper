#pragma once

#include <QString>

#include "core/result.h"
#include "platform/api/isingleinstance.h"

namespace baniphelper::core {

/// 单实例的 Windows 实现：命名内核互斥体 + 命名管道。
///
/// 用命名互斥体，而不是「进程名比对」或「留一个残留文件」：
/// 内核对象的生命周期随最后一个句柄关闭而结束，因此崩溃与强杀都不会留下一个假的
/// 「已有实例」状态。那正是这类实现最常见的坑 —— 程序非正常退出一次，
/// 之后永远启动不起来，且用户没有任何办法自己修复。
///
/// `signalExisting` 走命名管道客户端。**刻意不用窗口消息**：
/// 本程序是提权进程，UIPI 会拦掉普通权限进程发来的窗口消息，
/// 用窗口消息实现的唤起方案在提权场景下必然静默失效。
class WinSingleInstance final : public ISingleInstance {
 public:
  explicit WinSingleInstance(QString name);
  ~WinSingleInstance() override;

  WinSingleInstance(const WinSingleInstance&) = delete;
  WinSingleInstance& operator=(const WinSingleInstance&) = delete;

  [[nodiscard]] Result<bool> acquire() override;
  [[nodiscard]] Result<void> signalExisting() override;
  void release() noexcept override;

 private:
  [[nodiscard]] QString mutexName() const;
  [[nodiscard]] QString pipeName() const;

  QString name_;

  /// 内核对象句柄。这里存成 `void*` 而不是 `HANDLE`，是为了不让头文件
  /// 去包含 windows.h —— 那会把它的一堆宏（min/max 之类）带给包含本头的每一处。
  /// 实现内部会立刻转回 `HANDLE`。
  void* mutex_ = nullptr;

  bool held_ = false;
};

}  // namespace baniphelper::core
