#pragma once

#include <QString>

#include "core/result.h"
#include "platform/api/ifilterengine.h"

namespace baniphelper::core {

/// 过滤器引擎的 Windows 实现（WFP）。
///
/// 阶段一（S1.7）建立了骨架：打开引擎、注册自有 provider 与 sublayer、
/// 清理上次运行残留的自有过滤器、安全关闭。S2.4 补上规则的下发与撤销。
///
/// 归属判据是**自有的 provider 与 sublayer**：枚举时按 provider 限定，
/// 因此他方过滤器在结构上就不可能出现，也就不可能被误删。
/// 这比「枚举全部再逐个判断」更安全，代价是无法统计他方条数（那本来也不需要）。
///
/// ⚠️ **本类刻意只有 `engine_` 一个成员**。为了让 `filter_engine.h` 不必包含
/// `fwpmu.h`（那会把 windows.h 的一堆宏带给每一处包含本头的翻译单元），
/// 「构造 WFP 过滤器」「按规则标识枚举」这些辅助全部放在 `.cpp` 的匿名 namespace 里，
/// 以自由函数的形式接收引擎句柄。它们本来也不需要访问对象状态。
class WinFilterEngine final : public IFilterEngine {
 public:
  WinFilterEngine();
  ~WinFilterEngine() override;

  WinFilterEngine(const WinFilterEngine&) = delete;
  WinFilterEngine& operator=(const WinFilterEngine&) = delete;

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

 private:
  /// WFP 引擎句柄。存成 `void*` 而不是 `HANDLE`，避免本头文件去包含 windows.h，
  /// 那会把它的一堆宏带给包含本头的每一处。
  void* engine_ = nullptr;
};

}  // namespace baniphelper::core
