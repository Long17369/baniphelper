#include "core/declared_capabilities.h"

#include <utility>

namespace baniphelper::core {

DeclaredCapabilities::DeclaredCapabilities(QString backendName, CapabilitySet declared)
    : backendName_(std::move(backendName)), declared_(declared) {}

bool DeclaredCapabilities::isSupported(Capability capability) const {
  return declared_.contains(capability);
}

QList<Capability> DeclaredCapabilities::supported() const {
  return declared_.list();
}

QString DeclaredCapabilities::unsupportedReason(Capability capability) const {
  if (declared_.contains(capability)) {
    // 已支持的能力没有「不支持原因」。返回空是这里唯一允许出现空字符串的场合，
    // 契约测试也会检查这一点。
    return QString();
  }

  const auto it = unsupportedReasons_.constFind(capability);
  if (it != unsupportedReasons_.constEnd() && !it.value().trimmed().isEmpty()) {
    return it.value();
  }

  // 没登记具体原因也必须给出一句说明：界面禁用一个功能却不解释为什么，
  // 用户既不知道少了什么也不知道严重性，那与「静默失败」没有区别。
  return capabilityImpact(capability);
}

QString DeclaredCapabilities::backendName() const {
  return backendName_;
}

void DeclaredCapabilities::setUnsupportedReason(Capability capability, QString reason) {
  unsupportedReasons_.insert(capability, std::move(reason));
}

CapabilitySet DeclaredCapabilities::declared() const {
  return declared_;
}

QList<UnsupportedCapability> DeclaredCapabilities::unsupportedWithReasons() const {
  QList<UnsupportedCapability> result;
  for (Capability capability : kAllCapabilities) {
    if (!declared_.contains(capability)) {
      result.append(UnsupportedCapability{capability, unsupportedReason(capability)});
    }
  }
  return result;
}

}  // namespace baniphelper::core
