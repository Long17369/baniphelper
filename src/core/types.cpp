#include "core/types.h"

#include <QDir>

namespace baniphelper::core {

bool sameProcessIdentity(const ProcessIdentity& lhs, const ProcessIdentity& rhs) noexcept {
  // 大小写不区分是因为 Windows 文件系统不区分；**分隔符方向也要归一**，
  // 因为 `C:\Tools\a.exe` 与 `C:/Tools/a.exe` 是同一个文件，
  // 而规则里的路径经 `QDir::cleanPath` 之后是正斜杠形式，系统给出的进程路径
  // 却是反斜杠形式 —— 不归一的话「拿规则去比对进程」会永远配不上，
  // 而且失败得很安静：看起来是规则没命中，实际是形式不匹配。
  //
  // 归一放在这里而不是指望调用方自己做：身份比较的语义只有这一处定义。
  const QString left = QDir::cleanPath(lhs.imagePath);
  const QString right = QDir::cleanPath(rhs.imagePath);
  return QString::compare(left, right, Qt::CaseInsensitive) == 0;
}

}  // namespace baniphelper::core
