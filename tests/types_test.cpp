// 核心值类型的检查。
//
// 目前只覆盖 `sameProcessIdentity`。它的失败方式是**安静的**：形式不一致时返回
// 「不是同一个程序」，看起来像规则没命中，实际是比较方式不对。
// 而程序域的路径在规范化之后是正斜杠形式（`QDir::cleanPath`），
// 系统给出的进程路径却是反斜杠形式，两者必须能配上。

#include <QTest>

#include <QString>

#include "core/types.h"

using namespace baniphelper::core;

namespace {

ProcessIdentity identityOf(const QString& path) {
  ProcessIdentity identity;
  identity.imagePath = path;
  identity.displayName = QStringLiteral("test.exe");
  return identity;
}

}  // namespace

class TypesTest : public QObject {
  Q_OBJECT

 private slots:
  void processIdentityIgnoresCaseAndSeparatorDirection();
  void processIdentityStillSeparatesDifferentFiles();
};

void TypesTest::processIdentityIgnoresCaseAndSeparatorDirection() {
  // 分隔符方向：同一个文件的两种写法。
  QVERIFY(sameProcessIdentity(identityOf(QStringLiteral("C:\\Tools\\a.exe")),
                              identityOf(QStringLiteral("C:/Tools/a.exe"))));

  // 大小写：Windows 文件系统不区分。
  QVERIFY(sameProcessIdentity(identityOf(QStringLiteral("C:\\Tools\\a.exe")),
                              identityOf(QStringLiteral("c:\\tools\\A.EXE"))));

  // 冗余的分隔符也要归一，否则「用户手输的路径」与「系统给的路径」配不上。
  QVERIFY(sameProcessIdentity(identityOf(QStringLiteral("C:\\Tools\\\\a.exe")),
                              identityOf(QStringLiteral("C:/Tools/a.exe"))));
}

void TypesTest::processIdentityStillSeparatesDifferentFiles() {
  // 归一是为了能配上，不是为了让不同的文件也配上。
  QVERIFY(!sameProcessIdentity(identityOf(QStringLiteral("C:/Tools/a.exe")),
                               identityOf(QStringLiteral("C:/Tools/b.exe"))));
  QVERIFY(!sameProcessIdentity(identityOf(QStringLiteral("C:/Tools/a.exe")),
                               identityOf(QStringLiteral("D:/Tools/a.exe"))));
}

QTEST_GUILESS_MAIN(TypesTest)

#include "types_test.moc"
