#pragma once

#include <QList>

#include "core/result.h"
#include "core/types.h"
#include "platform/api/platform_types.h"

namespace baniphelper::core {

/// 连接中断。
///
/// **最重要的一条时序不变量：先封后断。**
/// 必须先让封禁规则生效，再断已有连接。顺序反了的话，对端会在被断开的瞬间重连成功，
/// 用户看到的现象是「点了封禁但连接立刻又回来了」。这条不变量由上层编排保证，
/// 本接口只提供「断」这个动作，不负责判定时机。
///
/// 契约要点：
///
/// - **中断后连接确实消失**，不是「标记为待关闭」；
/// - **不支持必须明确报错**，不允许静默返回成功。IPv6 上这就是常态：
///   本平台没有可用的系统接口，能力位应当为假，`kill` 返回 `NotSupported`；
/// - 批量断连**逐条上报失败原因**。不允许聚合成一句「部分失败」，
///   那会让用户无从判断哪条还活着。
class IKiller {
 public:
  IKiller() = default;
  virtual ~IKiller() = default;

  IKiller(const IKiller&) = delete;
  IKiller& operator=(const IKiller&) = delete;
  IKiller(IKiller&&) = delete;
  IKiller& operator=(IKiller&&) = delete;

  /// 该连接当前能否被中断。
  ///
  /// 用于界面在操作前就把按钮置灰，而不是让用户点一下才知道不行。
  /// 返回成功且值为 false 表示「能判断，但这条连接断不了」（例如 UDP 没有连接语义）；
  /// 返回失败表示「连判断都做不到」。
  [[nodiscard]] virtual Result<bool> canKill(const ConnectionKey& connection) const = 0;

  [[nodiscard]] virtual Result<void> kill(const ConnectionKey& connection) = 0;

  [[nodiscard]] virtual Result<KillReport> killMany(const QList<ConnectionKey>& connections) = 0;
};

}  // namespace baniphelper::core
