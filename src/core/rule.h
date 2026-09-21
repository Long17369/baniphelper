#pragma once

#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>

#include <cstdint>

#include "core/types.h"

namespace baniphelper::core {

/// 规则的稳定标识。用字符串而不是整数自增值，
/// 因为规则要落库、要跨进程传递、还要出现在 WebUI 的 URL 里。
using RuleId = QString;

/// 规则动作。
///
/// `Allow` 的优先级**整体高于** `Block`，白名单模式正是靠这一点实现，
/// 因此这里不是「先到先得」，而是「放行永远赢」。
enum class RuleAction : std::uint8_t {
  Block,
  Allow,
};

/// 一条匹配条件。
///
/// 语义（见 architecture.md 第 4.4 节，实现时必须逐条对上）：
///
/// - 条件之间是**交集**：一条规则命中的前提是所有条件都满足；
/// - 条件之内是**并集**：`values` 中任一命中，该条件即满足；
/// - `negate` 作用于**该条件整体**，等价于「不在这个集合里」。
struct MatchCondition {
  /// 匹配域：`proc`、`addr`、`port`、`proto`、`direction`。
  /// 域是开放集合，靠求值器注册表扩展；遇到不认识的域必须**拒绝加载并报错**。
  QString domain;

  /// 该域下的匹配方式，例如 `proc` 的 `exact` / `set` / `any` / `wildcard` / `dir`。
  /// 同样是开放集合，不认识的方式同样必须拒绝，**绝不宽松解释**。
  QString mode;

  /// 匹配值。文本形式由域与方式共同决定。
  QStringList values;

  /// 取反。会让规则变宽且不易察觉，因此界面必须把整条规则渲染成一句中文。
  bool negate = false;
};

[[nodiscard]] inline bool operator==(const MatchCondition& lhs,
                                     const MatchCondition& rhs) noexcept {
  return lhs.domain == rhs.domain && lhs.mode == rhs.mode && lhs.values == rhs.values &&
         lhs.negate == rhs.negate;
}

/// 当前规则格式版本。格式一旦变更，就靠它做迁移，不做「猜着读」。
inline constexpr int kCurrentRuleSchema = 1;

/// 规则，落库的完整形式。
struct Rule {
  RuleId id;
  RuleAction action = RuleAction::Block;

  /// **不允许为空**。要表达「全部」必须显式写一个 `mode = any` 的条件，
  /// 让审阅者一眼看到「这是全部」，而不是靠留空隐式得到。
  QList<MatchCondition> conditions;

  bool enabled = true;

  /// 自动过期时刻。无效值表示不过期。
  QDateTime expireAt;

  /// 备注，只给人和审计看，不参与判定。
  QString note;

  /// 规则格式版本，用于迁移。
  int schema = kCurrentRuleSchema;

  /// 创建时刻。同具体度时**较早者优先**，用于保证结果稳定可复现。
  QDateTime createdAt;
};

/// 规则的下发形式。
///
/// 与 `Rule` 分开，是因为引擎只关心判定所需的信息：
/// 优先级由核心层按条件具体度算出、不落库，而 `note`、`schema` 这类持久化字段与引擎无关。
struct RuleSpec {
  RuleId id;
  RuleAction action = RuleAction::Block;

  /// 不允许为空，同 `Rule::conditions`。
  QList<MatchCondition> conditions;

  /// 越大越优先。由核心层统一计算，引擎只负责按它映射到平台的优先级机制
  /// （Windows 上体现为子层权重）。
  std::int32_t priority = 0;

  /// 自动过期时刻。无效值表示不过期。引擎可据此自行回收，也可由核心层按时撤销。
  QDateTime expireAt;
};

}  // namespace baniphelper::core
