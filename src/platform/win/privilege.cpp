#include "platform/win/privilege.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <shellapi.h>

#include <string>
#include <vector>

namespace baniphelper::core {
namespace {

/// 取本进程可执行文件的全路径。
///
/// 不用 Qt 的 `QCoreApplication::applicationFilePath()`：本文件属于平台层，
/// 让它依赖「QCoreApplication 已经构造好」这件事没有必要，而 Win32 自己就能答。
QString selfExecutablePath() {
  std::vector<wchar_t> buffer(MAX_PATH, L'\0');
  for (;;) {
    const DWORD length =
        ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0) {
      return QString();
    }
    // 返回值等于缓冲区容量意味着可能被截断，扩一倍重试。
    if (length < buffer.size() - 1) {
      return QString::fromWCharArray(buffer.data(), static_cast<int>(length));
    }
    buffer.resize(buffer.size() * 2);
  }
}

}  // namespace

Result<bool> WinPrivilege::isElevated() const {
  HANDLE token = nullptr;
  if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token) == FALSE) {
    const DWORD error = ::GetLastError();
    return Result<bool>::fail(makeError(ErrorCode::Platform,
                                        QStringLiteral("打不开本进程的访问令牌，无法判断提权状态"),
                                        static_cast<std::int32_t>(error),
                                        QStringLiteral("OpenProcessToken")));
  }

  TOKEN_ELEVATION elevation{};
  DWORD returned = 0;
  const BOOL ok =
      ::GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &returned);
  const DWORD error = ok ? ERROR_SUCCESS : ::GetLastError();
  ::CloseHandle(token);

  if (ok == FALSE) {
    return Result<bool>::fail(makeError(ErrorCode::Platform,
                                        QStringLiteral("读取令牌的提权状态失败"),
                                        static_cast<std::int32_t>(error),
                                        QStringLiteral("GetTokenInformation")));
  }

  return Result<bool>::ok(elevation.TokenIsElevated != 0);
}

Result<void> WinPrivilege::relaunchElevated() {
  const Result<bool> elevated = isElevated();
  if (!elevated) {
    return Result<void>::fail(elevated.error());
  }

  if (elevated.value()) {
    // 已经提权却仍然去拉起新实例，会留下两个进程各自下发一套过滤器。
    return Result<void>::fail(makeError(
        ErrorCode::AlreadyExists, QStringLiteral("当前进程已经是提权状态，不需要再拉起一个自己")));
  }

  const QString path = selfExecutablePath();
  if (path.isEmpty()) {
    return Result<void>::fail(
        makeError(ErrorCode::Io, QStringLiteral("取不到本进程的可执行文件路径，无法提权重启")));
  }

  const std::wstring file = path.toStdWString();

  SHELLEXECUTEINFOW info{};
  info.cbSize = sizeof(info);
  info.fMask = SEE_MASK_NOCLOSEPROCESS;
  info.lpVerb = L"runas";
  info.lpFile = file.c_str();
  info.nShow = SW_SHOWNORMAL;

  if (::ShellExecuteExW(&info) == FALSE) {
    const DWORD error = ::GetLastError();

    if (error == ERROR_CANCELLED) {
      // 用户点了「否」。这不是程序缺陷，因此单独给一个能读懂的原因，
      // 而不是丢一句「操作失败」。
      return Result<void>::fail(makeError(ErrorCode::NotPermitted,
                                          QStringLiteral("提权确认被取消，封禁与断连功能将不可用"),
                                          static_cast<std::int32_t>(error),
                                          QStringLiteral("ShellExecuteExW")));
    }

    return Result<void>::fail(makeError(ErrorCode::Platform,
                                        QStringLiteral("以提权方式重新启动自身失败"),
                                        static_cast<std::int32_t>(error),
                                        QStringLiteral("ShellExecuteExW")));
  }

  if (info.hProcess != nullptr) {
    // 新实例已经起来了，句柄留着没用。注意这里不等待它结束：
    // 本进程随后会自行退出，等待只会拖慢启动。
    ::CloseHandle(info.hProcess);
  }

  return Result<void>::ok();
}

QString WinPrivilege::elevationRequirementText() const {
  return QStringLiteral(
      "未提权时无法下发封禁规则、无法中断已建立的连接，"
      "也无法清理上次残留的过滤器，只能查看界面。");
}

}  // namespace baniphelper::core
