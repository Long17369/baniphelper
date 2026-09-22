#pragma once

#include <functional>

#include "core/result.h"

namespace baniphelper::core {

/// 收到「有人要唤起本实例」时的处理函数。
///
/// 会被实现投递到**主线程**执行，调用方不必自己再做一次线程投递。
/// 之所以把这条写进契约：界面只能在主线程动，而这个回调天然来自别的线程或别的进程。
using ActivationHandler = std::function<void()>;

/// 单实例保证。
///
/// 必要性：重复启动会**重复下发过滤器**，撤销时又互相不知道对方下过什么，
/// 最终留下一堆没人认领的过滤器。
///
/// 契约要点：
///
/// - 「已有实例在跑」**不是错误**：`acquire` 返回成功且值为 false，
///   调用方据此走「唤起既有实例然后退出」的路径。把它当成错误会逼出一堆
///   无意义的分支判断；
/// - `signalExisting` 必须走**命名管道或本地 HTTP**。
///   Windows 的 UIPI 禁止普通权限进程给提权进程发窗口消息，
///   用窗口消息实现的唤错方案在提权场景下必然静默失效；
/// - `release` 在正常退出时调用；**异常退出不指望它**（崩溃与强杀不会走到），
///   因此所有权必须由系统在进程结束时自动回收，不能靠一个残留文件或一份注册表项来判断。
class ISingleInstance {
 public:
  ISingleInstance() = default;
  virtual ~ISingleInstance() = default;

  ISingleInstance(const ISingleInstance&) = delete;
  ISingleInstance& operator=(const ISingleInstance&) = delete;
  ISingleInstance(ISingleInstance&&) = delete;
  ISingleInstance& operator=(ISingleInstance&&) = delete;

  /// 尝试取得所有权。成功返回 true；已有实例在跑返回 false（不是失败）。
  [[nodiscard]] virtual Result<bool> acquire() = 0;

  /// 唤起既有实例的界面，然后本进程应当退出。
  ///
  /// 只有在**既有实例已经开了接收端**（`listenForActivation`）时才可能成功。
  /// 没有接收端时必须失败，不允许静默成功 —— 那会让本进程直接退出，
  /// 用户看到的却是新旧两个窗口都不见了。
  [[nodiscard]] virtual Result<void> signalExisting() = 0;

  /// 开始接收唤起请求。
  ///
  /// 契约要点：
  ///
  /// - 只建立通道，**不改变所有权**：先 `acquire` 再监听，顺序反了也不会互相干扰；
  /// - 重复调用是安全的，后注册的 handler 替换先前的；
  /// - handler 由实现在**主线程**调用（见 `ActivationHandler`）；
  /// - 传入空的 handler 必须当场以 `ErrorCode::InvalidArgument` 失败，
  ///   不能建出一个「收了请求但什么也不做」的黑洞；
  /// - 建立失败必须显式报错。若实现悄无声息地建不起来，二次启动的用户看到的是
  ///   「双击了但既有窗口没出来，第二个进程也退了」，且没有任何解释。
  [[nodiscard]] virtual Result<void> listenForActivation(ActivationHandler handler) = 0;

  /// 停止接收。可重复调用，也应在析构与退出路径上调用。
  ///
  /// 停止之后 `signalExisting` 必须失败（通道已不在），不允许继续报成功。
  virtual void stopListening() noexcept = 0;

  /// 释放所有权。必须在正常退出路径上调用；不抛异常。
  virtual void release() noexcept = 0;
};

}  // namespace baniphelper::core
