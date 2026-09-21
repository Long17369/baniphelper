#include "core/types.h"

namespace baniphelper::core {

bool sameProcessIdentity(const ProcessIdentity& lhs, const ProcessIdentity& rhs) noexcept {
  // Windows 文件系统不区分大小写，因此身份比较也不区分。
  // 用 Qt::CaseInsensitive 而不是自己写 toLower，避免无谓的分配。
  return QString::compare(lhs.imagePath, rhs.imagePath, Qt::CaseInsensitive) == 0;
}

}  // namespace baniphelper::core
