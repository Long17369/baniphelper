#include "platform/memory/paths.h"

#include <QDir>
#include <QString>

namespace baniphelper::core {
namespace {

/// 内存后端的数据根目录。名字里带 backend 是为了不和真实后端的目录混淆。
QDir applicationRoot() {
  return QDir(QDir::temp().absoluteFilePath(QStringLiteral("baniphelper-memory-backend")));
}

}  // namespace

Result<Paths> MemoryPaths::resolve() const {
  const QDir root = applicationRoot();

  Paths paths;
  paths.configFile = root.absoluteFilePath(QStringLiteral("config.json"));
  paths.databaseFile = root.absoluteFilePath(QStringLiteral("baniphelper.db"));
  paths.logDirectory = root.absoluteFilePath(QStringLiteral("logs"));
  return Result<Paths>::ok(paths);
}

Result<void> MemoryPaths::ensureDirectories() {
  const QDir root = applicationRoot();
  if (!root.exists() && !root.mkpath(QStringLiteral("."))) {
    return Result<void>::fail(makeError(
        ErrorCode::Io, QStringLiteral("无法创建内存后端数据目录：") + root.absolutePath()));
  }

  const QDir logs(root.absoluteFilePath(QStringLiteral("logs")));
  if (!logs.exists() && !logs.mkpath(QStringLiteral("."))) {
    return Result<void>::fail(makeError(
        ErrorCode::Io, QStringLiteral("无法创建内存后端日志目录：") + logs.absolutePath()));
  }

  return Result<void>::ok();
}

}  // namespace baniphelper::core
