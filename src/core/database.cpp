#include "core/database.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QMutexLocker>
#include <QRecursiveMutex>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QStringList>
#include <QVariant>

#include <atomic>
#include <utility>

#include "core/log.h"

namespace baniphelper::core {
namespace {

/// 连接名必须唯一。
///
/// Qt 的连接是按名字全局登记的：重名会让两个 `Database` 实例悄悄共用同一个连接，
/// 于是两个库的事务互相干扰，而现象会表现为「事务偶尔莫名失败」。
QString nextConnectionName() {
  static std::atomic<quint64> counter{0};
  return QStringLiteral("baniphelper-db-%1").arg(counter.fetch_add(1) + 1);
}

/// 把 Qt 的错误包装成中文明确的失败。错误文本为空时也要给个说法，
/// 否则界面会显示一句只有冒号的空话。
///
/// 返回 `Error` 而不是 `Result<void>`：调用处既可能是 `Result<void>`，
/// 也可能是 `Result<int>`、`Result<Transaction>`，返回裸 `Error` 才能两边通用。
Error sqlFailure(const QString& action, const QSqlError& error) {
  const QString detail =
      error.text().isEmpty() ? QStringLiteral("SQLite 未给出原因") : error.text();
  return makeError(ErrorCode::Io, QStringLiteral("%1失败：%2").arg(action, detail));
}

/// 迁移表的自检。版本号必须从 1 连续递增 —— 中间缺号意味着有人漏写了一条迁移，
/// 而那会让后面所有库的结构与代码不一致。
Result<void> checkMigrations(const QList<Migration>& migrations) {
  if (migrations.isEmpty()) {
    return Result<void>::fail(
        makeError(ErrorCode::InvalidArgument, QStringLiteral("迁移表为空。没有迁移就没有任何表")));
  }

  int expected = 1;
  for (const Migration& migration : migrations) {
    if (migration.version != expected) {
      return Result<void>::fail(
          makeError(ErrorCode::InvalidArgument,
                    QStringLiteral("迁移表的版本号必须从 1 连续递增，期望 %1，实际是 %2")
                        .arg(expected)
                        .arg(migration.version)));
    }
    if (migration.description.trimmed().isEmpty()) {
      return Result<void>::fail(makeError(
          ErrorCode::InvalidArgument, QStringLiteral("迁移 %1 缺少说明").arg(migration.version)));
    }
    if (migration.statements.isEmpty()) {
      return Result<void>::fail(makeError(ErrorCode::InvalidArgument,
                                          QStringLiteral("迁移 %1（%2）没有任何语句")
                                              .arg(migration.version)
                                              .arg(migration.description)));
    }
    ++expected;
  }
  return Result<void>::ok();
}

}  // namespace

QList<Migration> defaultMigrations() {
  Migration base;
  base.version = 1;
  base.description = QStringLiteral("建立 meta 键值表");
  base.statements = QStringList{
      QStringLiteral("CREATE TABLE meta (key TEXT PRIMARY KEY NOT NULL, value TEXT NOT NULL)")};

  // 后续迁移在这里追加，并遵守两条：版本号接着往下排；已发布的条目不修改、不重排。
  //   S2.9 → 2：rules 表
  //   S3   → 3：connection_records 表
  return QList<Migration>{base};
}

struct Database::Impl {
  /// 递归锁，不是偷懒。
  ///
  /// 公开方法之间是**互相调用**的（`open` 要走迁移，迁移要执行语句，
  /// `insertMany` 要开事务），用普通互斥体会当场自锁。
  /// 换成「所有内部方法都带 Locked 后缀」也能解决，但那把「记得别加锁」
  /// 变成一条靠人守的纪律，而这里只有一处调用者。
  mutable QRecursiveMutex mutex;

  QString connectionName;
  QString path;
  QList<Migration> migrations;
  bool open = false;

  /// 每次都新取一份连接，**不缓存**。
  ///
  /// 缓存下来的 `QSqlDatabase` 副本会让 `removeDatabase` 报「连接仍在使用」，
  /// 于是连接泄漏，反复开关数据库就会耗尽连接名。
  [[nodiscard]] QSqlDatabase connection() const {
    return QSqlDatabase::database(connectionName, false);
  }
};

Database::Database() : impl_(std::make_unique<Impl>()) {}

Database::~Database() {
  close();
}

int Database::latestSchemaVersion(const QList<Migration>& migrations) {
  int latest = 0;
  for (const Migration& migration : migrations) {
    if (migration.version > latest) {
      latest = migration.version;
    }
  }
  return latest;
}

Result<void> Database::open(const QString& filePath, QList<Migration> migrations) {
  QMutexLocker<QRecursiveMutex> locker(&impl_->mutex);

  if (impl_->open) {
    return Result<void>::fail(
        makeError(ErrorCode::AlreadyExists, QStringLiteral("数据库已经打开，不能重复打开")));
  }
  if (filePath.trimmed().isEmpty()) {
    return Result<void>::fail(
        makeError(ErrorCode::InvalidArgument, QStringLiteral("数据库文件路径为空")));
  }
  if (migrations.isEmpty()) {
    migrations = defaultMigrations();
  }
  const Result<void> tableOk = checkMigrations(migrations);
  if (!tableOk) {
    return tableOk;
  }

  const QString path = QDir::cleanPath(filePath);
  const QDir parent = QFileInfo(path).absoluteDir();
  if (!parent.exists() && !parent.mkpath(QStringLiteral("."))) {
    return Result<void>::fail(
        makeError(ErrorCode::Io, QStringLiteral("无法创建数据库目录：") + parent.absolutePath()));
  }

  impl_->connectionName = nextConnectionName();
  impl_->path = path;
  impl_->migrations = std::move(migrations);

  {
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), impl_->connectionName);
    db.setDatabaseName(path);
    if (!db.open()) {
      const QSqlError error = db.lastError();
      db = QSqlDatabase();  // 先结束副本，再移除连接，否则 Qt 会警告「连接仍在使用」
      QSqlDatabase::removeDatabase(impl_->connectionName);
      impl_->connectionName.clear();
      return sqlFailure(QStringLiteral("打开数据库 %1").arg(path), error);
    }
  }

  // `open()` 只是登记，真正的问题要到第一条语句才暴露：
  // 目标不是 SQLite 文件时它照样成功，查询时才报 "file is not a database"。
  // 所以这里主动压一条语句，把校验提前到打开阶段，并明确地**不动那个文件**。
  const Result<QList<QVariantList>> probe =
      query(QStringLiteral("SELECT count(*) FROM sqlite_master"));
  if (!probe) {
    const QString reason = probe.error().message;
    close();
    return Result<void>::fail(
        makeError(ErrorCode::Io,
                  QStringLiteral("打开的不是可用的 SQLite 数据库：%1（%2）").arg(path, reason)));
  }

  // WAL：并发读写的前提，也是「记录写入不成为瓶颈」那条设计的前提。
  //
  // 但**不是所有位置都开得起来**（网络盘、部分云同步目录只支持 delete 模式），
  // 而且失败时 SQLite 是「返回当前模式」而不是报错。所以要读回来确认，
  // 不能执行完就当成功 —— 那会让人以为记录写入很快，实际仍是串行。
  const Result<QList<QVariantList>> journal = query(QStringLiteral("PRAGMA journal_mode=WAL"));
  const bool walOn = journal && !journal.value().isEmpty() && !journal.value().first().isEmpty() &&
                     journal.value().first().first().toString().compare(QStringLiteral("wal"),
                                                                        Qt::CaseInsensitive) == 0;
  if (!walOn) {
    const QString actual = journal.value().isEmpty() || journal.value().first().isEmpty()
                               ? QStringLiteral("无法读取")
                               : journal.value().first().first().toString();
    close();
    return Result<void>::fail(
        makeError(ErrorCode::Io,
                  QStringLiteral("无法把数据库切到 WAL 模式，实际模式是「%1」：%2。"
                                 "这个位置大概率不支持 WAL（网络盘、云同步目录），请换一个本地目录")
                      .arg(actual, path)));
  }

  // 外键约束默认是关的，必须每个连接自己打开。
  const Result<void> foreignKeys = execute(QStringLiteral("PRAGMA foreign_keys=ON"));
  if (!foreignKeys) {
    close();
    return foreignKeys;
  }
  // 被别处占用时等一会儿再报错，而不是立刻失败：WAL 下读写是可以并存的。
  const Result<void> busyTimeout = execute(QStringLiteral("PRAGMA busy_timeout=5000"));
  if (!busyTimeout) {
    close();
    return busyTimeout;
  }
  // WAL 下 `synchronous=NORMAL` 是常规取舍：崩溃可能丢掉最后几个事务，但不会损坏库。
  const Result<void> synchronous = execute(QStringLiteral("PRAGMA synchronous=NORMAL"));
  if (!synchronous) {
    close();
    return synchronous;
  }

  impl_->open = true;

  const Result<void> migrated = [&]() -> Result<void> {
    // 先确认 meta 表在不在，而不是「读不到版本就当 0」：
    // 后者会把「表在、但读取真出错」也当成全新库，于是又去建一次表，把真错误盖掉。
    int current = 0;
    const Result<QList<QVariantList>> metaTable = query(
        QStringLiteral("SELECT name FROM sqlite_master WHERE type = 'table' AND name = 'meta'"));
    if (!metaTable) {
      return Result<void>::fail(metaTable.error());
    }
    if (!metaTable.value().isEmpty()) {
      const Result<QString> versionText = metaValue(QString::fromLatin1(kMetaKeySchemaVersion));
      if (versionText) {
        bool ok = false;
        const int parsed = versionText.value().toInt(&ok);
        if (!ok) {
          return Result<void>::fail(
              makeError(ErrorCode::Internal,
                        QStringLiteral("meta 表里的 %1 不是数字：%2")
                            .arg(QString::fromLatin1(kMetaKeySchemaVersion), versionText.value())));
        }
        current = parsed;
      } else if (versionText.error().code != ErrorCode::NotFound) {
        return Result<void>::fail(versionText.error());
      }
    }

    const int latest = latestSchemaVersion(impl_->migrations);
    if (current > latest) {
      // 用旧代码去动新库，比打不开危险得多：新版的表结构它不认识，
      // 写进去的东西可能破坏新版的约束。
      return Result<void>::fail(
          makeError(ErrorCode::NotSupported,
                    QStringLiteral("数据库的结构版本是 %1，本程序只支持到 %2。"
                                   "这个库是新版本的程序建的，请升级后再打开")
                        .arg(current)
                        .arg(latest)));
    }

    for (const Migration& migration : impl_->migrations) {
      if (migration.version <= current) {
        continue;
      }

      const Result<void> started = [&]() -> Result<void> {
        QSqlDatabase db = impl_->connection();
        if (!db.transaction()) {
          return sqlFailure(QStringLiteral("开启迁移事务"), db.lastError());
        }
        return Result<void>::ok();
      }();
      if (!started) {
        return started;
      }

      for (const QString& statement : migration.statements) {
        const Result<void> done = execute(statement);
        if (!done) {
          rollbackTransaction();
          return Result<void>::fail(
              makeError(ErrorCode::Io,
                        QStringLiteral("迁移 %1（%2）执行失败：%3")
                            .arg(migration.version)
                            .arg(migration.description, done.error().message)));
        }
      }

      const Result<void> recorded = setMetaValue(QString::fromLatin1(kMetaKeySchemaVersion),
                                                 QString::number(migration.version));
      if (!recorded) {
        rollbackTransaction();
        return recorded;
      }

      const Result<void> committed = commitTransaction();
      if (!committed) {
        rollbackTransaction();
        return committed;
      }

      logWrite(LogLevel::Info,
               QStringLiteral("数据库已迁移到版本 %1：%2")
                   .arg(migration.version)
                   .arg(migration.description));
    }

    return Result<void>::ok();
  }();

  if (!migrated) {
    close();
    return migrated;
  }

  // 创建时间只在缺失时写，重复打开不会改动它 —— 它的意义正是「这个库是哪一版建的」。
  const Result<QString> createdAt = metaValue(QString::fromLatin1(kMetaKeyCreatedAt));
  if (!createdAt && createdAt.error().code == ErrorCode::NotFound) {
    const Result<void> stamped = setMetaValue(QString::fromLatin1(kMetaKeyCreatedAt),
                                              QDateTime::currentDateTime().toString(Qt::ISODate));
    if (!stamped) {
      close();
      return stamped;
    }
  }

  return Result<void>::ok();
}

void Database::close() {
  QMutexLocker<QRecursiveMutex> locker(&impl_->mutex);
  if (impl_->connectionName.isEmpty()) {
    return;
  }

  {
    QSqlDatabase db = impl_->connection();
    if (db.isValid() && db.isOpen()) {
      db.close();
    }
  }  // 副本必须先析构，否则 removeDatabase 会报「连接仍在使用」

  QSqlDatabase::removeDatabase(impl_->connectionName);
  impl_->connectionName.clear();
  impl_->path.clear();
  impl_->migrations.clear();
  impl_->open = false;
}

bool Database::isOpen() const {
  QMutexLocker<QRecursiveMutex> locker(&impl_->mutex);
  return impl_->open;
}

QString Database::filePath() const {
  QMutexLocker<QRecursiveMutex> locker(&impl_->mutex);
  return impl_->path;
}

Result<QList<QVariantList>> Database::query(const QString& sql,
                                            const QVariantList& bindings) const {
  QMutexLocker<QRecursiveMutex> locker(&impl_->mutex);
  if (impl_->connectionName.isEmpty()) {
    return Result<QList<QVariantList>>::fail(
        makeError(ErrorCode::Internal, QStringLiteral("数据库还没有打开")));
  }

  QSqlQuery query(impl_->connection());
  if (!query.prepare(sql)) {
    return Result<QList<QVariantList>>::fail(
        sqlFailure(QStringLiteral("预处理查询"), query.lastError()));
  }
  for (int index = 0; index < bindings.size(); ++index) {
    query.bindValue(index, bindings.at(index));
  }
  if (!query.exec()) {
    return Result<QList<QVariantList>>::fail(
        sqlFailure(QStringLiteral("执行查询"), query.lastError()));
  }

  QList<QVariantList> rows;
  while (query.next()) {
    QVariantList row;
    const int count = query.record().count();
    row.reserve(count);
    for (int index = 0; index < count; ++index) {
      row.append(query.value(index));
    }
    rows.append(row);
  }
  return Result<QList<QVariantList>>::ok(rows);
}

Result<void> Database::execute(const QString& sql, const QVariantList& bindings) {
  QMutexLocker<QRecursiveMutex> locker(&impl_->mutex);
  if (impl_->connectionName.isEmpty()) {
    return Result<void>::fail(makeError(ErrorCode::Internal, QStringLiteral("数据库还没有打开")));
  }

  QSqlQuery query(impl_->connection());
  if (!query.prepare(sql)) {
    return sqlFailure(QStringLiteral("预处理语句"), query.lastError());
  }
  for (int index = 0; index < bindings.size(); ++index) {
    query.bindValue(index, bindings.at(index));
  }
  if (!query.exec()) {
    return sqlFailure(QStringLiteral("执行语句"), query.lastError());
  }
  return Result<void>::ok();
}

Result<int> Database::insertMany(const QString& sql, const QList<QVariantList>& rows) {
  QMutexLocker<QRecursiveMutex> locker(&impl_->mutex);
  if (!impl_->open) {
    return Result<int>::fail(makeError(ErrorCode::Internal, QStringLiteral("数据库还没有打开")));
  }
  if (rows.isEmpty()) {
    return Result<int>::ok(0);
  }

  // 绑定个数不一致时必须整批拒绝。
  //
  // 否则 SQLite 会沿用上一行留下的绑定值，写出「插入成功但值是上一行的」记录 ——
  // 这比报错难查得多，因为库里看起来有数据，只是内容不对。
  const int arity = rows.first().size();
  for (int index = 0; index < rows.size(); ++index) {
    if (rows.at(index).size() != arity) {
      return Result<int>::fail(makeError(
          ErrorCode::InvalidArgument,
          QStringLiteral("批量写入第 %1 行的绑定个数是 %2，与第一行的 %3 不一致，已整体拒绝")
              .arg(index + 1)
              .arg(rows.at(index).size())
              .arg(arity)));
    }
  }

  QSqlDatabase db = impl_->connection();
  if (!db.transaction()) {
    return Result<int>::fail(sqlFailure(QStringLiteral("开启批量写入事务"), db.lastError()));
  }

  QSqlQuery query(db);
  if (!query.prepare(sql)) {
    db.rollback();
    return Result<int>::fail(sqlFailure(QStringLiteral("预处理批量写入"), query.lastError()));
  }

  int written = 0;
  for (int index = 0; index < rows.size(); ++index) {
    const QVariantList& row = rows.at(index);
    for (int column = 0; column < row.size(); ++column) {
      query.bindValue(column, row.at(column));
    }
    if (!query.exec()) {
      const QSqlError error = query.lastError();
      db.rollback();
      return Result<int>::fail(makeError(
          ErrorCode::Io,
          QStringLiteral("批量写入第 %1 行失败，整批已回滚：%2")
              .arg(index + 1)
              .arg(error.text().isEmpty() ? QStringLiteral("SQLite 未给出原因") : error.text())));
    }
    ++written;
  }

  if (!db.commit()) {
    const QSqlError error = db.lastError();
    db.rollback();
    return Result<int>::fail(sqlFailure(QStringLiteral("提交批量写入"), error));
  }
  return Result<int>::ok(written);
}

Result<QString> Database::metaValue(const QString& key) const {
  const Result<QList<QVariantList>> rows =
      query(QStringLiteral("SELECT value FROM meta WHERE key = ?"), QVariantList{key});
  if (!rows) {
    return Result<QString>::fail(rows.error());
  }
  if (rows.value().isEmpty() || rows.value().first().isEmpty()) {
    return Result<QString>::fail(
        makeError(ErrorCode::NotFound, QStringLiteral("meta 表里没有键：%1").arg(key)));
  }
  return Result<QString>::ok(rows.value().first().first().toString());
}

Result<void> Database::setMetaValue(const QString& key, const QString& value) {
  return execute(QStringLiteral("INSERT INTO meta (key, value) VALUES (?, ?) "
                                "ON CONFLICT(key) DO UPDATE SET value = excluded.value"),
                 QVariantList{key, value});
}

Result<int> Database::schemaVersion() const {
  const Result<QString> text = metaValue(QString::fromLatin1(kMetaKeySchemaVersion));
  if (!text) {
    if (text.error().code == ErrorCode::NotFound) {
      return Result<int>::ok(0);
    }
    return Result<int>::fail(text.error());
  }
  bool ok = false;
  const int parsed = text.value().toInt(&ok);
  if (!ok) {
    return Result<int>::fail(
        makeError(ErrorCode::Internal,
                  QStringLiteral("meta 表里的 %1 不是数字：%2")
                      .arg(QString::fromLatin1(kMetaKeySchemaVersion), text.value())));
  }
  return Result<int>::ok(parsed);
}

Result<Transaction> Database::beginTransaction() {
  QMutexLocker<QRecursiveMutex> locker(&impl_->mutex);
  if (!impl_->open) {
    return Result<Transaction>::fail(
        makeError(ErrorCode::Internal, QStringLiteral("数据库还没有打开")));
  }
  QSqlDatabase db = impl_->connection();
  if (!db.transaction()) {
    return Result<Transaction>::fail(sqlFailure(QStringLiteral("开启事务"), db.lastError()));
  }
  return Result<Transaction>::ok(Transaction(this));
}

Result<void> Database::commitTransaction() {
  QMutexLocker<QRecursiveMutex> locker(&impl_->mutex);
  if (impl_->connectionName.isEmpty()) {
    return Result<void>::fail(makeError(ErrorCode::Internal, QStringLiteral("数据库还没有打开")));
  }
  QSqlDatabase db = impl_->connection();
  if (!db.commit()) {
    return sqlFailure(QStringLiteral("提交事务"), db.lastError());
  }
  return Result<void>::ok();
}

Result<void> Database::rollbackTransaction() {
  QMutexLocker<QRecursiveMutex> locker(&impl_->mutex);
  if (impl_->connectionName.isEmpty()) {
    return Result<void>::fail(makeError(ErrorCode::Internal, QStringLiteral("数据库还没有打开")));
  }
  QSqlDatabase db = impl_->connection();
  if (!db.rollback()) {
    return sqlFailure(QStringLiteral("回滚事务"), db.lastError());
  }
  return Result<void>::ok();
}

Transaction::Transaction(Database* database) : database_(database) {}

Transaction::Transaction(Transaction&& other) noexcept
    : database_(other.database_), finished_(other.finished_) {
  other.database_ = nullptr;
  other.finished_ = true;
}

Transaction& Transaction::operator=(Transaction&& other) noexcept {
  if (this != &other) {
    if (!finished_ && database_ != nullptr) {
      database_->rollbackTransaction();
    }
    database_ = other.database_;
    finished_ = other.finished_;
    other.database_ = nullptr;
    other.finished_ = true;
  }
  return *this;
}

Transaction::~Transaction() {
  if (!finished_ && database_ != nullptr) {
    // 没提交就是回滚。这不是可选项：剩下的半个事务留在连接上，
    // 会让「后一次写入」莫名其妙地一起被回滚。
    database_->rollbackTransaction();
  }
}

Result<void> Transaction::commit() {
  if (database_ == nullptr || finished_) {
    return Result<void>::fail(
        makeError(ErrorCode::Internal, QStringLiteral("这个事务已经结束，不能重复提交")));
  }
  const Result<void> done = database_->commitTransaction();
  if (!done) {
    return done;
  }
  finished_ = true;
  return Result<void>::ok();
}

Result<void> Transaction::rollback() {
  if (database_ == nullptr || finished_) {
    return Result<void>::ok();
  }
  const Result<void> done = database_->rollbackTransaction();
  finished_ = true;
  return done;
}

}  // namespace baniphelper::core
