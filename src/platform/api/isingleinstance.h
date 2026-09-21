#pragma once

#include "core/result.h"

namespace baniphelper::core {

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
  [[nodiscard]] virtual Result<void> signalExisting() = 0;

  /// 释放所有权。必须在正常退出路径上调用；不抛异常。
  virtual void release() noexcept = 0;
};

}  // namespace baniphelper::core
