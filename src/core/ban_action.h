#pragma once

#include <QList>

#include "core/result.h"
#include "core/rule.h"
#include "core/types.h"
#include "platform/api/ifilterengine.h"
#include "platform/api/ikiller.h"

namespace baniphelper::core {

/// 封禁一条规则，**随后**断开受它影响的已有连接。
///
/// 这个函数存在的全部理由就是**顺序**。
///
/// 过滤层（`ALE_AUTH_CONNECT` / `ALE_AUTH_RECV_ACCEPT`）只在**新连接**时判定，
/// 规则下发不会影响已经建立的连接，所以要额外断一下；而断的动作会让应用层立刻重连。
/// 顺序反了的话，重连发生在规则生效**之前**，那一瞬间是能成功的 ——
/// 用户看到的现象是「点了封禁，连接闪了一下又回来了」，而且只在少数时序下复现。
///
/// 由此推出两条硬约束：
///
/// 1. **规则下发失败时，一条连接都不许断。** 断了却封不住，等于白白打断用户的连接
///    而问题依旧；此时返回的失败就是下发那一步的失败，原样传出去。
/// 2. **下发成功之后才断，中间不插入任何别的动作。**
///
/// `established` 是**已经筛好的**连接清单 —— 「哪些连接受这条规则影响」是规则匹配的事，
/// 本函数不判断。清单为空时**不会调用**断连实现：没有东西可断，
/// 「断连实现一次都没被碰过」比「用空列表调用它一次」更容易核对。
///
/// 断连是**逐条**上报的：某一条断不掉不会让整件事失败，那条会出现在
/// `KillReport::failures` 里，界面据此说清「哪一条还活着」。
[[nodiscard]] Result<KillReport> applyBanAndKill(IFilterEngine& engine,
                                                 IKiller& killer,
                                                 const RuleSpec& rule,
                                                 const QList<ConnectionKey>& established);

}  // namespace baniphelper::core
