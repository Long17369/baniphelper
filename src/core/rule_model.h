#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

#include <cstdint>

#include "core/result.h"
#include "core/rule.h"

namespace baniphelper::core {

// ---------------------------------------------------------------------------
// 匹配域
// ---------------------------------------------------------------------------
//
// 域是**开放集合**：新增一个域只需新增一个求值器，规则结构不变
// （architecture.md 第 4.5 节）。所以这里给的是「本程序已认识的这一批」，
// 而 `MatchCondition::domain` 仍然以字符串保存，以便落库与跨进程传递。
//
// 遇到不认识的代号一律**报错**，绝不宽松解释。宁可这条规则不生效，
// 也不能按错误语义去封禁 —— 本项目的错误代价是断网。

/// 匹配域。
enum class MatchDomain : std::uint8_t {
  /// 程序域，取值是可执行文件路径。
  Proc,
  /// 地址域，取值是 IPv4 或 IPv6 的地址、网段、范围。
  Address,
  /// 端口域。是本地端口还是远端端口由方向域决定。
  Port,
  /// 协议域。
  Protocol,
  /// 方向域，站在本机视角。
  Direction,
};

/// 全部匹配域。
///
/// **新增域时必须同步加到这里**：界面枚举域、测试穷举域、文档覆盖检查都靠它，
/// 漏加的表现是「新域在界面上不存在」，而不是报错。
inline constexpr MatchDomain kAllMatchDomains[] = {
    MatchDomain::Proc,
    MatchDomain::Address,
    MatchDomain::Port,
    MatchDomain::Protocol,
    MatchDomain::Direction,
};

/// 落库与传输用的英文代号，例如 `proc`。
[[nodiscard]] const char* domainCode(MatchDomain domain) noexcept;

/// 界面显示用的中文名，例如「程序」。
[[nodiscard]] QString domainTitle(MatchDomain domain);

/// 解析英文代号。大小写不敏感；**不认识就失败**。
[[nodiscard]] Result<MatchDomain> parseDomain(const QString& code);

// ---------------------------------------------------------------------------
// 匹配方式
// ---------------------------------------------------------------------------

/// 匹配方式。
///
/// 方式是**跨域共用**的代号集合：`exact` 在程序域、地址域、端口域都表示
/// 「精确一个值」。某个域具体允许哪些方式由 `kModeSpecs` 决定，
/// 不允许的组合会被拒绝，不会被就近解释成别的意思。
enum class MatchMode : std::uint8_t {
  /// 精确一个值。
  Exact,
  /// 多个值的并集。
  Set,
  /// 不限取值，即该域的全部。
  Any,
  /// 路径通配（程序域）。
  Wildcard,
  /// 目录内的全部可执行文件（程序域）。
  Dir,
  /// 网段，写作 `地址/前缀长度`。
  Cidr,
  /// 范围，写作 `起-止`。
  Range,
  Tcp,
  Udp,
  /// 本机发起。
  Out,
  /// 外部发起。
  In,
  /// 出入站都要匹配。
  Both,
};

/// 全部匹配方式。用途同 `kAllMatchDomains`。
inline constexpr MatchMode kAllMatchModes[] = {
    MatchMode::Exact,
    MatchMode::Set,
    MatchMode::Any,
    MatchMode::Wildcard,
    MatchMode::Dir,
    MatchMode::Cidr,
    MatchMode::Range,
    MatchMode::Tcp,
    MatchMode::Udp,
    MatchMode::Out,
    MatchMode::In,
    MatchMode::Both,
};

/// 落库与传输用的英文代号，例如 `exact`。
[[nodiscard]] const char* modeCode(MatchMode mode) noexcept;

/// 界面显示用的中文名，例如「精确」。
[[nodiscard]] QString modeTitle(MatchMode mode);

/// 解析英文代号。大小写不敏感；**不认识就失败**。
[[nodiscard]] Result<MatchMode> parseMode(const QString& code);

// ---------------------------------------------------------------------------
// (域, 方式) 的定义
// ---------------------------------------------------------------------------

/// 取值在语法上属于哪一类。校验与规范化都按它分派。
///
/// 分成枚举而不是「每域一个函数」，是因为同一个形状在多个域下复用
/// （`Range` 既用于地址范围也用于端口范围），而形状只有一份。
enum class ValueShape : std::uint8_t {
  /// 不接受取值，`values` 必须为空。`any` 与协议、方向的枚举取值都属于这一类。
  None,
  /// 可执行文件全路径。
  ExecutablePath,
  /// 可执行文件路径的通配模式。
  WildcardPath,
  /// 目录路径。
  DirectoryPath,
  /// 单个 IP 地址字面量，IPv4 或 IPv6。
  AddressLiteral,
  /// 网段，写作 `地址/前缀长度`。
  Subnet,
  /// 地址范围，写作 `起-止`，两端必须同族。
  AddressRange,
  /// 端口号。
  PortNumber,
  /// 端口范围，写作 `起-止`。
  PortRange,
};

/// 一个 (域, 方式) 组合的定义。
struct ModeSpec {
  MatchDomain domain = MatchDomain::Proc;
  MatchMode mode = MatchMode::Exact;
  ValueShape shape = ValueShape::None;

  /// 具体度基准分。越大越具体，用于算优先级（architecture.md 第 4.6 节）。
  std::int32_t specificity = 0;

  /// 该方式**本身**就是「宽条件」。取反造成的宽度另行判定。
  bool wide = false;
};

/// 查 (域, 方式) 的定义。不认识的组合返回错误，**不做任何猜测**。
[[nodiscard]] Result<ModeSpec> findModeSpec(MatchDomain domain, MatchMode mode);

/// 某个域支持的全部方式，按定义表里的顺序。
[[nodiscard]] QList<MatchMode> modesOf(MatchDomain domain);

// ---------------------------------------------------------------------------
// 取值
// ---------------------------------------------------------------------------

/// 校验并规范化一个取值。
///
/// 规范化只做**不会让用户认不出来**的等价改写：IP 地址写成标准形式、端口去掉前导零。
/// **路径原样保留**，一个字符都不改 —— 用户写 `C:\Tools\a.exe` 就存这个。
///
/// 等价性的判断放到**比较的时候**做（见 `sameProcessIdentity`，那里按平台规则归一）：
/// 存储层做归一的话，改动是**有损**的（`..` 被解开、UNC 写法被改写），
/// 存进去就回不到用户写的样子，界面上也会出现「我没写过的形式」。
///
/// 大小写一律不改写，只在比较时忽略（同 `sameProcessIdentity`）。
///
/// 界面可以拿它做逐项即时校验。
[[nodiscard]] Result<QString> normalizeValue(MatchDomain domain,
                                             MatchMode mode,
                                             const QString& value);

/// 一个闭区间形式的地址范围。两端必须同族，起点不得大于终点。
///
/// 单独成一个类型而不是「两个字符串」，是因为展开器要按它算地址族、
/// 要与平台的地址区间匹配对上，用值类型能让「两端同族」这个约束写在类型上。
struct AddressSpan {
  Address lower;
  Address upper;
};

[[nodiscard]] inline bool operator==(const AddressSpan& lhs, const AddressSpan& rhs) noexcept {
  return lhs.lower == rhs.lower && lhs.upper == rhs.upper;
}

/// 一个闭区间形式的端口范围，含两端。两端相同表示单个端口。
struct PortSpan {
  std::uint16_t lower = 0;
  std::uint16_t upper = 0;
};

[[nodiscard]] inline bool operator==(const PortSpan& lhs, const PortSpan& rhs) noexcept {
  return lhs.lower == rhs.lower && lhs.upper == rhs.upper;
}

/// 把已规范化的地址字面量解析成值类型，顺带取回地址族。
///
/// 展开器要按地址族把过滤器拆开，而 `MatchCondition::values` 里只有文本，
/// 所以需要这一步。它对没经过规范化校验的输入同样安全：会从头校一遍。
[[nodiscard]] Result<Address> parseAddress(const QString& literal);

/// 把已规范化的网段取值（`地址/前缀长度`）展开成地址区间。
///
/// 网段在平台上是一次匹配，不该被拆成逐地址的过滤器 —— 那会把条数炸掉，
/// 所以展开器需要的是「网段的两个端点」而不是值本身。
[[nodiscard]] Result<AddressSpan> subnetToSpan(const QString& subnet);

/// 把已规范化的地址范围取值（`起-止`）解析成地址区间。
[[nodiscard]] Result<AddressSpan> addressRangeToSpan(const QString& range);

/// 把已规范化的端口取值（`80` 或 `80-443`）解析成端口区间。
[[nodiscard]] Result<PortSpan> portToSpan(const QString& value);

/// 地址的定长字节表示：IPv4 是 4 字节、IPv6 是 16 字节，一律网络序（大端）。
///
/// 平台层要拿它去构造过滤条件（WFP 的 IPv4 条件收 32 位整数、IPv6 收 16 字节数组），
/// 求补也要靠它做数值比较。放在核心层是为了让「地址怎么变成二进制」只有一处实现
/// —— 平台层自己再解析一遍，就多出一份可能与核心层不一致的规则，
/// 而两份规则不一致的表现是**同一份规则在不同地方封不同的范围**。
[[nodiscard]] Result<QByteArray> addressToBytes(const Address& address);

/// 求一组地址区间在其地址族全域内的补集（阶段二 S2.4）。
///
/// 用途只有一个，但是关键的一个：`addr ∉ {A, B}` 这种多值取反**不能**翻成
/// 「≠A 或 ≠B」—— 那个条件恒为真，等于把地址条件整个丢掉，规则会比预期
/// **封得宽**（见 `filter_plan.h` 顶部的说明）。必须先求出集合的补集，
/// 再用补集里的正向区间各出一个条件。
///
/// 入参必须同族、每段起点不大于终点；结果按起点升序，两两不重叠也不相邻。
/// **入参为空返回空**：「不限定」的补集是空集，而不是全集 ——
/// 空集与全集是相反的语义，调用方必须自己区分，这里不替调用方猜。
[[nodiscard]] Result<QList<AddressSpan>> complementAddressSpans(const QList<AddressSpan>& spans);

/// 求一组端口区间在 0–65535 内的补集。约束与语义同 `complementAddressSpans`。
[[nodiscard]] QList<PortSpan> complementPortSpans(const QList<PortSpan>& spans);

// ---------------------------------------------------------------------------
// 校验与规范化
// ---------------------------------------------------------------------------

/// 校验一个条件。不修改入参。
[[nodiscard]] Result<void> validateCondition(const MatchCondition& condition);

/// 校验并规范化一个条件。
///
/// **要么全改，要么不改**：失败时 `condition` 保持原样。
/// 半改的状态比不改更糟 —— 调用方无从知道该回滚哪些字段。
[[nodiscard]] Result<void> normalizeCondition(MatchCondition& condition);

/// 校验一条规则。
///
/// 校验的是四件事：`schema` 是当前版本、条件非空、每个条件合法、同一域不重复。
///
/// 不校验 `id`（新增规则时由上层生成），也不要求 `expireAt` 未过期
/// —— 过期规则只是不生效，不是非法。
[[nodiscard]] Result<void> validateRule(const Rule& rule);

/// 校验并规范化整条规则，语义同 `normalizeCondition`。
[[nodiscard]] Result<void> normalizeRule(Rule& rule);

// ---------------------------------------------------------------------------
// 具体度与优先级
// ---------------------------------------------------------------------------

/// `allow` 在优先级中的基址。
///
/// 「放行永远赢」不是靠比较两边的具体度运气，而是靠基址直接隔开：
/// 各条件具体度之和最多 `kMaxSpecificityTotal`，加上取反加成也远小于这个基址，
/// 所以**任何**一条放行规则的优先级都高于**任何**一条阻断规则。
inline constexpr std::int32_t kAllowPriorityBase = 10000;

/// `block` 在优先级中的基址。
inline constexpr std::int32_t kBlockPriorityBase = 0;

/// 取反条件的加成。
///
/// 刻意取小值：一条规则里最多 5 个条件，加成总和最大 5，
/// 而具体度的最小档差是 100，所以取反**只**影响同具体度之间的先后，
/// 不可能把一条规则抬到更具体的档位上。
inline constexpr std::int32_t kNegationBonus = 1;

/// 单个条件的具体度。越大越具体。条件非法时失败。
[[nodiscard]] Result<std::int32_t> conditionSpecificity(const MatchCondition& condition);

/// 整条规则的优先级，由 `RuleSpec::priority` 承载。越大越优先。
///
/// 它就是「动作基址 + 各条件具体度之和 + 取反加成」，语义只有这一处定义：
/// 引擎只负责把它单调映射到平台的优先级机制，不再自己解释条件。
[[nodiscard]] Result<std::int32_t> computePriority(const Rule& rule);

// ---------------------------------------------------------------------------
// 宽条件与风险分级
// ---------------------------------------------------------------------------

/// 一条规则里被判定为宽条件的那一处。
struct WidthIssue {
  /// 命中的条件在 `Rule::conditions` 里的下标。
  int conditionIndex = -1;

  /// 中文说明，界面直接显示。不允许为空 —— 没有说法的风险标记等于没标。
  QString description;
};

/// 风险等级。分级依据是宽条件的**个数**（architecture.md 第 4.7 节）。
enum class RiskLevel : std::uint8_t {
  /// 宽条件少于 2 个。
  Normal,
  /// 宽条件恰好 2 个。
  High,
  /// 宽条件 3 个及以上。
  Extreme,
};

/// 宽条件个数到风险等级的映射。
///
/// 单独导出是因为界面要在**还没构造出规则**时就能显示风险提示（边编辑边算），
/// 分级的阈值只应该有一处定义。
[[nodiscard]] RiskLevel riskLevelOf(int wideConditionCount) noexcept;

/// 宽条件评估结果。
struct WidthReport {
  QList<WidthIssue> issues;
  RiskLevel level = RiskLevel::Normal;

  [[nodiscard]] int count() const noexcept {
    return static_cast<int>(issues.size());
  }
};

/// 评估一条规则的宽度。规则非法时失败。
///
/// 宽条件包含四类：`any` 或 `both` 这种全取值方式、被取反的条件、
/// 以及过于宽泛的通配模式。全取值条件**不允许**再取反（那会让规则永不命中），
/// 这项校验在 `validateRule` 里已经拦下，所以这里每个条件最多贡献一处。
[[nodiscard]] Result<WidthReport> assessWidth(const Rule& rule);

}  // namespace baniphelper::core
