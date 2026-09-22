#pragma once

#include <QDateTime>
#include <QList>
#include <QString>

#include "core/irulestore.h"
#include "core/result.h"
#include "core/rule.h"
#include "core/types.h"
#include "platform/api/ifilterengine.h"

namespace baniphelper::core {

/// 某条规则在这次操作里没能生效，以及为什么。`reason` 不允许为空。
struct RuleProblem {
  RuleId id;
  QString reason;
};

/// 启动恢复的结果。
///
/// 四类**互不重叠**，加起来就是库里规则的总数 —— 这不是巧合，
/// 「每一条规则都要有交代」正是这个结构存在的意义：
/// 一条既没下发、又不在任何一类里的规则，就是一次静默失败。
struct RestoreReport {
  int applied = 0;
  int alreadyExpired = 0;
  int disabled = 0;
  QList<RuleProblem> failures;

  [[nodiscard]] int total() const noexcept {
    return applied + alreadyExpired + disabled + static_cast<int>(failures.size());
  }
};

/// 一趟过期巡检的结果。
///
/// 与 `RestoreReport` 同样是「每条都要有交代」：`disabled` 与 `failures` 加起来
/// 就是这一趟该处理的条数。只有「压根问不出到期列表」才会让整个调用失败。
struct ExpireReport {
  /// 已经撤下过滤器并停用的标识。
  QList<RuleId> disabled;

  /// 没处理成功的那些，逐条带原因。
  QList<RuleProblem> failures;
};

/// 规则的生命周期编排：落库、下发、撤销、过期失效、启动恢复。
///
/// 它把「存」（`IRuleStore`）与「生效」（`IFilterEngine`）绑在一起，并**规定顺序与回滚**。
/// 单独成型的理由与 `ban_action` 同源：写规则的入口将来会有好几个
/// （规则编辑器、连接列表右键、WebUI、导入），每处各写一遍顺序就一定有一处会写错，
/// 而写错的症状是「库里显示启用、实际没生效」这种界面上看不出来的状态。
///
/// 三条规矩：
///
/// 1. **先下发、后落库。** 反过来的话，落库成功而下发失败会留下一条
///    「库里写着启用、实际没生效」的规则，而界面上看不出任何异常；
/// 2. **落库失败要把引擎恢复成原样。** 规则可能本来就存在（更新场景），
///    所以恢复动作是「把库里那条**旧版本**重新下发」，而不是一律撤销 ——
///    后者会让更新前的旧规则也跟着失效，等于编辑失败顺带把原来能用的东西弄没了；
/// 3. **过期失效先撤、后停用。** 撤销失败就不改启用状态：规则还在生效，
///    库里的记录必须与事实一致，否则重启之后它会以「启用中」被重新下发。
///
/// ⚠️ `save` **不做过期判定**：存一条已经过期的规则是合法的（`validateRule` 明说
/// 「过期规则只是不生效，不是非法」），而把它下不下发由 `expireDue` 那一趟统一处理。
/// 调用方应当在启动时与运行期间都调它，别指望 `save` 顺手代劳。
class RuleRuntime {
 public:
  RuleRuntime(IRuleStore& store, IFilterEngine& engine);

  RuleRuntime(const RuleRuntime&) = delete;
  RuleRuntime& operator=(const RuleRuntime&) = delete;

  /// 新增或更新一条规则：先下发（或按 `enabled` 撤销），再落库。失败时引擎恢复到原样。
  [[nodiscard]] Result<void> save(const Rule& rule);

  /// 删除一条规则：先撤销过滤器，再删库。失败时把过滤器装回去。
  [[nodiscard]] Result<void> remove(const RuleId& id);

  /// 启动恢复：把库里「启用且未过期」的规则逐条下发。
  ///
  /// 单条失败**不中断**：剩下的规则照样下发，失败的那些逐条出现在 `failures` 里。
  /// 一条规则写坏了不该让其它规则全都不生效。
  [[nodiscard]] Result<RestoreReport> restore();

  /// 把已到期的规则撤下来并停用，返回这一趟的结果。
  ///
  /// 幂等：已经撤过、已经停用的规则不会出现在 `expired()` 里，再调也不会重复。
  /// 单条失败不中断剩下的 —— 一条规则撤不掉不该让其它过期规则继续生效。
  [[nodiscard]] Result<ExpireReport> expireDue(const QDateTime& now);

 private:
  IRuleStore& store_;
  IFilterEngine& engine_;
};

}  // namespace baniphelper::core
