#include "core/platform_backend.h"

#include <memory>

#include "core/declared_capabilities.h"
#include "platform/win/filter_engine.h"
#include "platform/win/privilege.h"
#include "platform/win/single_instance.h"

namespace baniphelper::core {

Result<PlatformBackend> createPlatformBackend(const QString& singleInstanceName) {
  PlatformBackend backend;
  backend.name = QStringLiteral("win");

  // 只声明已经真正实现的能力。还没做的一律不写进来，界面据此显式禁用。
  // 声明了却做不到，比不声明更糟：用户会以为功能可用，点下去才发现不行。
  CapabilitySet declared;
  declared.add(Capability::Elevation);
  declared.add(Capability::SingleInstance);

  auto capabilities = std::make_unique<DeclaredCapabilities>(backend.name, declared);

  // 尚未实现的能力给出具体原因，而不是只退回到通用的后果说明：
  // 「阶段二 S2.4」比「无法阻断通信」更能告诉使用者现在处在哪一步。
  capabilities->setUnsupportedReason(Capability::FilterIPv4,
                                     QStringLiteral("过滤器引擎的下发能力尚未实现，阶段二 S2.4"));
  capabilities->setUnsupportedReason(Capability::FilterIPv6,
                                     QStringLiteral("过滤器引擎的下发能力尚未实现，阶段二 S2.4"));

  backend.capabilities = std::move(capabilities);
  backend.privilege = std::make_unique<WinPrivilege>();
  backend.filterEngine = std::make_unique<WinFilterEngine>();

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
