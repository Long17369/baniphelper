#pragma once

#include <QString>

#include "core/result.h"
#include "platform/api/ifilterengine.h"

namespace baniphelper::core {

/// 过滤器引擎的内存实现。
///
/// 它模拟的核心事实只有一件：「引擎里现在存着多少条自有过滤器」。阶段一（S1.7）需要的
/// 正是这一件事 —— 打开、关闭、把自家过滤器清干净。
///
/// 规则下发与撤销属于阶段二，这里与真实后端一样明确返回「不支持」。
/// 两个后端在契约测试下的行为必须一致，否则契约测试就失去了对照意义。
class MemoryFilterEngine final : public IFilterEngine {
 public:
  MemoryFilterEngine();

  [[nodiscard]] Result<void> open() override;
  [[nodiscard]] Result<void> close() override;
  [[nodiscard]] bool isOpen() const override;

  [[nodiscard]] Result<void> applyRule(const RuleSpec& rule) override;
  [[nodiscard]] Result<void> revokeRule(const RuleId& id) override;
  [[nodiscard]] Result<void> revokeAll() override;
  [[nodiscard]] Result<QList<AppliedRuleSummary>> appliedRules() const override;

  [[nodiscard]] Result<void> applyDefaultBlock(std::int32_t priority) override;
  [[nodiscard]] Result<bool> hasDefaultBlock() const override;
  [[nodiscard]] Result<void> revokeDefaultBlock() override;

  [[nodiscard]] Result<CleanupReport> cleanupOrphans() override;

  /// 预置若干「上次运行残留」的过滤器。
  ///
  /// 真实后端在测试里造不出残留：那需要真的下发过滤器，而下发还没实现。
  /// 「清理确实把残留清掉了」这件事因此只能在内存后端上验证 ——
  /// 这正是契约测试需要一个可控对照物的原因。
  void seedOrphans(int count);

  /// 当前模拟现存的过滤器条数，供测试核对清理是否彻底。
  [[nodiscard]] int storedFilterCount() const;

 private:
  bool open_ = false;
  int filterCount_ = 0;
};

}  // namespace baniphelper::core
