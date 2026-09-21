#pragma once

#include <functional>

#include "core/result.h"
#include "core/types.h"
#include "platform/api/platform_types.h"

namespace baniphelper::core {

/// 会话事件回调。在平台层自己的线程上同步调用，回调内不得阻塞。
using SessionEventSink = std::function<void(const SessionEvent&)>;

/// 系统会话事件订阅。
///
/// 用途是让采样与统计「跟着界面走」：锁屏或挂起时降频到接近零，恢复时补齐。
/// 因此本能力缺失不会影响正确性，只会让占用偏高——**但要在能力位上如实声明**，
/// 好让界面上的「空闲占用」说明不至于变成一句无条件的承诺。
///
/// 契约要点：订阅与退订成对；进程退出时必须退订，否则平台侧订阅会残留。
class ISessionMonitor {
 public:
  ISessionMonitor() = default;
  virtual ~ISessionMonitor() = default;

  ISessionMonitor(const ISessionMonitor&) = delete;
  ISessionMonitor& operator=(const ISessionMonitor&) = delete;
  ISessionMonitor(ISessionMonitor&&) = delete;
  ISessionMonitor& operator=(ISessionMonitor&&) = delete;

  [[nodiscard]] virtual Result<SubscriptionId> subscribe(SessionEventSink sink) = 0;

  /// 对未知句柄调用返回成功。
  [[nodiscard]] virtual Result<void> unsubscribe(SubscriptionId id) = 0;
};

}  // namespace baniphelper::core
