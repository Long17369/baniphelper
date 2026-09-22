#include "core/platform_backend.h"

#include <memory>

#include "core/declared_capabilities.h"
#include "platform/win/filter_engine.h"
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
  CapabilitySet declared;
  declared.add(Capability::FilterIPv4);
  declared.add(Capability::FilterIPv6);
  declared.add(Capability::OrphanCleanup);
  declared.add(Capability::Elevation);
  declared.add(Capability::SingleInstance);

  auto capabilities = std::make_unique<DeclaredCapabilities>(backend.name, declared);

  backend.capabilities = std::move(capabilities);
  backend.privilege = std::make_unique<WinPrivilege>();
  backend.filterEngine = std::make_unique<WinFilterEngine>();
  backend.paths = std::make_unique<WinPaths>();

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
