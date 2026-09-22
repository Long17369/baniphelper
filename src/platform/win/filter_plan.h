#pragma once

#include <QList>
#include <QString>

#include <cstdint>

#include "core/result.h"
#include "core/rule.h"
#include "core/rule_model.h"
#include "core/types.h"

namespace baniphelper::core {

// ---------------------------------------------------------------------------
// 过滤器清单
// ---------------------------------------------------------------------------
//
// 一条规则在 Windows 上要拆成几条过滤器，取决于下面三个维度：
//
// - **方向**：出站与入站是两个不同的层，各自一条；
// - **地址族**：IPv4 与 IPv6 是两个不同的层，各自一条；
// - **入站 UDP**：UDP 没有连接建立这一步，除了接收授权还要再补一条数据报层。
//
// 每个维度都对应平台的硬约束（不同的层没法合并），所以条数 = 落点数 × 地址族数，很小。
//
// ⚠️ **取值个数不是维度**：同一条过滤器里，同一个字段可以出现多个条件，
// 它们的语义是**「或」**。这一条是实测确认的，不是推断：
// 给一条 Windows 防火墙规则填两个远程地址，导出的 WFP 结果是**一条** filter，
// 里面 FWPM_CONDITION_IP_REMOTE_ADDRESS 出现了两次；再用一条只列到其中一个地址的
// 出站阻止规则去连那个地址，确实连不上 —— 若多条件是「与」，那条规则恒为假，
// 根本拦不住任何东西。复现脚本见 `tmp/probe-fw-multivalue.ps1` 与
// `tmp/probe-fw-or-semantics.ps1`。
//
// ⚠️ 但**取反不能这样合并**。`addr ∉ {A, B}` 写成「≠A 或 ≠B」是恒真的，
// 等于把地址条件整个丢掉，规则会比预期**封得宽** —— 这是最容易发生又最难发现的错误。
// 所以取反怎么翻译必须由平台层按字段逐个决定，见 `FilterCondition::negate`。
//
// ⚠️ **本文件刻意不包含任何平台专有头**（`fwpmu.h` 之类）：
// 拆法本身是纯逻辑，能独立测；把它翻成 `FWPM_FILTER0` 才是平台相关的一步，
// 那一步在 `filter_plan_wfp.cpp` 里。将来若另一个平台的拆法与此相同，
// 把这几个文件整体上移到 `core/` 即可，依赖方向不用改。

/// 过滤落点。决定过滤器落在平台的哪一层、哪个动作上。
enum class FilterStage : std::uint8_t {
  /// 出站新连接的授权判定。客户端发起连接时走这里。
  OutboundConnect,
  /// 入站新连接的接收授权判定。对方连过来时走这里。
  InboundAccept,
  /// 入站 UDP 数据报。
  ///
  /// 单独存在是因为 UDP 没有连接建立这一步：只挡接收授权拦不住已经在发包的对端，
  /// 必须再在数据报层补一条（见 docs/architecture.md 第 3 节）。
  InboundDatagram,
};

/// 落点的英文代号，供日志与测试输出使用。
[[nodiscard]] const char* filterStageName(FilterStage stage) noexcept;

/// 过滤条件的字段。
enum class FilterField : std::uint8_t {
  /// 程序可执行文件全路径。平台层负责转成 NT 设备路径。
  AppPath,
  /// 远端地址。
  RemoteAddress,
  /// 端口。
  ///
  /// **出站条目上是远端端口，入站条目上是本地端口**：站在本机视角，
  /// 「封 80 端口」在出站方向是「不许连别人的 80」，在入站方向是「不许别人连我的 80」。
  /// 对方连过来时它的源端口是随机的，拿它做条件没有意义。
  Port,
  /// 传输协议。
  Protocol,
};

/// 字段的英文代号，供日志与测试输出使用。
[[nodiscard]] const char* filterFieldName(FilterField field) noexcept;

/// 一个已定型的过滤条件。
///
/// 取值都在这里，但**只有一个字段有效**（由 `field` 决定），其余保持默认值。
/// 这样安排而不是用变体，是为了让构造与读取两边的写法都直白：
/// 展开器填哪个字段、平台层读哪个字段，都一眼看得出来。
struct FilterCondition {
  FilterField field = FilterField::AppPath;

  /// 取反。
  ///
  /// ⚠️ **展开期不给它求补**。把「不在这个集合里」算成若干个正向区间是一个选择，
  /// 但那是**表达方式**的选择，不是语义的一部分。所以这里原样带着标记往下传，
  /// 由 `resolveNegation` 统一决定这处取反怎么落地。
  ///
  /// ⚠️ 硬约束：**同一字段上如果有不止一个条件，就不能翻成「不等于」**。
  /// 多个条件是「或」，而 `≠A 或 ≠B` 恒真，等于把条件整个丢掉 ——
  /// 规则会比预期**封得宽**，是这类错误里最难发现的方向。
  bool negate = false;

  /// `AppPath` 用。
  QString appPath;

  /// `RemoteAddress` 用。两端同族，起点不大于终点；两端相同表示单个地址。
  AddressSpan address;

  /// `Port` 用。含两端，两端相同表示单个端口。
  std::uint16_t portLower = 0;
  std::uint16_t portUpper = 0;

  /// `Protocol` 用。
  TransportProtocol protocol = TransportProtocol::Tcp;
};

/// 一条过滤器。
struct FilterPlanEntry {
  FilterStage stage = FilterStage::OutboundConnect;
  RuleAction action = RuleAction::Block;

  /// 地址族。只支持 IPv4 的平台遇到 IPv6 条目必须**整体报错**，
  /// 不允许只下发 IPv4 那一部分从而悄悄放走 IPv6。
  AddressFamily family = AddressFamily::V4;

  /// 优先级，与所属规则一致。同一条规则展开出的所有条目共用它。
  std::int32_t priority = 0;

  /// 条件。
  ///
  /// **同一字段可以出现多次，语义是「或」**；不同字段之间是「与」。
  /// 多地址、多端口、多程序路径就是这样表达的 —— 不必拆成多条过滤器。
  QList<FilterCondition> conditions;
};

/// 一条规则的展开结果。
struct FilterPlan {
  QList<FilterPlanEntry> entries;

  /// 条目数量只是**下限**。
  ///
  /// 展开期不对取反求补（那是平台的表达自由），所以平台若为了表达取反而多拆几条，
  /// 实际条数会大于它。核对规则是：**实际条数不得少于它，少了就是有东西没下发**。
  [[nodiscard]] int lowerBoundCount() const noexcept {
    return static_cast<int>(entries.size());
  }
};

/// 单条规则的过滤器条数上限。
///
/// 条数只由落点与地址族决定（最多 3 × 2），正常怎么都到不了这个数。
/// 留着它是因为展开的维度将来会增加（例如把本地端口与远端端口分开出条目），
/// 而一旦维度乘积失控，每一条都要占内核资源。超限时**拒绝下发并说明原因**，
/// 而不是截断 —— 截断会把一条「部分生效」的规则伪装成成功。
inline constexpr int kMaxPlanEntries = 64;

/// 单条过滤器里同一个字段最多能放多少个取值。
///
/// 同字段条件太多会让过滤器又大又难匹配，而且这种规则几乎总是写错了
/// （例如把一整张地址表倒进一条规则里）。宁可在展开期就报错让人去改
/// 或者拆成几条规则，而不是下发一条能跑但没法维护的东西。
inline constexpr int kMaxConditionsPerField = 256;

/// 把一条规则展开成过滤器清单。
///
/// 展开做的事就是本文件开头列的那四个维度。取反不求补；集合型取值一律拆成
/// 一条一个取值。
///
/// 目前只支持 `proc` 域的 `exact` / `set` / `any`。`wildcard` 与 `dir`
/// 需要先知道「系统里有哪些程序」才能落成确定的过滤器，属于
/// [phases/02-filtering.md](../../../docs/phases/02-filtering.md) 的 S2.11 与 S2.12；
/// 在那之前遇到它们一律返回 `ErrorCode::NotSupported` 并说明。
/// **不静默跳过**：跳过的后果是规则看上去下发了、实际没封。
[[nodiscard]] Result<FilterPlan> expandRule(const RuleSpec& rule);

/// 把一个条目里的取反条件化成平台可直接表达的形式（阶段二 S2.4）。
///
/// `expandRule` 刻意**不求补**，把 `negate` 原样交出来；求补与报错都在这一步。
/// 判定规则是：
///
/// | 字段 | 该字段的条件数 | 取反 | 处理 |
/// | --- | --- | --- | --- |
/// | 地址、端口、协议 | 任意 | 是 | **求补**，换成补集里的正向取值 |
/// | 程序 | 1 | 是 | 原样保留，交给平台的不等匹配 |
/// | 程序 | 多个 | 是 | **报错**：算不出补集，而多值只能「或」，`≠A 或 ≠B` 恒真 |
/// | 任意 | 任意 | 否 | 原样保留，同字段多条件本来就是「或」 |
///
/// 除程序域的单值取反之外，返回的条目里 `negate` **一律为假**，
/// 平台层因此不必再判断取反该怎么表达 —— 一处判断，只有一处实现。
///
/// 地址与端口两条都可能**求补为空**（例如 `addr ∉ {0.0.0.0/0}`），
/// 那是一条永不生效的规则，与「不限定这个字段」正好相反，所以报错而不是放行。
[[nodiscard]] Result<FilterPlanEntry> resolveNegation(const FilterPlanEntry& entry);

}  // namespace baniphelper::core
