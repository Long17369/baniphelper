#include "platform/win/single_instance.h"

#include <windows.h>

#include <QString>

#include <string>
#include <utility>

#include "core/log.h"
#include "core/main_thread_dispatch.h"

namespace baniphelper::core {
namespace {

/// `Local\` 限本机会话。不用 `Global\`：本程序是单用户桌面工具，
/// 跳会话互斥只会让「另一个登录会话里也开着」变成互相挤掉，没有意义。
constexpr wchar_t kMutexPrefix[] = L"Local\\";

constexpr wchar_t kPipePrefix[] = L"\\\\.\\pipe\\";

/// 唤起消息的内容。发一个固定字符串即表示「请把界面显示出来」，
/// 不需要报文格式：一次连接就是一次请求。
constexpr char kSignalPayload[] = "show";

/// 一次请求最多读这么多字节，够放下 `show` 与手工敲的变体。
constexpr DWORD kReadBufferSize = 64;

HANDLE toHandle(void* pointer) {
  return static_cast<HANDLE>(pointer);
}

/// 报文是否就是「把界面拿出来」。
///
/// 大小写与首尾空白都不计较：往管道里写东西的不只是我们自己的客户端，
/// 排查时用 `echo show > \\.\pipe\<名字>` 手工试一次也能用，那很方便。
bool isShowPayload(const char* data, DWORD size) {
  const QString text = QString::fromLatin1(data, static_cast<int>(size)).trimmed();
  return text.compare(QStringLiteral("show"), Qt::CaseInsensitive) == 0;
}

}  // namespace

WinSingleInstance::WinSingleInstance(QString name) : name_(std::move(name)) {}

WinSingleInstance::~WinSingleInstance() {
  stopListening();
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

    // 打不开只有两种可能：根本没有既有实例，或者它虽然持有所有权但没开接收端。
    // 两者都必须报错，不允许静默成功 —— 静默成功会让本进程直接退出，
    // 用户看到的却是新旧两个窗口都不见了。
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

Result<void> WinSingleInstance::listenForActivation(ActivationHandler handler) {
  if (!handler) {
    // 空处理函数会把接收端变成一个「收了请求但什么也不做」的黑洞，
    // 而接口又是拿 Result 报错的，那就当场拒掉。
    return Result<void>::fail(
        makeError(ErrorCode::InvalidArgument, QStringLiteral("唤起处理函数为空，无法建立接收端")));
  }

  {
    QMutexLocker locker(&handlerMutex_);
    handler_ = std::move(handler);
  }

  if (listening_) {
    // 通道已建好，换个处理函数就行，不重开线程。
    return Result<void>::ok();
  }

  // 第一个管道实例在这里同步建好，**不能**交给线程去建：
  // 接口承诺返回成功时通道已经可用，而线程要等调度才有机会跑。
  // 少了这一步，紧接着的二次启动会撞在「线程还没来得及建管道」的窗口上，
  // 报一个假的「既有实例没有在监听唤起通道」—— 那是个真正会让人查错方向的坑。
  void* pipe = createPipeInstance();
  if (pipe == nullptr) {
    return Result<void>::fail(
        makeError(ErrorCode::Platform, QStringLiteral("建立单实例接收端失败：无法创建命名管道")));
  }

  stopEvent_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  ioEvent_ = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (stopEvent_ == nullptr || ioEvent_ == nullptr) {
    const DWORD error = ::GetLastError();
    ::CloseHandle(toHandle(pipe));
    stopListening();
    return Result<void>::fail(makeError(ErrorCode::Platform,
                                        QStringLiteral("创建单实例监听事件失败"),
                                        static_cast<std::int32_t>(error),
                                        QStringLiteral("CreateEventW")));
  }

  pendingPipe_ = pipe;
  listening_ = true;
  listener_ = std::thread([this]() { listenLoop(); });
  return Result<void>::ok();
}

void* WinSingleInstance::createPipeInstance() const {
  const std::wstring pipePath = pipeName().toStdWString();

  // 实例数不设上限：限成 1 的话，上一个实例还没关掉时客户端会拿到
  // ERROR_PIPE_BUSY 而失败，而它本来只是想把界面拿出来。
  HANDLE pipe = ::CreateNamedPipeW(
      pipePath.c_str(),
      PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
      PIPE_UNLIMITED_INSTANCES,
      kReadBufferSize,
      kReadBufferSize,
      0,
      nullptr);
  if (pipe == INVALID_HANDLE_VALUE) {
    logWrite(LogLevel::Error,
             QStringLiteral("创建单实例管道实例失败（错误码 %1），之后将无法被唤起")
                 .arg(static_cast<qulonglong>(::GetLastError())));
    return nullptr;
  }

  return pipe;
}

void WinSingleInstance::stopListening() noexcept {
  const HANDLE stopEvent = toHandle(stopEvent_);
  if (stopEvent != nullptr) {
    // 置位就够了：监听线程的每一次等待都包含这个事件。
    ::SetEvent(stopEvent);
  }

  if (listener_.joinable()) {
    listener_.join();
  }

  listening_ = false;

  if (pendingPipe_ != nullptr) {
    // 线程可能还没来得及接手。不关就是漏一个内核句柄。
    ::CloseHandle(toHandle(pendingPipe_));
    pendingPipe_ = nullptr;
  }

  if (stopEvent_ != nullptr) {
    ::CloseHandle(toHandle(stopEvent_));
    stopEvent_ = nullptr;
  }
  if (ioEvent_ != nullptr) {
    ::CloseHandle(toHandle(ioEvent_));
    ioEvent_ = nullptr;
  }

  QMutexLocker locker(&handlerMutex_);
  handler_ = nullptr;
}

void WinSingleInstance::listenLoop() {
  const HANDLE stopEvent = toHandle(stopEvent_);
  const HANDLE ioEvent = toHandle(ioEvent_);

  // 接手调用者刚建好的那个实例。
  HANDLE pipe = toHandle(pendingPipe_);
  pendingPipe_ = nullptr;

  while (pipe != nullptr) {
    // 连接。重叠方式 + 同时等停止信号，这样停下来时不会被一个阻塞的等待挂住。
    OVERLAPPED connectOverlapped{};
    connectOverlapped.hEvent = ioEvent;
    ::ResetEvent(ioEvent);

    const BOOL connected = ::ConnectNamedPipe(pipe, &connectOverlapped);
    if (connected == FALSE) {
      const DWORD error = ::GetLastError();
      if (error == ERROR_IO_PENDING) {
        const HANDLE waits[2] = {stopEvent, ioEvent};
        const DWORD signaled = ::WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (signaled != WAIT_OBJECT_0 + 1) {
          ::CancelIoEx(pipe, &connectOverlapped);
          ::CloseHandle(pipe);
          break;
        }

        DWORD transferred = 0;
        ::GetOverlappedResult(pipe, &connectOverlapped, &transferred, FALSE);
      } else if (error != ERROR_PIPE_CONNECTED && error != ERROR_NO_DATA) {
        // 客户端在 ConnectNamedPipe 之前就已完成连接时给的是 ERROR_PIPE_CONNECTED，
        // 那算成功；**ERROR_NO_DATA 同样算成功** —— 它表示客户端连上之后又立刻关掉了
        // （错误码字面上写着「没有数据」，很容易被误当成失败而把这一轮请求丢掉）。
        // 这恰恰是正常情形：我们的客户端就是写完一个字符串就关。
        // 字节模式管道里已经写进去的数据在客户端关闭后仍然读得出来。
        // 其余才是真的失败：记一笔并退出线程 —— 不重试是因为重试会拿着同一个
        // 已经作废的句柄转圈，而退出至少是诚实的，下次启动会重建接收端。
        logWrite(LogLevel::Warn,
                 QStringLiteral("接收唤起请求时等待连接失败（错误码 %1）")
                     .arg(static_cast<qulonglong>(error)));
        ::CloseHandle(pipe);
        break;
      }
    }

    // 收报文。客户端总是写完就关，读一次就够。
    char buffer[kReadBufferSize] = {};
    DWORD read = 0;

    OVERLAPPED readOverlapped{};
    readOverlapped.hEvent = ioEvent;
    ::ResetEvent(ioEvent);

    const BOOL readOk = ::ReadFile(pipe, buffer, sizeof(buffer) - 1, &read, &readOverlapped);
    if (readOk == FALSE) {
      const DWORD error = ::GetLastError();
      read = 0;
      if (error == ERROR_IO_PENDING) {
        const HANDLE waits[2] = {stopEvent, ioEvent};
        const DWORD signaled = ::WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (signaled == WAIT_OBJECT_0 + 1) {
          DWORD transferred = 0;
          if (::GetOverlappedResult(pipe, &readOverlapped, &transferred, FALSE) != FALSE) {
            read = transferred;
          }
        } else {
          ::CancelIoEx(pipe, &readOverlapped);
        }
      }
    }

    // **先建下一个实例，再关掉当前这个**：两者顺序反过来的话，
    // 中间那一小段时间里通道是不存在的，恰好在这时启动的第二个实例会拿到
    // ERROR_FILE_NOT_FOUND，报成「既有实例没有在监听」。
    HANDLE next = toHandle(createPipeInstance());

    ::DisconnectNamedPipe(pipe);
    ::CloseHandle(pipe);

    // 停止信号置位后不再往下跑：这一轮即使收到了请求也不再投递，
    // 因为那边已经在拆界面了。
    if (::WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) {
      if (next != nullptr) {
        ::CloseHandle(next);
      }
      break;
    }

    if (read > 0 && isShowPayload(buffer, read)) {
      ActivationHandler handler;
      {
        QMutexLocker locker(&handlerMutex_);
        handler = handler_;
      }
      dispatchActivation(handler);
    }

    pipe = next;
    if (pipe == nullptr) {
      break;
    }
  }
}

void WinSingleInstance::dispatchActivation(const ActivationHandler& handler) {
  if (!handler) {
    return;
  }

  // 界面只能在主线程动，而这里大概率是监听线程，因此统一投递过去。
  runOnMainThread(handler);
}

void WinSingleInstance::release() noexcept {
  if (mutex_ != nullptr) {
    ::CloseHandle(static_cast<HANDLE>(mutex_));
    mutex_ = nullptr;
  }
  held_ = false;
}

}  // namespace baniphelper::core
