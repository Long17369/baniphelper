// 规则到过滤器清单的展开（阶段二 S2.2）。
//
// 本文件只做「拆」这一件事，产出的是纯 Qt 值类型，不碰任何 WFP 结构 ——
// 因此它能脱离引擎单独测（见 tests/filter_plan_test.cpp）。
// 把清单翻成 `FWPM_FILTER0` 是 `filter_plan_wfp.cpp` 的事。
//
// 拆的维度只有三个：方向、地址族、入站 UDP。它们都对应平台的硬约束
// （不同的层没法合并成一条）。**取值个数不是维度**：同一条过滤器里同一字段
// 可以出现多个条件，语义是「或」，这一点已经实测确认，依据写在头文件里。
//
// 一条贯穿的原则：**拆法必须确定**。同一份规则任何时候都要拆出同样的清单，
// 因为清单的条数会被拿去与实际下发的条数对账（`AppliedRuleSummary::filterCount`）。

#include "platform/win/filter_plan.h"

#include <QList>
#include <QString>
#include <QStringList>

#include "core/error.h"

namespace baniphelper::core {
namespace {

/// 一条规则里各域求值之后的取值集合。
///
/// 空列表统一表示「该域不限定」，展开时就不给对应字段加条件。
/// 「不限定」不等于「可以少做一族」—— 地址域不限定照样要 IPv4 与 IPv6 各出一条，
/// 那两件事的区别是本项目最容易出错的地方之一。
struct RuleValues {
  QList<QString> appPaths;
  bool appPathNegated = false;

  QList<AddressSpan> addresses;
  bool addressNegated = false;

  QList<PortSpan> ports;
  bool portNegated = false;

  /// 协议。空表示不限定：展开成一条**不设协议条件**的过滤器，
  /// 而不是 TCP 与 UDP 各一条 —— 后者条数翻倍而语义完全一样。
  QList<TransportProtocol> protocols;
  bool protocolNegated = false;

  /// 方向。规则里没有方向条件时两个都是真，即「不限方向」。
  bool outbound = true;
  bool inbound = true;
};

// ---------------------------------------------------------------------------
// 域求值器
// ---------------------------------------------------------------------------

Result<void> evaluateProc(const MatchCondition& condition, RuleValues& values) {
  const MatchMode mode = parseMode(condition.mode).value();
  if (mode == MatchMode::Any) {
    return Result<void>::ok();
  }
  if (mode == MatchMode::Wildcard || mode == MatchMode::Dir) {
    // 这两种要先把「系统里有哪些程序」变成确定的路径列表才能落成过滤器。
    // 阶段二把它们排在 S2.11 与 S2.12，在那之前如实报不支持。
    return Result<void>::fail(unsupportedError(
        QStringLiteral("程序域的「%1」匹配").arg(modeTitle(mode)),
        QStringLiteral(
            "要先知道系统里有哪些程序才能落成确定的过滤器，计划在 S2.11 与 S2.12 落地")));
  }

  for (const QString& value : condition.values) {
    values.appPaths.append(value);
  }
  values.appPathNegated = condition.negate;
  return Result<void>::ok();
}

Result<void> evaluateAddress(const MatchCondition& condition, RuleValues& values) {
  const MatchMode mode = parseMode(condition.mode).value();
  if (mode == MatchMode::Any) {
    return Result<void>::ok();
  }

  for (const QString& value : condition.values) {
    Result<AddressSpan> span =
        makeError(ErrorCode::Internal, QStringLiteral("没有处理的地址匹配方式"));
    switch (mode) {
      case MatchMode::Exact: {
        auto address = parseAddress(value);
        if (!address) {
          return Result<void>::fail(address.error());
        }
        AddressSpan single;
        single.lower = address.value();
        single.upper = address.value();
        span = single;
        break;
      }
      case MatchMode::Cidr:
        span = subnetToSpan(value);
        break;
      case MatchMode::Range:
        span = addressRangeToSpan(value);
        break;
      default:
        break;
    }

    if (!span) {
      return Result<void>::fail(span.error());
    }
    values.addresses.append(span.value());
  }

  values.addressNegated = condition.negate;
  return Result<void>::ok();
}

Result<void> evaluatePort(const MatchCondition& condition, RuleValues& values) {
  const MatchMode mode = parseMode(condition.mode).value();
  if (mode == MatchMode::Any) {
    return Result<void>::ok();
  }

  // `exact` / `set` / `range` 的取值写法在规范化之后是统一的（`80` 或 `80-443`），
  // 所以这里不必按方式分支。
  for (const QString& value : condition.values) {
    auto span = portToSpan(value);
    if (!span) {
      return Result<void>::fail(span.error());
    }
    values.ports.append(span.value());
  }

  values.portNegated = condition.negate;
  return Result<void>::ok();
}

Result<void> evaluateProtocol(const MatchCondition& condition, RuleValues& values) {
  const MatchMode mode = parseMode(condition.mode).value();
  if (mode == MatchMode::Any) {
    return Result<void>::ok();
  }

  values.protocols.append(mode == MatchMode::Tcp ? TransportProtocol::Tcp : TransportProtocol::Udp);
  values.protocolNegated = condition.negate;
  return Result<void>::ok();
}

Result<void> evaluateDirection(const MatchCondition& condition, RuleValues& values) {
  const MatchMode mode = parseMode(condition.mode).value();
  values.outbound = mode == MatchMode::Out || mode == MatchMode::Both;
  values.inbound = mode == MatchMode::In || mode == MatchMode::Both;
  return Result<void>::ok();
}

/// 域求值器注册表。
///
/// 用表而不是就地 `switch`，是为了让 S2.11 与 S2.12 只加一个求值器就能接上
/// 通配与目录匹配，展开流程一行都不用改（architecture.md 第 4.5 节）。
struct DomainEvaluator {
  MatchDomain domain;
  Result<void> (*evaluate)(const MatchCondition& condition, RuleValues& values);
};

constexpr DomainEvaluator kEvaluators[] = {
    {MatchDomain::Proc, evaluateProc},
    {MatchDomain::Address, evaluateAddress},
    {MatchDomain::Port, evaluatePort},
    {MatchDomain::Protocol, evaluateProtocol},
    {MatchDomain::Direction, evaluateDirection},
};

// ---------------------------------------------------------------------------
// 拆分维度
// ---------------------------------------------------------------------------

/// 一组条件要落在哪些落点上。
///
/// 顺序是固定的：出站、入站接收、入站数据报。顺序影响清单的排列，
/// 而清单要可比对，所以不能让它随实现细节漂移。
QList<FilterStage> stagesFor(bool outbound, bool inbound, bool udp) {
  QList<FilterStage> stages;
  if (outbound) {
    stages.append(FilterStage::OutboundConnect);
  }
  if (inbound) {
    stages.append(FilterStage::InboundAccept);
    if (udp) {
      stages.append(FilterStage::InboundDatagram);
    }
  }
  return stages;
}

/// 地址条件按地址族切开的结果。`spans` 为空表示这一族不限定地址。
struct AddressSlice {
  AddressFamily family = AddressFamily::V4;
  QList<AddressSpan> spans;
};

/// 把地址条件按族切开。
///
/// 地址域不限定（或规则里根本没有地址条件）时，两族各出一条「不限定地址」的切片：
/// 「不限定」要的是**每一族都不限**，不是「只在 IPv4 上限」，那是两回事。
QList<AddressSlice> addressSlices(const QList<AddressSpan>& spans) {
  if (spans.isEmpty()) {
    return QList<AddressSlice>{AddressSlice{AddressFamily::V4, {}},
                               AddressSlice{AddressFamily::V6, {}}};
  }

  QList<AddressSlice> slices;
  for (const AddressFamily family : {AddressFamily::V4, AddressFamily::V6}) {
    AddressSlice slice;
    slice.family = family;
    for (const AddressSpan& span : spans) {
      if (span.lower.family == family) {
        slice.spans.append(span);
      }
    }
    if (!slice.spans.isEmpty()) {
      slices.append(slice);
    }
  }
  return slices;
}

}  // namespace

const char* filterStageName(FilterStage stage) noexcept {
  switch (stage) {
    case FilterStage::OutboundConnect:
      return "outbound-connect";
    case FilterStage::InboundAccept:
      return "inbound-accept";
    case FilterStage::InboundDatagram:
      return "inbound-datagram";
  }
  return "";
}

const char* filterFieldName(FilterField field) noexcept {
  switch (field) {
    case FilterField::AppPath:
      return "app-path";
    case FilterField::RemoteAddress:
      return "remote-address";
    case FilterField::Port:
      return "port";
    case FilterField::Protocol:
      return "protocol";
  }
  return "";
}

Result<FilterPlan> expandRule(const RuleSpec& rule) {
  // 先走一遍 S2.1 的整规则校验：空条件、同域重复、取值不合法都在那里拦。
  // 不假设调用方一定校验过 —— 展开器的入参来自配置、数据库或界面。
  Rule checked;
  checked.id = rule.id;
  checked.action = rule.action;
  checked.conditions = rule.conditions;
  auto normalized = normalizeRule(checked);
  if (!normalized) {
    return Result<FilterPlan>::fail(normalized.error());
  }

  RuleValues values;
  for (const MatchCondition& condition : checked.conditions) {
    const MatchDomain domain = parseDomain(condition.domain).value();
    bool handled = false;
    for (const DomainEvaluator& evaluator : kEvaluators) {
      if (evaluator.domain != domain) {
        continue;
      }
      auto evaluated = evaluator.evaluate(condition, values);
      if (!evaluated) {
        return Result<FilterPlan>::fail(evaluated.error());
      }
      handled = true;
      break;
    }
    if (!handled) {
      // 正常到不了这里：normalizeRule 已经拒绝过不认识的域。
      return Result<FilterPlan>::fail(
          makeError(ErrorCode::Internal,
                    QStringLiteral("域「%1」没有对应的求值器").arg(domainTitle(domain))));
    }
  }

  if (!values.outbound && !values.inbound) {
    return Result<FilterPlan>::fail(
        makeError(ErrorCode::InvalidArgument,
                  QStringLiteral("规则既不要出站也不要入站，展开不出任何过滤器")));
  }

  // 同字段条件太多说明规则写歪了（常见于把一整张地址表倒进来），当场拒绝。
  const auto tooManyValues = [](int count, const QString& field) -> Result<void> {
    if (count <= kMaxConditionsPerField) {
      return Result<void>::ok();
    }
    return Result<void>::fail(makeError(
        ErrorCode::InvalidArgument,
        QStringLiteral("「%1」域给了 %2 个取值，超过 %3 个。过滤器的单个字段塞不下这么多，"
                       "请拆成几条规则")
            .arg(field)
            .arg(count)
            .arg(kMaxConditionsPerField)));
  };
  auto countCheck =
      tooManyValues(static_cast<int>(values.appPaths.size()), domainTitle(MatchDomain::Proc));
  if (!countCheck) {
    return Result<FilterPlan>::fail(countCheck.error());
  }
  countCheck =
      tooManyValues(static_cast<int>(values.addresses.size()), domainTitle(MatchDomain::Address));
  if (!countCheck) {
    return Result<FilterPlan>::fail(countCheck.error());
  }
  countCheck = tooManyValues(static_cast<int>(values.ports.size()), domainTitle(MatchDomain::Port));
  if (!countCheck) {
    return Result<FilterPlan>::fail(countCheck.error());
  }

  // 协议不限定时也**要**带上数据报层：UDP 是其中一员，而 UDP 没有连接建立这一步。
  const bool protocolRestricted = !values.protocols.isEmpty();
  const bool hasUdp = protocolRestricted ? values.protocols.contains(TransportProtocol::Udp) : true;

  FilterPlan plan;
  for (const FilterStage stage : stagesFor(values.outbound, values.inbound, hasUdp)) {
    for (const AddressSlice& slice : addressSlices(values.addresses)) {
      FilterPlanEntry entry;
      entry.stage = stage;
      entry.action = rule.action;
      entry.family = slice.family;
      entry.priority = rule.priority;

      // 条件的排列顺序固定：程序、地址、端口、协议。
      // 顺序固定是为了让清单可比对，日志与测试都不必自己排序。
      for (const QString& appPath : values.appPaths) {
        FilterCondition condition;
        condition.field = FilterField::AppPath;
        condition.negate = values.appPathNegated;
        condition.appPath = appPath;
        entry.conditions.append(condition);
      }
      for (const AddressSpan& span : slice.spans) {
        FilterCondition condition;
        condition.field = FilterField::RemoteAddress;
        condition.negate = values.addressNegated;
        condition.address = span;
        entry.conditions.append(condition);
      }
      for (const PortSpan& port : values.ports) {
        FilterCondition condition;
        condition.field = FilterField::Port;
        condition.negate = values.portNegated;
        condition.portLower = port.lower;
        condition.portUpper = port.upper;
        entry.conditions.append(condition);
      }
      if (protocolRestricted) {
        for (const TransportProtocol protocol : values.protocols) {
          FilterCondition condition;
          condition.field = FilterField::Protocol;
          condition.negate = values.protocolNegated;
          condition.protocol = protocol;
          entry.conditions.append(condition);
        }
      }

      plan.entries.append(entry);
      if (plan.entries.size() > kMaxPlanEntries) {
        // 超限就整体拒绝，不截断：截断会把一条「部分生效」的规则
        // 伪装成下发成功，那比拒绝危险得多。
        return Result<FilterPlan>::fail(
            makeError(ErrorCode::InvalidArgument,
                      QStringLiteral("这条规则展开出的过滤器超过 %1 条，请拆成多条规则")
                          .arg(kMaxPlanEntries)));
      }
    }
  }

  return plan;
}

}  // namespace baniphelper::core
