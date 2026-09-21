#include "platform/win/paths.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <shlobj.h>

#include <QDir>
#include <QString>

namespace baniphelper::core {
namespace {

constexpr wchar_t kAppFolderName[] = L"BanIPHelper";

/// 取 `%APPDATA%`（漫游应用数据目录）。
///
/// 为什么用 `SHGetFolderPathW` 而不是更新的 `SHGetKnownFolderPath`：
/// 后者要 `FOLDERID_RoamingAppData` 这个 GUID 符号，而 MinGW 的头只给声明、
/// 不带定义，得额外引一个库进去。为一个目录路径多挂一个依赖不划算，
/// 与 `kAuthnWinnt` 那个取舍同一个道理。
///
/// 取不到时退回环境变量 `APPDATA`，两条路都失败才算失败，并在错误信息里写明。
Result<QString> roamingAppData() {
  wchar_t buffer[MAX_PATH] = {};
  const HRESULT hr =
      ::SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, buffer);
  if (SUCCEEDED(hr) && buffer[0] != L'\0') {
    return Result<QString>::ok(QString::fromWCharArray(buffer));
  }

  const QString fromEnvironment = qEnvironmentVariable("APPDATA");
  if (!fromEnvironment.isEmpty()) {
    return Result<QString>::ok(fromEnvironment);
  }

  return Result<QString>::fail(makeError(
      ErrorCode::NotFound,
      QStringLiteral(
          "取不到漫游应用数据目录：SHGetFolderPathW 失败（0x%1），环境变量 APPDATA 也为空")
          .arg(static_cast<quint32>(hr), 8, 16, QLatin1Char('0'))));
}

/// 本工具的数据根目录 `%APPDATA%\BanIPHelper`。
Result<QDir> applicationRoot() {
  const Result<QString> base = roamingAppData();
  if (!base) {
    return Result<QDir>::fail(base.error());
  }
  return Result<QDir>::ok(
      QDir(base.value() + QLatin1Char('\\') + QString::fromWCharArray(kAppFolderName)));
}

}  // namespace

Result<Paths> WinPaths::resolve() const {
  const Result<QDir> root = applicationRoot();
  if (!root) {
    return Result<Paths>::fail(root.error());
  }

  Paths paths;
  paths.configFile = root.value().absoluteFilePath(QStringLiteral("config.json"));
  paths.databaseFile = root.value().absoluteFilePath(QStringLiteral("baniphelper.db"));
  paths.logDirectory = root.value().absoluteFilePath(QStringLiteral("logs"));
  return Result<Paths>::ok(paths);
}

Result<void> WinPaths::ensureDirectories() {
  const Result<QDir> root = applicationRoot();
  if (!root) {
    return Result<void>::fail(root.error());
  }

  if (!root.value().exists() && !root.value().mkpath(QStringLiteral("."))) {
    return Result<void>::fail(makeError(
        ErrorCode::Io, QStringLiteral("无法创建应用数据目录：") + root.value().absolutePath()));
  }

  const QDir logs(root.value().absoluteFilePath(QStringLiteral("logs")));
  if (!logs.exists() && !logs.mkpath(QStringLiteral("."))) {
    return Result<void>::fail(
        makeError(ErrorCode::Io, QStringLiteral("无法创建日志目录：") + logs.absolutePath()));
  }

  return Result<void>::ok();
}

}  // namespace baniphelper::core
