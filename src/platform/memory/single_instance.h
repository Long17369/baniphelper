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
/// 而不是报成错误。
class MemorySingleInstance final : public ISingleInstance {
 public:
  explicit MemorySingleInstance(QString name);

  [[nodiscard]] Result<bool> acquire() override;
  [[nodiscard]] Result<void> signalExisting() override;
  void release() noexcept override;

 private:
  QString name_;
  bool held_ = false;
};

}  // namespace baniphelper::core
