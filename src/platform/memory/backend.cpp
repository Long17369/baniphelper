#include "platform/memory/backend.h"

#include <memory>

#include "core/declared_capabilities.h"
#include "platform/memory/conn_monitor.h"
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
  // `EventDrivenConnections` 同理：`MemoryConnMonitor` 真的把投进去的事件送到订阅者手上，
  // 因此「订上了就真收得到」这条契约能在不需要 ETW、也不需要提权的条件下被验到底。
  CapabilitySet declared;
  declared.add(Capability::KillTcpV4);
  declared.add(Capability::EventDrivenConnections);
  declared.add(Capability::Elevation);
  declared.add(Capability::SingleInstance);

  auto capabilities = std::make_unique<DeclaredCapabilities>(backend.name, declared);

  // 与真实后端保持同一套说法：两个后端在界面上的表现必须一致，
  // 否则「用内存后端先做界面」就失去了意义。
  capabilities->setUnsupportedReason(Capability::FilterIPv4,
                                     QStringLiteral("过滤器引擎的下发能力尚未实现，阶段二 S2.4"));
  capabilities->setUnsupportedReason(Capability::FilterIPv6,
                                     QStringLiteral("过滤器引擎的下发能力尚未实现，阶段二 S2.4"));
  // 内存后端枚举不了系统里的进程，所以**不声明** ProcessEnumeration。
  // 它照样能回放预置的连接快照，但那是「替身能跑」，不是「这个后端做得到这件事」。
  capabilities->setUnsupportedReason(
      Capability::ProcessEnumeration,
      QStringLiteral("内存后端只回放预置的快照，不枚举系统里的进程；"
                     "它的用途是当契约测试的对照物与界面开发期的替身"));

  backend.capabilities = std::move(capabilities);
  backend.privilege = std::make_unique<MemoryPrivilege>();
  backend.filterEngine = std::make_unique<MemoryFilterEngine>();
  backend.killer = std::make_unique<MemoryKiller>();
  backend.paths = std::make_unique<MemoryPaths>();
  backend.connMonitor = std::make_unique<MemoryConnMonitor>();

  const QString name =
      singleInstanceName.isEmpty() ? QString::fromLatin1(kSingleInstanceName) : singleInstanceName;
  backend.singleInstance = std::make_unique<MemorySingleInstance>(name);

  return backend;
}

}  // namespace baniphelper::core
