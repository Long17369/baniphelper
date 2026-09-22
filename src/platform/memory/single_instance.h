#pragma once

#include <QString>

#include "core/result.h"
#include "platform/api/isingleinstance.h"

namespace baniphelper::core {

/// 单实例的内存实现。
///
/// 用途有二：给契约测试提供一个行为完全可控的对照物；
/// 以及在还没有真实后端的平台上让上层先跑起来（阶段八 S8.1 的铺路）。
///
/// 语义与真实实现保持一致：同一个名字第二次取得所有权会返回「已有实例」，
/// 而不是报成错误；唤起请求只有接收端已经建好时才能送达，
/// 而且同样经由 `runOnMainThread` 投递，不因为它是内存实现就当场同步回调。
class MemorySingleInstance final : public ISingleInstance {
 public:
  explicit MemorySingleInstance(QString name);
  ~MemorySingleInstance() override;

  [[nodiscard]] Result<bool> acquire() override;
  [[nodiscard]] Result<void> signalExisting() override;
  [[nodiscard]] Result<void> listenForActivation(ActivationHandler handler) override;
  void stopListening() noexcept override;
  void release() noexcept override;

 private:
  QString name_;
  bool held_ = false;
  bool listening_ = false;
};

}  // namespace baniphelper::core
