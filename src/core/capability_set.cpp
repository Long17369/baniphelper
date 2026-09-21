#include "core/capability_set.h"

namespace baniphelper::core {

std::uint32_t CapabilitySet::bitOf(Capability capability) noexcept {
  return static_cast<std::uint32_t>(capability);
}

CapabilitySet CapabilitySet::fromList(const QList<Capability>& capabilities) {
  CapabilitySet set;
  for (Capability capability : capabilities) {
    set.add(capability);
  }
  return set;
}

void CapabilitySet::add(Capability capability) noexcept {
  mask_ |= bitOf(capability);
}

void CapabilitySet::remove(Capability capability) noexcept {
  mask_ &= ~bitOf(capability);
}

bool CapabilitySet::contains(Capability capability) const noexcept {
  const std::uint32_t bit = bitOf(capability);
  // 用「按位与结果非零」而不是相等判断：掩码里可能同时含多个位，
  // 而能力位本身都是单一的 1U << n，因此两者等价，但这里对非法取值也更宽容。
  return (mask_ & bit) != 0;
}

QList<Capability> CapabilitySet::list() const {
  QList<Capability> result;
  for (Capability capability : kAllCapabilities) {
    if (contains(capability)) {
      result.append(capability);
    }
  }
  return result;
}

int CapabilitySet::size() const noexcept {
  // 直接数位，不走 list()：后者要分配内存，把它放进 noexcept 函数里
  // 会让 noexcept 变成一句空话。
  std::uint32_t mask = mask_;
  int count = 0;
  while (mask != 0) {
    mask &= mask - 1;  // 每次清掉最低的一个 1
    ++count;
  }
  return count;
}

QString CapabilitySet::toLogString() const {
  const QList<Capability> capabilities = list();
  if (capabilities.isEmpty()) {
    return QStringLiteral("(无)");
  }

  QStringList names;
  names.reserve(capabilities.size());
  for (Capability capability : capabilities) {
    names.append(QString::fromLatin1(capabilityName(capability)));
  }
  return names.join(QLatin1Char('|'));
}

bool operator==(const CapabilitySet& lhs, const CapabilitySet& rhs) {
  // 约定里 list() 的顺序是稳定的，因此比较列表即比较集合本身。
  return lhs.list() == rhs.list();
}

bool operator!=(const CapabilitySet& lhs, const CapabilitySet& rhs) {
  return !(lhs == rhs);
}

}  // namespace baniphelper::core
