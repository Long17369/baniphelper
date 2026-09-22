#pragma once

#include <QMutex>
#include <QString>

#include <thread>

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
///
/// 接收端（`listenForActivation`）是同一个管道名字下的服务端，由一个专职线程守着。
/// 之所以要线程：管道的等待是**阻塞**的，而它要一直等下去；放在主线程就等于把界面冻住。
class WinSingleInstance final : public ISingleInstance {
 public:
  explicit WinSingleInstance(QString name);
  ~WinSingleInstance() override;

  WinSingleInstance(const WinSingleInstance&) = delete;
  WinSingleInstance& operator=(const WinSingleInstance&) = delete;

  [[nodiscard]] Result<bool> acquire() override;
  [[nodiscard]] Result<void> signalExisting() override;
  [[nodiscard]] Result<void> listenForActivation(ActivationHandler handler) override;
  void stopListening() noexcept override;
  void release() noexcept override;

 private:
  [[nodiscard]] QString mutexName() const;
  [[nodiscard]] QString pipeName() const;

  /// 监听线程主体：等人连、收报文、投递，然后换一个新管道实例重来。
  void listenLoop();

  /// 新建一个管道实例，供客户端连接。失败返回 `nullptr` 并已记过日志。
  ///
  /// 之所以把「建实例」与「等连接」拆开：接口承诺 `listenForActivation` 返回成功时
  /// 通道**已经可用**，所以第一个实例必须在调用者的线程上同步建好，
  /// 否则紧接着的二次启动会撞在「线程还没来得及建管道」的窗口上，报一个假的
  /// 「既有实例没有在监听」。
  [[nodiscard]] void* createPipeInstance() const;

  /// 把处理函数投递到主线程执行。
  static void dispatchActivation(const ActivationHandler& handler);

  QString name_;

  /// 内核对象句柄。这里存成 `void*` 而不是 `HANDLE`，是为了不让头文件
  /// 去包含 windows.h —— 那会把它的一堆宏（min/max 之类）带给包含本头的每一处。
  /// 实现内部会立刻转回 `HANDLE`。
  void* mutex_ = nullptr;

  bool held_ = false;

  /// 停止信号（手动重置事件，置位后一直有效）。
  ///
  /// 监听线程的每一次等待都把它算进去，因此「停止」这个动作永远有确定的出口，
  /// 不需要靠关闭句柄去打断一个正在阻塞的调用 ——
  /// 那种做法依赖未文档化的行为，也正是这类实现最常见的挂死点。
  void* stopEvent_ = nullptr;

  /// 重叠 I/O 的完成事件（自动重置）。与停止信号一起进 `WaitForMultipleObjects`。
  void* ioEvent_ = nullptr;

  /// 已经建好、等着交给监听线程的第一个管道实例。
  /// 交接只发生一次，之后线程自己在循环里建。
  void* pendingPipe_ = nullptr;

  std::thread listener_;
  bool listening_ = false;

  /// 处理函数可能被重复注册（后注册的替换先前的），因此读写都要加锁。
  QMutex handlerMutex_;
  ActivationHandler handler_;
};

}  // namespace baniphelper::core
