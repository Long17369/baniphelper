#pragma once

#include <QList>

#include <cstdint>

#include "core/result.h"
#include "core/rule.h"
#include "platform/api/platform_types.h"

namespace baniphelper::core {

/// 过滤器引擎：把规则变成平台实际的过滤动作。
///
/// 实现必须满足以下契约（每一条都会写成契约测试，见 phases/08-portability.md 第 6 节）：
///
/// - **幂等**：同一个 `RuleId` 重复下发等价于下发一次，不产生重复过滤器。
///   连续增删同一规则一百次之后，过滤器总数必须回到初始值；
/// - **原子**：一条规则展开出的多条过滤器必须在一个事务内提交，失败时不得留下半成品；
///   平台支持事务机制时（Windows 上是 WFP 事务）必须用上，不允许逐条提交；
/// - **归属可辨**：本引擎下发的一切都必须挂在自有的 provider 与 sublayer 之下，
///   这样启动清理才能只清自家。**绝不允许按「层」全量清理**，那会误伤其他工具；
/// - **优先级由核心层给定**：`RuleSpec::priority` 是算好的，引擎只负责映射到平台的
///   优先级机制，不自行重新排序。`RuleAction::Allow` 必须整体高于 `RuleAction::Block`；
/// - **不支持的过滤族如实报错**：不支持 IPv6 时，含 IPv6 地址的规则必须返回
///   `ErrorCode::NotSupported`，**不允许只下发 IPv4 部分从而悄悄放走 IPv6**。
class IFilterEngine {
 public:
  IFilterEngine() = default;
  virtual ~IFilterEngine() = default;

  IFilterEngine(const IFilterEngine&) = delete;
  IFilterEngine& operator=(const IFilterEngine&) = delete;
  IFilterEngine(IFilterEngine&&) = delete;
  IFilterEngine& operator=(IFilterEngine&&) = delete;

  /// 打开引擎并注册自有 provider 与 sublayer。重复调用是安全的。
  [[nodiscard]] virtual Result<void> open() = 0;

  /// 关闭引擎。**不会**自动撤销已下发的过滤器，是否撤销由调用方决定。
  [[nodiscard]] virtual Result<void> close() = 0;

  [[nodiscard]] virtual bool isOpen() const = 0;

  /// 下发一条规则。幂等。
  [[nodiscard]] virtual Result<void> applyRule(const RuleSpec& rule) = 0;

  /// 撤销该规则下发的全部过滤器。对未下发的标识调用返回成功（幂等）。
  [[nodiscard]] virtual Result<void> revokeRule(const RuleId& id) = 0;

  /// 撤销本进程下发的一切。退出路径与「一键解除」都走它。
  [[nodiscard]] virtual Result<void> revokeAll() = 0;

  [[nodiscard]] virtual Result<QList<AppliedRuleSummary>> appliedRules() const = 0;

  /// 下发白名单模式的默认阻断。
  ///
  /// 单独成型而不是做成一条普通规则，是因为它必须落在**独立且权重更高**的位置上，
  /// 否则放行项赢不了它。调用方必须保证它在全部放行项**之后**下发。
  [[nodiscard]] virtual Result<void> applyDefaultBlock(std::int32_t priority) = 0;

  [[nodiscard]] virtual Result<bool> hasDefaultBlock() const = 0;

  /// 撤销默认阻断。回滚路径走这个，**只做删除不做新增**。
  [[nodiscard]] virtual Result<void> revokeDefaultBlock() = 0;

  /// 启动清理：清掉上一次运行残留的自家过滤器。
  ///
  /// 归属判据只能是自有 provider 与 sublayer。实现不得按层枚举后全量删除，
  /// 也不得因为「看起来像我们的」就删。
  [[nodiscard]] virtual Result<CleanupReport> cleanupOrphans() = 0;
};

}  // namespace baniphelper::core
