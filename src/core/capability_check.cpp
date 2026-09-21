#include "core/capability_check.h"

namespace baniphelper::core {

Result<void> requireCapability(const ICapabilities& capabilities,
                               Capability capability,
                               const QString& operation) {
  if (capabilities.isSupported(capability)) {
    return Result<void>::ok();
  }

  QString reason = capabilities.unsupportedReason(capability);
  if (reason.trimmed().isEmpty()) {
    // 实现没给原因，也不能让失败说明留空：补上该能力通用的后果说明。
    // 这一手是「无静默失败路径」的最后一道保险，它不依赖任何实现自觉。
    reason = capabilityImpact(capability);
  }

  return Result<void>::fail(unsupportedError(operation, reason));
}

}  // namespace baniphelper::core
