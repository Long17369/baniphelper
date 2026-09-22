#pragma once

#include <QString>

#include <memory>

#include "core/icapabilities.h"
#include "core/result.h"
#include "platform/api/ifilterengine.h"
#include "platform/api/ikiller.h"
#include "platform/api/ipaths.h"
#include "platform/api/iprivilege.h"
#include "platform/api/isingleinstance.h"

namespace baniphelper::core {

/// 单实例的默认名字。平台实现自行决定命名空间前缀（Windows 上会用 `Local\`）。
inline constexpr char kSingleInstanceName[] = "BanIPHelper.SingleInstance";

/// 平台后端：把当前平台的一组实现聚在一起交给上层。
///
/// 这是**唯一**允许上层与平台实现打交道的地方。`core/` 与 `ui/` 只包含本头文件，
/// 绝不包含 `platform/<os>/` 下的任何头，因此换平台时它们一行都不用改。
///
/// 声明放在 `core/` 而实现分平台，是刻意的依赖倒置：上层依赖的是一个平台无关的
/// 装配入口，平台实现反过来去实现它。若把声明放进 `platform/`，`core/` 就得反过来
/// 依赖平台层，分层立刻失效。
///
/// 目前装的是已经做出来的成员，后续步骤按需要往里加
/// （连接监视、字节统计、会话事件、自启动、目标解析）。
struct PlatformBackend {
  /// 后端标识，写进日志，例如 `win`、`memory`。不允许为空。
  QString name;

  std::unique_ptr<IPrivilege> privilege;
  std::unique_ptr<ISingleInstance> singleInstance;
  std::unique_ptr<IFilterEngine> filterEngine;

  /// 连接中断（S2.8）。
  ///
  /// ⚠️ **不与 `filterEngine` 合在一起**，尽管两者总是被同一个动作一起用。
  /// 理由是它们的可用性依赖不同的东西：过滤器引擎只需要提起过权，
  /// 而断连在某些平台上根本做不到（IPv6 就是常态）。
  /// 合成一个之后，「规则能下发但连接断不掉」这个状态就表达不出来了。
  std::unique_ptr<IKiller> killer;

  /// 配置、数据库与日志目录。
  ///
  /// 日志必须在启动最早期就起来，而上层（`ui/`）不被允许直接问系统要路径，
  /// 所以它只能从这里拿 —— 等不到配置子系统 S1.5 做完。
  std::unique_ptr<IPaths> paths;

  /// 该后端声明具备哪些能力。上层据此决定哪些操作要置灰。
  std::unique_ptr<ICapabilities> capabilities;

  /// 各成员是否都填上了。装配不完整属于缺陷，调用方必须显式失败而不是继续跑。
  [[nodiscard]] bool isComplete() const;
};

/// 构造当前平台的后端。
///
/// 实现在 `platform/<os>/backend.cpp` 里，一个平台一份，由 CMake 在编译期挑一个。
///
/// `singleInstanceName` 留空表示用默认名 `kSingleInstanceName`。
/// 之所以留这个口子：契约测试需要用一个临时名字构造实例，否则会和正在运行的程序
/// 抢同一个内核对象，测出来的结果取决于当时有没有开着程序。
/// 生产代码一律留空，不要传名字。
[[nodiscard]] Result<PlatformBackend> createPlatformBackend(
    const QString& singleInstanceName = QString());

}  // namespace baniphelper::core
