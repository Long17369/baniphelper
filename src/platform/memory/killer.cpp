// 连接中断的内存实现（阶段二 S2.8）。
//
// 不做真的断连，只记账。判据与真实后端一致 —— 理由写在头文件里。

#include "platform/memory/killer.h"

#include <QString>

#include "core/error.h"
#include "core/types.h"

namespace baniphelper::core {
namespace {

/// 与真实后端同一套判据。`ok` 为假时 `reason` 必须非空。
struct Killability {
  bool ok = false;
  Error reason;
};

Killability judge(const ConnectionKey& connection) {
  if (connection.protocol != TransportProtocol::Tcp) {
    return Killability{false,
                       unsupportedError(QStringLiteral("中断 UDP 的「连接」"),
                                        QStringLiteral("UDP 没有建立与拆除这一步，也就没有可删的"
                                                       "传输控制块"))};
  }

  const AddressFamily family = connection.remote.address.family;
  if (family != connection.local.address.family) {
    return Killability{
        false,
        makeError(ErrorCode::InvalidArgument,
                  QStringLiteral("连接两端的地址族不一致（本端 %1、对端 %2），"
                                 "这不可能是同一条连接")
                      .arg(connection.local.address.text, connection.remote.address.text))};
  }

  if (family == AddressFamily::V6) {
    return Killability{false,
                       unsupportedError(QStringLiteral("中断 IPv6 连接"),
                                        QStringLiteral("本平台没有等价于删除传输控制块的 IPv6 "
                                                       "接口。封禁对后续连接仍然有效，"
                                                       "但已建立的那条只能等它自己结束"))};
  }

  if (connection.local.address.text.trimmed().isEmpty() ||
      connection.remote.address.text.trimmed().isEmpty()) {
    return Killability{
        false, makeError(ErrorCode::InvalidArgument, QStringLiteral("连接两端的地址不能为空"))};
  }

  if (connection.local.port == 0 || connection.remote.port == 0) {
    return Killability{false,
                       makeError(ErrorCode::InvalidArgument,
                                 QStringLiteral("连接两端的端口都必须非零（本端 %1、"
                                                "对端 %2）")
                                     .arg(connection.local.port)
                                     .arg(connection.remote.port))};
  }

  return Killability{true, Error{}};
}

}  // namespace

MemoryKiller::MemoryKiller() = default;
MemoryKiller::~MemoryKiller() = default;

Result<bool> MemoryKiller::canKill(const ConnectionKey& connection) const {
  const Killability verdict = judge(connection);
  if (verdict.ok) {
    return Result<bool>::ok(true);
  }
  if (verdict.reason.code == ErrorCode::NotSupported) {
    return Result<bool>::ok(false);
  }
  return Result<bool>::fail(verdict.reason);
}

Result<void> MemoryKiller::kill(const ConnectionKey& connection) {
  const Killability verdict = judge(connection);
  if (!verdict.ok) {
    return Result<void>::fail(verdict.reason);
  }
  killed_.append(connection);
  return Result<void>::ok();
}

Result<KillReport> MemoryKiller::killMany(const QList<ConnectionKey>& connections) {
  KillReport report;
  report.requested = static_cast<int>(connections.size());

  for (const ConnectionKey& connection : connections) {
    const Result<void> outcome = kill(connection);
    if (outcome) {
      ++report.killed;
      continue;
    }
    KillFailure failure;
    failure.connection = connection;
    failure.reason = outcome.error().message;
    report.failures.append(failure);
  }

  return Result<KillReport>::ok(report);
}

void MemoryKiller::clearKilled() {
  killed_.clear();
}

}  // namespace baniphelper::core
