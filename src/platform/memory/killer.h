#pragma once

#include <QList>

#include "core/result.h"
#include "platform/api/ikiller.h"

namespace baniphelper::core {

/// 连接中断的内存实现。
///
/// 它不真的断任何东西，只**记下**「哪些连接被要求断过」。
/// 存在的理由与内存后端的其他成员一样：界面在没有管理员权限时也要能跑起来，
/// 而且「编排的顺序对不对」这类断言不该依赖真的去断一条生产连接。
///
/// ⚠️ 它对「能不能断」给出的答案必须与真实后端**一致**（UDP 不行、IPv6 不行、
/// 地址族不一致是调用方给错了）。不一致的话，界面在两种后端下的表现就会分叉，
/// 而「先用内存后端把界面做出来」正是它存在的意义。
/// 判据因此在两个实现里各写了一遍 —— 「IPv6 断不了」是**本平台**的事实，
/// 把它抽到公共处会让将来某个能做这件事的平台被迫承接一个错误的答案。
class MemoryKiller final : public IKiller {
 public:
  MemoryKiller();
  ~MemoryKiller() override;

  MemoryKiller(const MemoryKiller&) = delete;
  MemoryKiller& operator=(const MemoryKiller&) = delete;

  [[nodiscard]] Result<bool> canKill(const ConnectionKey& connection) const override;
  [[nodiscard]] Result<void> kill(const ConnectionKey& connection) override;
  [[nodiscard]] Result<KillReport> killMany(const QList<ConnectionKey>& connections) override;

  /// 被要求断过的连接，按先后顺序。供测试核对编排是否真的走到了这一步。
  [[nodiscard]] const QList<ConnectionKey>& killedConnections() const noexcept {
    return killed_;
  }

  void clearKilled();

 private:
  QList<ConnectionKey> killed_;
};

}  // namespace baniphelper::core
