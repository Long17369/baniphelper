#include "core/platform_backend.h"

#include <memory>

#include "core/declared_capabilities.h"
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

  backend.capabilities = std::make_unique<DeclaredCapabilities>(backend.name, declared);
  backend.privilege = std::make_unique<WinPrivilege>();

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
