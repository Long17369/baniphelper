#include "platform/memory/single_instance.h"

#include <QHash>
#include <QSet>

#include <utility>

#include "core/main_thread_dispatch.h"

namespace baniphelper::core {
namespace {

/// 进程内已被占用的实例名。
///
/// 用函数内静态量而不是命名空间级全局量：既避开静态初始化顺序问题，
/// 也保证只有真正用到内存后端时才构造它。
QSet<QString>& heldNames() {
  static QSet<QString> names;
  return names;
}

/// 进程内已建立的接收端：实例名 → 处理函数。
QHash<QString, ActivationHandler>& listeners() {
  static QHash<QString, ActivationHandler> map;
  return map;
}

}  // namespace

MemorySingleInstance::MemorySingleInstance(QString name) : name_(std::move(name)) {}

MemorySingleInstance::~MemorySingleInstance() {
  // 不停掉监听的话，处理函数会一直留在全局表里，
  // 下一个用同名实例名的测试会拿到一个已经析构的对象的回调。
  stopListening();
}

Result<bool> MemorySingleInstance::acquire() {
  if (held_) {
    // 同一个对象重复取得所有权是幂等的，与真实实现一致。
    return Result<bool>::ok(true);
  }

  if (heldNames().contains(name_)) {
    // 「已有实例在进行中」不是错误，调用方据此走「唤起既有实例然后退出」的路径。
    return Result<bool>::ok(false);
  }

  heldNames().insert(name_);
  held_ = true;
  return Result<bool>::ok(true);
}

Result<void> MemorySingleInstance::signalExisting() {
  const ActivationHandler handler = listeners().value(name_);
  if (!handler) {
    // 没有接收端却报成功，就是静默失败：调用方会以为唤起了界面然后自己退出，
    // 结果两个窗口都不见了。真实实现在这种情况下的 CreateFileW 也会失败。
    return Result<void>::fail(makeError(
        ErrorCode::NotFound, QStringLiteral("既有实例没有在监听唤起通道：%1").arg(name_)));
  }

  runOnMainThread(handler);
  return Result<void>::ok();
}

Result<void> MemorySingleInstance::listenForActivation(ActivationHandler handler) {
  if (!handler) {
    return Result<void>::fail(
        makeError(ErrorCode::InvalidArgument, QStringLiteral("唤起处理函数为空，无法建立接收端")));
  }

  // 后注册的替换先前的，重复调用安全。
  listeners().insert(name_, std::move(handler));
  listening_ = true;
  return Result<void>::ok();
}

void MemorySingleInstance::stopListening() noexcept {
  if (!listening_) {
    // 重复停止是安全的，退出路径可能被走两次。
    return;
  }

  listeners().remove(name_);
  listening_ = false;
}

void MemorySingleInstance::release() noexcept {
  if (!held_) {
    // 重复释放是安全的，退出路径可能被走两次。
    return;
  }

  heldNames().remove(name_);
  held_ = false;
}

}  // namespace baniphelper::core
