#include "platform/memory/backend.h"

#include <memory>

#include "core/declared_capabilities.h"
#include "platform/memory/filter_engine.h"
#include "platform/memory/killer.h"
#include "platform/memory/paths.h"
#include "platform/memory/privilege.h"
#include "platform/memory/single_instance.h"

namespace baniphelper::core {

PlatformBackend makeMemoryBackend(const QString& singleInstanceName) {
  PlatformBackend backend;
  backend.name = QStringLiteral("memory");

  // 内存后端只声明它真的模拟了的能力，不做「看起来什么都行」的假声明。
  // `KillTcpV4` 在这里是**真的模拟了**：`MemoryKiller` 会记账并给出与真实后端
  // 同一套「能不能断」的答案（UDP 不行、IPv6 不行），所以如实声明。
  CapabilitySet declared;
  declared.add(Capability::KillTcpV4);
  declared.add(Capability::Elevation);
  declared.add(Capability::SingleInstance);

  auto capabilities = std::make_unique<DeclaredCapabilities>(backend.name, declared);

  // 与真实后端保持同一套说法：两个后端在界面上的表现必须一致，
  // 否则「用内存后端先做界面」就失去了意义。
  capabilities->setUnsupportedReason(Capability::FilterIPv4,
                                     QStringLiteral("过滤器引擎的下发能力尚未实现，阶段二 S2.4"));
  capabilities->setUnsupportedReason(Capability::FilterIPv6,
                                     QStringLiteral("过滤器引擎的下发能力尚未实现，阶段二 S2.4"));

  backend.capabilities = std::move(capabilities);
  backend.privilege = std::make_unique<MemoryPrivilege>();
  backend.filterEngine = std::make_unique<MemoryFilterEngine>();
  backend.killer = std::make_unique<MemoryKiller>();
  backend.paths = std::make_unique<MemoryPaths>();

  const QString name =
      singleInstanceName.isEmpty() ? QString::fromLatin1(kSingleInstanceName) : singleInstanceName;
  backend.singleInstance = std::make_unique<MemorySingleInstance>(name);

  return backend;
}

}  // namespace baniphelper::core
