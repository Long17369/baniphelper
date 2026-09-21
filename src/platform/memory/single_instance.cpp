#include "platform/memory/single_instance.h"

#include <QSet>

#include <utility>

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

}  // namespace

MemorySingleInstance::MemorySingleInstance(QString name) : name_(std::move(name)) {}

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
  if (!held_ && heldNames().contains(name_)) {
    return Result<void>::ok();
  }

  // 没有既有实例却报成功，就是静默失败：调用方会以为唤起了界面然后自己退出，
  // 结果两个窗口都不见了。
  return Result<void>::fail(
      makeError(ErrorCode::NotFound, QStringLiteral("没有可唤起的既有实例：%1").arg(name_)));
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
