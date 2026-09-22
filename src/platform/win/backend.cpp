#include "core/platform_backend.h"

#include <memory>

#include "core/declared_capabilities.h"
#include "platform/win/conn_monitor.h"
#include "platform/win/filter_engine.h"
#include "platform/win/killer.h"
#include "platform/win/paths.h"
#include "platform/win/privilege.h"
#include "platform/win/single_instance.h"

namespace baniphelper::core {

Result<PlatformBackend> createPlatformBackend(const QString& singleInstanceName) {
  PlatformBackend backend;
  backend.name = QStringLiteral("win");

  // 只声明已经真正实现的能力。还没做的一律不写进来，界面据此显式禁用。
  // 声明了却做不到，比不声明更糟：用户会以为功能可用，点下去才发现不行。
  //
  // `FilterIPv4` / `FilterIPv6` 在 S2.4 落地：规则能翻成过滤器并用事务提交、
  // 能按规则撤销、重复下发幂等，都已实测（tmp/drill-s2.4.cpp）。
  // 这两个能力位说的是**下发**，至于「封得住」由阶段二的 S2.5 与 S2.6 分别验收。
  //
  // `OrphanCleanup` 其实 S1.7 就实现了（启动时清掉上次运行残留的自家过滤器，
  // 三种终止方式都演练过），一直漏在声明之外 —— 漏声明的后果是界面上
  // 那一栏显示成「做不到」，而它明明做得到。
  // `KillTcpV4` 在 S2.8 落地：删除传输控制块（`SetTcpEntry`）能立刻断掉已建立的 IPv4 TCP。
  // `KillTcpV6` **不声明** —— 本平台没有等价接口，而且不允许用户态发原始 TCP 报文，
  // 连自己构造一个 RST 都做不到。声明了却做不到比不声明更糟：
  // 用户会以为 IPv6 连接也能断，点下去才发现不行。
  //
  // `ProcessEnumeration` 在 S3.1 落地：端点表给出 PID，再由 `QueryFullProcessImageNameW`
  // 解出可执行文件路径。⚠️ 未提权时解不出来的是**大多数**（实测本机 52 个 PID 只有 20 个能解），
  // 但能力位的语义是「这个平台提供这项能力」而不是「每一行都解得出来」：
  // 解不出的行会如实留空，界面显示「读不到程序信息」，不冒名字。
  //
  // `EventDrivenConnections` **不声明**（S3.3 才做）。
  CapabilitySet declared;
  declared.add(Capability::FilterIPv4);
  declared.add(Capability::FilterIPv6);
  declared.add(Capability::KillTcpV4);
  declared.add(Capability::OrphanCleanup);
  declared.add(Capability::ProcessEnumeration);
  declared.add(Capability::Elevation);
  declared.add(Capability::SingleInstance);

  auto capabilities = std::make_unique<DeclaredCapabilities>(backend.name, declared);

  backend.capabilities = std::move(capabilities);
  backend.privilege = std::make_unique<WinPrivilege>();
  backend.filterEngine = std::make_unique<WinFilterEngine>();
  backend.killer = std::make_unique<WinKiller>();
  backend.paths = std::make_unique<WinPaths>();
  backend.connMonitor = std::make_unique<WinConnMonitor>();

  const QString name =
      singleInstanceName.isEmpty() ? QString::fromLatin1(kSingleInstanceName) : singleInstanceName;
  backend.singleInstance = std::make_unique<WinSingleInstance>(name);

  if (!backend.isComplete()) {
    return Result<PlatformBackend>::fail(
        makeError(ErrorCode::Internal, QStringLiteral("Windows 后端装配不完整：有成员没被填上")));
  }

  return Result<PlatformBackend>::ok(std::move(backend));
}

}  // namespace baniphelper::core
