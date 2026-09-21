#include "platform/memory/filter_engine.h"

namespace baniphelper::core {
namespace {

Error notImplementedYet(const QString& operation) {
  return unsupportedError(operation, QStringLiteral("过滤器引擎的下发能力尚未实现，阶段二 S2.4"));
}

}  // namespace

MemoryFilterEngine::MemoryFilterEngine() = default;

bool MemoryFilterEngine::isOpen() const {
  return open_;
}

Result<void> MemoryFilterEngine::open() {
  open_ = true;
  return Result<void>::ok();
}

Result<void> MemoryFilterEngine::close() {
  open_ = false;
  return Result<void>::ok();
}

Result<CleanupReport> MemoryFilterEngine::cleanupOrphans() {
  if (!open_) {
    // 与真实后端一致：引擎没打开就清理，必须报错而不是静默成功。
    return Result<CleanupReport>::fail(makeError(
        ErrorCode::InvalidArgument, QStringLiteral("过滤器引擎尚未打开，无法清理过滤器")));
  }

  CleanupReport report;
  report.removedOwn = filterCount_;
  report.skippedForeign = 0;
  filterCount_ = 0;

  return Result<CleanupReport>::ok(report);
}

Result<void> MemoryFilterEngine::revokeAll() {
  const Result<CleanupReport> report = cleanupOrphans();
  if (!report) {
    return Result<void>::fail(report.error());
  }
  return Result<void>::ok();
}

Result<void> MemoryFilterEngine::applyRule(const RuleSpec& rule) {
  Q_UNUSED(rule);
  return Result<void>::fail(notImplementedYet(QStringLiteral("下发封禁规则")));
}

Result<void> MemoryFilterEngine::revokeRule(const RuleId& id) {
  Q_UNUSED(id);
  return Result<void>::fail(notImplementedYet(QStringLiteral("撤销指定规则")));
}

Result<QList<AppliedRuleSummary>> MemoryFilterEngine::appliedRules() const {
  return Result<QList<AppliedRuleSummary>>::fail(
      notImplementedYet(QStringLiteral("查询已生效规则")));
}

Result<void> MemoryFilterEngine::applyDefaultBlock(std::int32_t priority) {
  Q_UNUSED(priority);
  return Result<void>::fail(notImplementedYet(QStringLiteral("下发默认阻断")));
}

Result<bool> MemoryFilterEngine::hasDefaultBlock() const {
  return Result<bool>::fail(notImplementedYet(QStringLiteral("查询默认阻断状态")));
}

Result<void> MemoryFilterEngine::revokeDefaultBlock() {
  return Result<void>::fail(notImplementedYet(QStringLiteral("撤销默认阻断")));
}

void MemoryFilterEngine::seedOrphans(int count) {
  if (count > 0) {
    filterCount_ += count;
  }
}

int MemoryFilterEngine::storedFilterCount() const {
  return filterCount_;
}

}  // namespace baniphelper::core
