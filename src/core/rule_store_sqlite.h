#pragma once

#include <QHash>
#include <QString>

#include "core/database.h"
#include "core/irulestore.h"
#include "core/result.h"
#include "core/rule.h"
#include "core/types.h"

namespace baniphelper::core {

/// 规则持久化的 SQLite 实现。
///
/// 表结构见 `database.cpp` 的迁移 2，几条取舍记在那里；这里只补三条使用层面的约定：
///
/// - **只校验、不改写**。入库前跑一遍 `validateRule`，但**写进去的是调用方给的原文**。
///   原因与 S2.1 的路径那件事同源：归一化是**比较与求值**时才需要的事，
///   存储层改写用户输入会让界面上出现「我没写过的形式」。
///   取值拼法（`2001:DB8::1` 与 `2001:db8::1`）的收敛由求值时的
///   `normalizeCondition` 负责，两边各管一段；
/// - **`created_at` 归存储层所有**。新增时用调用方给的（无效则取当下），
///   更新时**忽略调用方给的值、保留库里那个**。它是「同具体度则先建的优先」的依据，
///   一旦被改写，规则之间的先后就会在每次编辑后漂移，结果不再可复现；
/// - **读出来也校验**。库里可能留着被手改过的行、或旧版本写下的取值，
///   遇到不认识的域或方式必须拒绝并报错，**不允许宽松解释**
///   —— 按错的语义封禁比这条规则不生效危险得多。
class SqliteRuleStore final : public IRuleStore {
 public:
  /// 不做连接检查，只记住引用：`Database` 的可用性由调用方负责
  /// （每次操作都会经由它，未打开时自然失败并给出原因）。
  explicit SqliteRuleStore(Database& database);
  ~SqliteRuleStore() override;

  SqliteRuleStore(const SqliteRuleStore&) = delete;
  SqliteRuleStore& operator=(const SqliteRuleStore&) = delete;

  [[nodiscard]] Result<QList<Rule>> list() const override;
  [[nodiscard]] Result<Rule> find(const RuleId& id) const override;
  [[nodiscard]] Result<void> upsert(const Rule& rule) override;
  [[nodiscard]] Result<void> remove(const RuleId& id) override;
  [[nodiscard]] Result<void> clear() override;
  [[nodiscard]] Result<QList<Rule>> expired(const QDateTime& now) const override;

  [[nodiscard]] Result<SubscriptionId> subscribe(RuleChangeSink sink) override;
  [[nodiscard]] Result<void> unsubscribe(SubscriptionId id) override;

 private:
  /// 变更发生后通知订阅者。**只报告成功的变更**：没写进去的东西不该让界面去刷新。
  void notify(const RuleId& id) const;

  Database& database_;

  mutable QHash<SubscriptionId, RuleChangeSink> sinks_;
  mutable SubscriptionId nextSubscription_ = 1;
};

}  // namespace baniphelper::core
