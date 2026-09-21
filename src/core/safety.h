#pragma once

#include <QDateTime>
#include <QList>
#include <QString>

#include <cstdint>

#include "core/rule.h"

namespace baniphelper::core {

/// 必备放行项。
///
/// 白名单模式是「默认阻断 + 例外放行」，放行清单漏一项的后果是断网或者自己失联，
/// 因此每一项都必须自带「为什么必须有它」的说明，界面要能直接展示。
struct AllowlistEntry {
  /// 稳定标识，例如 `local-loopback`。
  QString id;

  /// 中文名称，界面直接显示。
  QString title;

  /// 断开它的后果。这一栏不允许为空：没有后果说明的放行项，用户无从判断能否去掉。
  QString reason;

  /// 放行条件。与规则共用同一种匹配条件结构，走的也是同一条下发链路。
  QList<MatchCondition> conditions;

  /// 是否允许用户取消勾选。
  /// 回环、自身进程、界面监听端口这几项必须为 false，否则等于把「锁死自己」做成了功能。
  bool removable = false;
};

/// 白名单模式的倒计时状态。
///
/// 超时自动回滚是防「把自己彻底锁死」的唯一可靠手段，因此这里的字段**不可配置为关闭**。
struct RollbackState {
  /// 是否正处于倒计时中。
  bool active = false;

  /// 超时时刻。到点未确认即回滚。
  QDateTime deadline;

  /// 剩余秒数，供界面显示。
  int remainingSeconds = 0;
};

}  // namespace baniphelper::core
