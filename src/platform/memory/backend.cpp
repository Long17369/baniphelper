#include "platform/memory/backend.h"

#include <memory>

#include "core/declared_capabilities.h"
#include "platform/memory/privilege.h"
#include "platform/memory/single_instance.h"

namespace baniphelper::core {

PlatformBackend makeMemoryBackend(const QString& singleInstanceName) {
  PlatformBackend backend;
  backend.name = QStringLiteral("memory");

  // 内存后端只声明它真的模拟了的能力，不做「看起来什么都行」的假声明。
  CapabilitySet declared;
  declared.add(Capability::Elevation);
  declared.add(Capability::SingleInstance);

  backend.capabilities = std::make_unique<DeclaredCapabilities>(backend.name, declared);
  backend.privilege = std::make_unique<MemoryPrivilege>();

  const QString name =
      singleInstanceName.isEmpty() ? QString::fromLatin1(kSingleInstanceName) : singleInstanceName;
  backend.singleInstance = std::make_unique<MemorySingleInstance>(name);

  return backend;
}

}  // namespace baniphelper::core
