// 规则的生命周期编排（阶段二 S2.9）。
//
// 顺序与回滚的规矩写在头文件里，这里逐条落实。实现刻意短而直白：
// 这个模块的价值全在「谁先谁后」上，多一层抽象只会让它更难核对。

#include "core/rule_runtime.h"

#include "core/error.h"
#include "core/rule_model.h"

namespace baniphelper::core {
namespace {

RuleSpec specFrom(const Rule& rule, std::int32_t priority) {
  RuleSpec spec;
  spec.id = rule.id;
  spec.action = rule.action;
  spec.conditions = rule.conditions;
  spec.priority = priority;
  spec.expireAt = rule.expireAt;
  return spec;
}

/// 把「库里那条旧版本」重新装回引擎。
///
/// `hasPrevious` 为假（本来就没有这条规则）时等价于撤销 —— 那正是「原样」。
/// ⚠️ **撤销时必须用传进来的 `id`，不能读 `previous.id`**：没有旧版本时
/// `previous` 是默认构造的空壳，它的标识是空串，撤销空串等于什么都没做，
/// 而恰好留下「库里没写、过滤器却在」这种它本来要防的状态。
/// 这里刻意不复用 `save` 的代码路径：`save` 会写库，而回滚**绝不能再写一次**。
Result<void> putBack(IFilterEngine& engine,
                     const RuleId& id,
                     const Rule& previous,
                     bool hasPrevious) {
  if (!hasPrevious || !previous.enabled) {
    return engine.revokeRule(id);
  }
  const Result<std::int32_t> priority = computePriority(previous);
  if (!priority) {
    return Result<void>::fail(priority.error());
  }
  return engine.applyRule(specFrom(previous, priority.value()));
}

/// 回滚失败的说法。**必须与「原操作失败」分开讲**：两件事都失败时，
/// 用户需要知道现在的状态是存疑的，而不是只看到第一层原因。
Error rollbackFailed(const QString& action,
                     const RuleId& id,
                     const Error& original,
                     const Error& rollback) {
  return makeError(rollback.code,
                   QStringLiteral("%1规则「%2」失败（%3），而且把它恢复原样也失败了（%4）："
                                  "引擎里的过滤器与数据库现在可能不一致，重启后会以数据库为准")
                       .arg(action, id, original.message, rollback.message),
                   rollback.nativeCode,
                   rollback.nativeSource);
}

}  // namespace

RuleRuntime::RuleRuntime(IRuleStore& store, IFilterEngine& engine)
    : store_(store), engine_(engine) {}

Result<void> RuleRuntime::save(const Rule& rule) {
  const Result<void> valid = validateRule(rule);
  if (!valid) {
    return Result<void>::fail(
        makeError(valid.error().code,
                  QStringLiteral("规则「%1」没有通过校验：%2").arg(rule.id, valid.error().message),
                  valid.error().nativeCode,
                  valid.error().nativeSource));
  }

  const Result<std::int32_t> priority = computePriority(rule);
  if (!priority) {
    return Result<void>::fail(priority.error());
  }

  // 更新场景要先拿到旧版本 —— 落库失败时要把引擎恢复成它。
  Rule previous;
  bool hasPrevious = false;
  const Result<Rule> found = store_.find(rule.id);
  if (found) {
    previous = found.value();
    hasPrevious = true;
  } else if (found.error().code != ErrorCode::NotFound) {
    return Result<void>::fail(found.error());
  }

  // 第一步：让引擎先动。这一步失败就到此为止，库里一个字节都没改。
  const RuleSpec spec = specFrom(rule, priority.value());
  Result<void> engineStep = rule.enabled ? engine_.applyRule(spec) : engine_.revokeRule(rule.id);
  if (!engineStep) {
    return Result<void>::fail(
        makeError(engineStep.error().code,
                  QStringLiteral("%1规则「%2」失败，数据库未改动：%3")
                      .arg(rule.enabled ? QStringLiteral("下发") : QStringLiteral("撤销"),
                           rule.id,
                           engineStep.error().message),
                  engineStep.error().nativeCode,
                  engineStep.error().nativeSource));
  }

  // 第二步：落库。失败就把引擎恢复成原样 —— 否则会留下一条「库里没写、过滤器却在」
  // 的规则，重启之后它凭空消失，而用户以为已经保存了。
  const Result<void> written = store_.upsert(rule);
  if (!written) {
    const Result<void> rollback = putBack(engine_, rule.id, previous, hasPrevious);
    if (!rollback) {
      return Result<void>::fail(
          rollbackFailed(QStringLiteral("写入"), rule.id, written.error(), rollback.error()));
    }
    return written;
  }

  return Result<void>::ok();
}

Result<void> RuleRuntime::remove(const RuleId& id) {
  Rule previous;
  bool hasPrevious = false;
  const Result<Rule> found = store_.find(id);
  if (found) {
    previous = found.value();
    hasPrevious = true;
  } else if (found.error().code != ErrorCode::NotFound) {
    return Result<void>::fail(found.error());
  }

  // 先撤过滤器：删库成功而撤销失败的话，库里已经没有这条规则、过滤器却还在，
  // 下次启动的清理（按 provider 清残留）才会把它收掉 —— 中间这一段时间是「幽灵规则」。
  const Result<void> revoked = engine_.revokeRule(id);
  if (!revoked) {
    return Result<void>::fail(
        makeError(revoked.error().code,
                  QStringLiteral("撤销规则「%1」的过滤器失败，数据库未改动：%2")
                      .arg(id, revoked.error().message),
                  revoked.error().nativeCode,
                  revoked.error().nativeSource));
  }

  const Result<void> removed = store_.remove(id);
  if (!removed) {
    const Result<void> rollback = putBack(engine_, id, previous, hasPrevious);
    if (!rollback) {
      return Result<void>::fail(
          rollbackFailed(QStringLiteral("删除"), id, removed.error(), rollback.error()));
    }
    return removed;
  }

  return Result<void>::ok();
}

Result<RestoreReport> RuleRuntime::restore() {
  const Result<QList<Rule>> rules = store_.list();
  if (!rules) {
    return Result<RestoreReport>::fail(rules.error());
  }

  const QDateTime now = QDateTime::currentDateTimeUtc();
  RestoreReport report;

  for (const Rule& rule : rules.value()) {
    if (!rule.enabled) {
      ++report.disabled;
      continue;
    }
    // 已过期的规则**不下发**：它本来就该失效，下发一次再撤掉只是白白在内核里
    // 过一手，还会在日志里制造一次「生效了」的假象。
    if (rule.expireAt.isValid() && rule.expireAt.toUTC() <= now) {
      ++report.alreadyExpired;
      continue;
    }

    const Result<std::int32_t> priority = computePriority(rule);
    if (!priority) {
      report.failures.append(RuleProblem{rule.id, priority.error().message});
      continue;
    }
    auto applied = engine_.applyRule(specFrom(rule, priority.value()));
    if (!applied) {
      report.failures.append(RuleProblem{rule.id, applied.error().message});
      continue;
    }
    ++report.applied;
  }

  return Result<RestoreReport>::ok(report);
}

Result<ExpireReport> RuleRuntime::expireDue(const QDateTime& now) {
  const Result<QList<Rule>> due = store_.expired(now);
  if (!due) {
    return Result<ExpireReport>::fail(due.error());
  }

  ExpireReport report;
  for (const Rule& rule : due.value()) {
    const Result<void> revoked = engine_.revokeRule(rule.id);
    if (!revoked) {
      report.failures.append(RuleProblem{
          rule.id,
          QStringLiteral("撤销过滤器失败，仍按启用处理：%1").arg(revoked.error().message)});
      continue;
    }

    Rule updated = rule;
    updated.enabled = false;
    const Result<void> written = store_.upsert(updated);
    if (!written) {
      // 过滤器已经撤了、库里还写着启用。**不改回引擎**：规则确实已经到期，
      // 让它重新生效比状态不一致更糟。下一次巡检（或下次启动的恢复）会收敛。
      report.failures.append(RuleProblem{
          rule.id,
          QStringLiteral("过滤器已撤下，但停用状态没写进数据库：%1").arg(written.error().message)});
      continue;
    }

    report.disabled.append(rule.id);
  }

  return Result<ExpireReport>::ok(report);
}

}  // namespace baniphelper::core
