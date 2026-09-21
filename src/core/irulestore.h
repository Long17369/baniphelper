#pragma once

#include <QDateTime>
#include <QList>
#include <QString>

#include <functional>

#include "core/result.h"
#include "core/rule.h"
#include "core/types.h"

namespace baniphelper::core {

/// 规则变更的回调，参数是被改动的规则标识。批量变更时分多次调用。
/// 可能在非界面线程上被调用，消费方自行切线程。
using RuleChangeSink = std::function<void(const RuleId& id)>;

/// 规则持久化。
///
/// 契约要点：
///
/// - **入库前必须校验**：`conditions` 为空、`domain` 或 `mode` 不认识、取值无法解析，
///   一律拒绝并给出明确错误。**绝不宽松解释**，宁可这条规则不生效也不能按错语义封禁；
/// - 一次 `upsert` 是一次原子操作，不允许出现「写了一半的规则」；
/// - `remove` 对不存在的标识视为成功（幂等），否则撤销流程会被重复调用搅乱；
/// - 本接口只管持久化，**不下发过滤器**。下发与回滚由上层编排，顺序由上层负责。
class IRuleStore {
 public:
  IRuleStore() = default;
  virtual ~IRuleStore() = default;

  IRuleStore(const IRuleStore&) = delete;
  IRuleStore& operator=(const IRuleStore&) = delete;
  IRuleStore(IRuleStore&&) = delete;
  IRuleStore& operator=(IRuleStore&&) = delete;

  [[nodiscard]] virtual Result<QList<Rule>> list() const = 0;

  [[nodiscard]] virtual Result<Rule> find(const RuleId& id) const = 0;

  /// 新增或整体替换。
  [[nodiscard]] virtual Result<void> upsert(const Rule& rule) = 0;

  /// 删除。对不存在的标识返回成功。
  [[nodiscard]] virtual Result<void> remove(const RuleId& id) = 0;

  /// 清空全部规则。属于危险操作，调用方必须先走二次确认与审计，
  /// 并且要先确认当前不处于白名单模式，否则会留下一个「默认阻断且无放行项」的死局。
  [[nodiscard]] virtual Result<void> clear() = 0;

  /// 已到期但仍标记为启用的规则。由调用方负责撤销其过滤器并更新启用状态，
  /// 存储层不自行改动数据。
  [[nodiscard]] virtual Result<QList<Rule>> expired(const QDateTime& now) const = 0;

  [[nodiscard]] virtual Result<SubscriptionId> subscribe(RuleChangeSink sink) = 0;

  [[nodiscard]] virtual Result<void> unsubscribe(SubscriptionId id) = 0;
};

}  // namespace baniphelper::core
