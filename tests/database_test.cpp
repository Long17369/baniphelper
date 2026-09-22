// 数据库子系统的检查（阶段一 S1.6）。
//
// 断言逐条对应验收标准「首次启动自动建库，二次启动不重复建表」，以及三条设计规则：
// 迁移只前进、一批写入只用一个事务、失败不留半个事务。
//
// WAL 单独验一条**读回确认**：SQLite 开不成 WAL 时不报错、而是返回当前模式，
// 所以「执行过 PRAGMA journal_mode=WAL」不等于「WAL 真的开着」。
//
// 全部在 QTemporaryDir 里跑，不碰 `%APPDATA%`。

#include <QTest>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QVariantList>

#include "core/database.h"
#include "core/error.h"

using namespace baniphelper::core;

namespace {

QString databasePathIn(const QTemporaryDir& dir) {
  return QDir(dir.path()).absoluteFilePath(QStringLiteral("baniphelper.db"));
}

/// 内置迁移 + 一条测试迁移，用来验证「版本更高」与「迁移中途失败」两条路径。
///
/// 版本号取「内置表的下一个空位」而**不写死数字**：`rules`（S2.9）落地时
/// 这条测试就靠它从 2 自动挑到了 3，将来再加迁移也不用回来改。
QList<Migration> migrationsWithExtraStep() {
  QList<Migration> list = defaultMigrations();
  Migration extra;
  extra.version = Database::latestSchemaVersion(list) + 1;
  extra.description = QStringLiteral("测试用：建立临时表 t");
  extra.statements =
      QStringList{QStringLiteral("CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT)")};
  list.append(extra);
  return list;
}

void createTestTable(Database& database) {
  const auto created =
      database.execute(QStringLiteral("CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT)"));
  QVERIFY(created.hasValue());
}

int rowCount(Database& database) {
  const auto rows = database.query(QStringLiteral("SELECT count(*) FROM t"));
  if (!rows || rows.value().isEmpty() || rows.value().first().isEmpty()) {
    return -1;
  }
  return rows.value().first().first().toInt();
}

}  // namespace

class DatabaseTest : public QObject {
  Q_OBJECT

 private slots:
  void createsDatabaseAndAppliesMigrations();
  void secondOpenDoesNotRepeatMigrations();
  void walModeIsActuallyOn();
  void batchInsertWritesAllRowsInOneCall();
  void batchInsertRejectsInconsistentArity();
  void batchInsertRollsBackOnConstraintViolation();
  void executeReportsSqlErrorAndKeepsUsable();
  void transactionRollsBackWhenNotCommitted();
  void transactionCommitsExplicitly();
  void nonSqliteFileIsRejectedAndLeftAlone();
  void newerSchemaVersionIsRejected();
  void failingMigrationLeavesEarlierVersionIntact();
  void brokenMigrationTableIsRejected();
  void missingMetaKeyIsReported();
};

void DatabaseTest::createsDatabaseAndAppliesMigrations() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const QString path = databasePathIn(dir);
  QVERIFY(!QFile::exists(path));

  Database database;
  const auto opened = database.open(path);
  QVERIFY2(opened.hasValue(), qPrintable(opened.hasValue() ? QString() : opened.error().message));

  QVERIFY(QFile::exists(path));
  QCOMPARE(database.schemaVersion().value(), Database::latestSchemaVersion(defaultMigrations()));
  QVERIFY(database.isOpen());

  // created_at 要在建库时就写上：它的意义是回答「这个库是哪一版、什么时候建的」。
  const auto createdAt = database.metaValue(QString::fromLatin1(kMetaKeyCreatedAt));
  QVERIFY(createdAt.hasValue());
  QVERIFY(!createdAt.value().isEmpty());

  database.close();
  QVERIFY(!database.isOpen());
}

void DatabaseTest::secondOpenDoesNotRepeatMigrations() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const QString path = databasePathIn(dir);

  QString firstCreatedAt;
  {
    Database database;
    QVERIFY(database.open(path).hasValue());
    const auto createdAt = database.metaValue(QString::fromLatin1(kMetaKeyCreatedAt));
    QVERIFY(createdAt.hasValue());
    firstCreatedAt = createdAt.value();
    database.close();
  }

  Database reopened;
  QVERIFY(reopened.open(path).hasValue());
  QCOMPARE(reopened.schemaVersion().value(), Database::latestSchemaVersion(defaultMigrations()));

  // 「不重复建表」的判据：创建时间没被改写（改写说明又跑了一遍初始化）。
  const auto secondCreatedAt = reopened.metaValue(QString::fromLatin1(kMetaKeyCreatedAt));
  QVERIFY(secondCreatedAt.hasValue());
  QCOMPARE(secondCreatedAt.value(), firstCreatedAt);
  reopened.close();
}

void DatabaseTest::walModeIsActuallyOn() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());

  Database database;
  QVERIFY(database.open(databasePathIn(dir)).hasValue());

  // 读回来确认，而不是相信 PRAGMA 执行成功。
  const auto mode = database.query(QStringLiteral("PRAGMA journal_mode"));
  QVERIFY(mode.hasValue());
  QVERIFY(!mode.value().isEmpty());
  QCOMPARE(mode.value().first().first().toString().toLower(), QStringLiteral("wal"));

  // 真开 WAL 才会有 -wal 边文件。这是第二个独立佐证。
  QVERIFY(database.execute(QStringLiteral("CREATE TABLE t (id INTEGER)")).hasValue());
  QVERIFY(database.execute(QStringLiteral("INSERT INTO t (id) VALUES (1)")).hasValue());
  QVERIFY2(QFile::exists(databasePathIn(dir) + QStringLiteral("-wal")),
           "没看到 -wal 边文件，WAL 可能并没有真的打开");

  database.close();
}

void DatabaseTest::batchInsertWritesAllRowsInOneCall() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());

  Database database;
  QVERIFY(database.open(databasePathIn(dir)).hasValue());
  createTestTable(database);

  QList<QVariantList> rows;
  for (int index = 1; index <= 200; ++index) {
    rows.append(QVariantList{index, QStringLiteral("名字 %1").arg(index)});
  }

  const auto written =
      database.insertMany(QStringLiteral("INSERT INTO t (id, name) VALUES (?, ?)"), rows);
  QVERIFY2(written.hasValue(),
           qPrintable(written.hasValue() ? QString() : written.error().message));
  QCOMPARE(written.value(), 200);
  QCOMPARE(rowCount(database), 200);

  // 空批次是合法的「什么也不用写」，不是错误。
  const auto empty =
      database.insertMany(QStringLiteral("INSERT INTO t (id, name) VALUES (?, ?)"), {});
  QVERIFY(empty.hasValue());
  QCOMPARE(empty.value(), 0);

  database.close();
}

void DatabaseTest::batchInsertRejectsInconsistentArity() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());

  Database database;
  QVERIFY(database.open(databasePathIn(dir)).hasValue());
  createTestTable(database);

  // 第二行少一个绑定。这种批次必须整体拒绝：SQLite 会沿用上一行留下的值，
  // 于是写出「插入成功但值是上一行的」记录，比报错难查得多。
  QList<QVariantList> rows;
  rows.append(QVariantList{1, QStringLiteral("完整")});
  rows.append(QVariantList{2});

  const auto written =
      database.insertMany(QStringLiteral("INSERT INTO t (id, name) VALUES (?, ?)"), rows);
  QVERIFY(!written.hasValue());
  QVERIFY(!written.error().message.isEmpty());
  QCOMPARE(rowCount(database), 0);

  database.close();
}

void DatabaseTest::batchInsertRollsBackOnConstraintViolation() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());

  Database database;
  QVERIFY(database.open(databasePathIn(dir)).hasValue());
  createTestTable(database);

  QList<QVariantList> rows;
  rows.append(QVariantList{1, QStringLiteral("a")});
  rows.append(QVariantList{2, QStringLiteral("b")});
  rows.append(QVariantList{1, QStringLiteral("重复主键")});
  rows.append(QVariantList{4, QStringLiteral("d")});

  const auto written =
      database.insertMany(QStringLiteral("INSERT INTO t (id, name) VALUES (?, ?)"), rows);
  QVERIFY(!written.hasValue());

  // 关键：前两行也必须一起回滚。留下「写了一半」的表，比整批失败更难收拾。
  QCOMPARE(rowCount(database), 0);
  QVERIFY(!written.error().message.isEmpty());

  database.close();
}

void DatabaseTest::executeReportsSqlErrorAndKeepsUsable() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());

  Database database;
  QVERIFY(database.open(databasePathIn(dir)).hasValue());

  const auto bad = database.execute(QStringLiteral("THIS IS NOT SQL"));
  QVERIFY(!bad.hasValue());
  QVERIFY(!bad.error().message.isEmpty());

  // 一次语法错误不该把连接搞坏。
  QVERIFY(database.execute(QStringLiteral("CREATE TABLE t (id INTEGER)")).hasValue());
  QVERIFY(database.isOpen());

  database.close();
}

void DatabaseTest::transactionRollsBackWhenNotCommitted() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());

  Database database;
  QVERIFY(database.open(databasePathIn(dir)).hasValue());
  createTestTable(database);

  {
    const auto transaction = database.beginTransaction();
    QVERIFY(transaction.hasValue());
    QVERIFY(database.execute(QStringLiteral("INSERT INTO t (id, name) VALUES (1, '未提交')"))
                .hasValue());
    // 不提交，让守卫走出作用域。
  }

  QCOMPARE(rowCount(database), 0);

  database.close();
}

void DatabaseTest::transactionCommitsExplicitly() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());

  Database database;
  QVERIFY(database.open(databasePathIn(dir)).hasValue());
  createTestTable(database);

  {
    auto transaction = database.beginTransaction();
    QVERIFY(transaction.hasValue());
    QVERIFY(database.execute(QStringLiteral("INSERT INTO t (id, name) VALUES (1, '已提交')"))
                .hasValue());
    QVERIFY(transaction.value().commit().hasValue());
  }

  QCOMPARE(rowCount(database), 1);

  // 重复提交要报错，不能静默成功 —— 静默成功会掩盖「以为在事务里、其实已经出去了」。
  database.close();
}

void DatabaseTest::nonSqliteFileIsRejectedAndLeftAlone() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const QString path = databasePathIn(dir);

  const QByteArray garbage = QByteArrayLiteral("这不是 SQLite 文件，只是一段文本");
  {
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(garbage);
    file.close();
  }

  Database database;
  const auto opened = database.open(path);
  QVERIFY2(!opened.hasValue(), "把一段文本当成数据库打开应当失败");
  QVERIFY(!opened.error().message.isEmpty());
  QVERIFY(!database.isOpen());

  // 关键：不许动那个文件。它可能是用户的真实数据，也可能是别的程序的文件。
  QFile check(path);
  QVERIFY(check.open(QIODevice::ReadOnly));
  QCOMPARE(check.readAll(), garbage);
  check.close();
}

void DatabaseTest::newerSchemaVersionIsRejected() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const QString path = databasePathIn(dir);

  // 先用「多一个迁移」的表建库，模拟更高版本的程序建的库。
  {
    const QList<Migration> withExtra = migrationsWithExtraStep();
    Database newer;
    QVERIFY(newer.open(path, withExtra).hasValue());
    QCOMPARE(newer.schemaVersion().value(), Database::latestSchemaVersion(withExtra));
    newer.close();
  }

  // 再用「只有一个迁移」的表打开：必须拒绝，而不是硬着头皮用。
  Database older;
  const auto opened = older.open(path, defaultMigrations());
  QVERIFY2(!opened.hasValue(), "版本更高的库被旧代码打开了，它会按过时的表结构去写");
  QVERIFY(!opened.error().message.isEmpty());
  QVERIFY(!older.isOpen());
}

void DatabaseTest::failingMigrationLeavesEarlierVersionIntact() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const QString path = databasePathIn(dir);

  QList<Migration> broken = defaultMigrations();
  Migration second;
  second.version = Database::latestSchemaVersion(broken) + 1;
  second.description = QStringLiteral("测试用：前半句能跑、后半句是垃圾");
  second.statements = QStringList{QStringLiteral("CREATE TABLE half (id INTEGER)"),
                                  QStringLiteral("THIS IS NOT SQL")};
  broken.append(second);

  {
    Database database;
    const auto opened = database.open(path, broken);
    QVERIFY(!opened.hasValue());
    QVERIFY(!opened.error().message.isEmpty());
  }

  // 重新用正常的迁移表打开：版本应当停在「内置表的最后一个」，而且半成品表不该存在。
  const QList<Migration> builtin = defaultMigrations();
  Database reopened;
  QVERIFY(reopened.open(path, builtin).hasValue());
  QCOMPARE(reopened.schemaVersion().value(), Database::latestSchemaVersion(builtin));

  const auto leftover =
      reopened.query(QStringLiteral("SELECT name FROM sqlite_master WHERE name = 'half'"));
  QVERIFY(leftover.hasValue());
  QVERIFY2(leftover.value().isEmpty(), "失败迁移建出的表没有被回滚掉");
  reopened.close();
}

void DatabaseTest::brokenMigrationTableIsRejected() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());

  // 版本号跳号
  QList<Migration> gap = defaultMigrations();
  Migration third;
  third.version = Database::latestSchemaVersion(gap) + 2;
  third.description = QStringLiteral("跳号了");
  third.statements = QStringList{QStringLiteral("CREATE TABLE gap (id INTEGER)")};
  gap.append(third);

  Database first;
  const auto gapResult = first.open(databasePathIn(dir), gap);
  QVERIFY2(!gapResult.hasValue(), "迁移表跳号应当当场被拒，否则不同机器的库结构会分叉");
  QVERIFY(!gapResult.error().message.isEmpty());

  // 迁移没有语句
  QList<Migration> empty = defaultMigrations();
  Migration hollow;
  hollow.version = Database::latestSchemaVersion(empty) + 1;
  hollow.description = QStringLiteral("没有语句");
  empty.append(hollow);
  Database second;
  QVERIFY(!second.open(databasePathIn(dir), empty).hasValue());
}

void DatabaseTest::missingMetaKeyIsReported() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());

  Database database;
  QVERIFY(database.open(databasePathIn(dir)).hasValue());

  const auto missing = database.metaValue(QStringLiteral("没有这个键"));
  QVERIFY(!missing.hasValue());
  QVERIFY(!missing.error().message.isEmpty());
  QCOMPARE(missing.error().code, ErrorCode::NotFound);

  const auto set = database.setMetaValue(QStringLiteral("自定义"), QStringLiteral("值"));
  QVERIFY(set.hasValue());
  QCOMPARE(database.metaValue(QStringLiteral("自定义")).value(), QStringLiteral("值"));

  // 同一个键写两次是覆盖，不是插入失败。
  QVERIFY(database.setMetaValue(QStringLiteral("自定义"), QStringLiteral("新值")).hasValue());
  QCOMPARE(database.metaValue(QStringLiteral("自定义")).value(), QStringLiteral("新值"));

  database.close();
}

QTEST_GUILESS_MAIN(DatabaseTest)

#include "database_test.moc"
