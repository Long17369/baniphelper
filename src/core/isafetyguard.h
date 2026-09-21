#pragma once

#include <QList>
#include <QString>

#include <functional>

#include "core/result.h"
#include "core/rule.h"
#include "core/safety.h"
#include "core/types.h"

namespace baniphelper::core {

/// 回滚状态变化的回调：倒计时开始、剩余秒数变化、超时回滚完成都会触发。
/// 可能在非界面线程上被调用，消费方自行切线程。
using RollbackEventSink = std::function<void(const RollbackState&)>;

/// 安全阀：白名单模式、必备放行、超时自动回滚、自我保护。
///
/// 本接口是本项目**唯一以「不出事故」为交付目标**的接口，因此契约比别处更硬：
///
/// - **下发顺序是不变量**：切到白名单模式时，必须**先下发全部放行项，最后才下发默认阻断**。
///   顺序反了会在中间态把网络切断，用户可能连回滚都点不到；
/// - **超时自动回滚不可关闭**。倒计时到点未确认就还原到上一个可用配置，
///   这是防「把自己彻底锁死」的唯一可靠手段，不提供配置项去关它；
/// - **回滚只做删除不做新增**，缩小失败面：回滚本身再失败就没有下一层兜底了；
/// - **自我保护优先于任何规则**：任何规则组合都不得切断自身进程与更新通道，
///   由核心层在投递前调用 `protectsSelf` 拦截，而不是指望用户不写错。
class ISafetyGuard {
 public:
  ISafetyGuard() = default;
  virtual ~ISafetyGuard() = default;

  ISafetyGuard(const ISafetyGuard&) = delete;
  ISafetyGuard& operator=(const ISafetyGuard&) = delete;
  ISafetyGuard(ISafetyGuard&&) = delete;
  ISafetyGuard& operator=(ISafetyGuard&&) = delete;

  /// 切换白名单模式。开启时启动倒计时；关闭时立即撤销默认阻断并结束倒计时。
  [[nodiscard]] virtual Result<void> setWhitelistMode(bool enabled) = 0;

  [[nodiscard]] virtual bool whitelistMode() const = 0;

  /// 当前生效的放行清单，含核心内置项与平台注入项。
  [[nodiscard]] virtual Result<QList<AllowlistEntry>> allowlist() const = 0;

  /// 开关某个可选的放行项。对 `removable == false` 的项调用必须返回
  /// `ErrorCode::NotPermitted`，**不允许静默忽略**：静默忽略会让用户以为关成功了。
  [[nodiscard]] virtual Result<void> setAllowlistEntryEnabled(const QString& id, bool enabled) = 0;

  /// 注入平台特有的必备放行项（例如 Windows 上的连通性探测）。
  ///
  /// 之所以用注入而不是让核心层判断平台：核心层不允许出现平台概念。
  /// 平台装配代码（`platform/<os>/`）把数据准备好后交给本方法，核心层只负责使用。
  virtual void setPlatformAllowlist(QList<AllowlistEntry> entries) = 0;

  [[nodiscard]] virtual RollbackState rollbackState() const = 0;

  /// 用户在倒计时内确认保留当前配置，倒计时随即结束。
  [[nodiscard]] virtual Result<void> confirmCurrentConfiguration() = 0;

  /// 立即回滚到上一个可用配置，不等倒计时。
  [[nodiscard]] virtual Result<void> rollbackNow() = 0;

  /// 判断某条规则是否会切断自身。为真时调用方必须拒绝下发并给出明确提示。
  [[nodiscard]] virtual bool protectsSelf(const RuleSpec& rule) const = 0;

  /// 自我保护项的放行清单，供审计界面展示「我为什么封不掉自己」。
  [[nodiscard]] virtual Result<QList<AllowlistEntry>> selfProtectionEntries() const = 0;

  [[nodiscard]] virtual Result<SubscriptionId> subscribe(RollbackEventSink sink) = 0;

  [[nodiscard]] virtual Result<void> unsubscribe(SubscriptionId id) = 0;
};

}  // namespace baniphelper::core
