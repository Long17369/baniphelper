// 「先封后断」的编排（阶段二 S2.8）。
//
// 本文件只有二十行实现，但它承担的是整个封禁链路里唯一一条**时序**不变量。
// 为什么值得单独成一个模块、而不是写在界面里：界面上「封禁」这个动作会有
// 好几处入口（连接列表、规则编辑器、托盘菜单、将来的 WebUI），
// 每一处各写一遍顺序，就一定有一处会写反，而写反的症状是「偶尔闪一下就恢复」，
// 极难复现。放在这里之后，顺序只有一处定义，而且可测。

#include "core/ban_action.h"

#include "core/error.h"

namespace baniphelper::core {

Result<KillReport> applyBanAndKill(IFilterEngine& engine,
                                   IKiller& killer,
                                   const RuleSpec& rule,
                                   const QList<ConnectionKey>& established) {
  // 第一步：下发规则。**这一步失败就到此为止** —— 后面一行都不许执行。
  auto applied = engine.applyRule(rule);
  if (!applied) {
    return Result<KillReport>::fail(applied.error());
  }

  // 第二步：断开已有连接。
  KillReport report;
  report.requested = static_cast<int>(established.size());
  if (established.isEmpty()) {
    // 没有东西可断，就不去碰断连实现。
    return Result<KillReport>::ok(report);
  }

  auto killed = killer.killMany(established);
  if (!killed) {
    // 断连实现整体失败（例如引擎没打开）。规则已经生效了，这不算失败，
    // 但**必须**把情况说清楚：规则已下发、连接没断，用户需要知道该重试断连。
    return Result<KillReport>::fail(
        makeError(killed.error().code,
                  QStringLiteral("规则已下发并生效，但断开已有连接失败：%1。"
                                 "连接仍在，请重试断开（规则不需要重下发）")
                      .arg(killed.error().message),
                  killed.error().nativeCode,
                  killed.error().nativeSource));
  }

  return killed;
}

}  // namespace baniphelper::core
