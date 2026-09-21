#include "platform/win/single_instance.h"

#include <windows.h>

#include <utility>

namespace baniphelper::core {
namespace {

/// `Local\` 限本机会话。不用 `Global\`：本程序是单用户桌面工具，
/// 跨会话互斥只会让「另一个登录会话里也开着」变成互相挤掉，没有意义。
constexpr wchar_t kMutexPrefix[] = L"Local\\";

constexpr wchar_t kPipePrefix[] = L"\\\\.\\pipe\\";

/// 唤起消息的内容。管道协议由托盘侧（阶段一 S1.8）实现接收端，
/// 这里只约定「发一个固定字符串即表示请把界面显示出来」。
constexpr char kSignalPayload[] = "show";

}  // namespace

WinSingleInstance::WinSingleInstance(QString name) : name_(std::move(name)) {}

WinSingleInstance::~WinSingleInstance() {
  release();
}

QString WinSingleInstance::mutexName() const {
  return QString::fromWCharArray(kMutexPrefix) + name_;
}

QString WinSingleInstance::pipeName() const {
  return QString::fromWCharArray(kPipePrefix) + name_;
}

Result<bool> WinSingleInstance::acquire() {
  if (held_) {
    return Result<bool>::ok(true);
  }

  const std::wstring name = mutexName().toStdWString();

  // 第二个参数为 FALSE：拿到句柄不等于取得所有权，这里要的只是「这个对象是不是
  // 刚刚才被创建出来的」这个信息，靠 GetLastError 区分。
  ::SetLastError(ERROR_SUCCESS);
  HANDLE handle = ::CreateMutexW(nullptr, FALSE, name.c_str());
  if (handle == nullptr) {
    const DWORD error = ::GetLastError();
    return Result<bool>::fail(makeError(ErrorCode::Platform,
                                        QStringLiteral("创建单实例互斥体失败"),
                                        static_cast<std::int32_t>(error),
                                        QStringLiteral("CreateMutexW")));
  }

  if (::GetLastError() == ERROR_ALREADY_EXISTS) {
    // 已经有人在跑。我们并不需要这个句柄，立刻关掉，
    // 免得把它保留到进程退出、把它的生命周期拖长。
    ::CloseHandle(handle);
    return Result<bool>::ok(false);
  }

  mutex_ = handle;
  held_ = true;
  return Result<bool>::ok(true);
}

Result<void> WinSingleInstance::signalExisting() {
  const std::wstring name = pipeName().toStdWString();

  HANDLE pipe = ::CreateFileW(name.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
  if (pipe == INVALID_HANDLE_VALUE) {
    const DWORD error = ::GetLastError();

    // 接收端由托盘侧建立（阶段一 S1.8）。在它建好之前，这里必须报错而不是
    // 静默成功：静默成功会让本进程直接退出，用户看到的却是新旧两个窗口都不见了。
    return Result<void>::fail(
        makeError(ErrorCode::NotFound,
                  QStringLiteral("既有实例没有在监听唤起通道，无法唤起它的界面"),
                  static_cast<std::int32_t>(error),
                  QStringLiteral("CreateFileW")));
  }

  DWORD written = 0;
  const BOOL ok = ::WriteFile(
      pipe, kSignalPayload, static_cast<DWORD>(sizeof(kSignalPayload) - 1), &written, nullptr);
  const DWORD error = ok ? ERROR_SUCCESS : ::GetLastError();
  ::CloseHandle(pipe);

  if (ok == FALSE) {
    return Result<void>::fail(makeError(ErrorCode::Io,
                                        QStringLiteral("向既有实例发送唤起请求失败"),
                                        static_cast<std::int32_t>(error),
                                        QStringLiteral("WriteFile")));
  }

  return Result<void>::ok();
}

void WinSingleInstance::release() noexcept {
  if (mutex_ != nullptr) {
    ::CloseHandle(static_cast<HANDLE>(mutex_));
    mutex_ = nullptr;
  }
  held_ = false;
}

}  // namespace baniphelper::core
