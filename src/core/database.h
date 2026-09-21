#pragma once

#include <QList>
#include <QString>
#include <QVariant>
#include <QVariantList>

#include <memory>

#include "core/result.h"

namespace baniphelper::core {

/// `meta` 表里记迁移版本号的键。
inline constexpr char kMetaKeySchemaVersion[] = "schema_version";

/// `meta` 表里记「本库创建于何时」的键。排查时能回答「这个库是哪一版建的」。
inline constexpr char kMetaKeyCreatedAt[] = "created_at";

/// 一次结构变更。
///
/// `statements` 一条一个元素：SQLite 的预处理一次只吃一条语句，
/// 把多条语句拼成一个字符串传进去只会执行第一条，而**后面的静默不执行**。
struct Migration {
  /// 结构版本号，从 1 开始连续递增。
  int version = 0;

  /// 中文说明，写进日志，回答「这次打开动过什么」。
  QString description;

  /// 按顺序执行的语句。
  QStringList statements;
};

/// 本程序内置的迁移表。**只追加，不修改已发布的条目** ——
/// 改动已发布的迁移，对那些已经跑过它的库不再生效，于是不同机器上的库结构会悄悄分叉。
[[nodiscard]] QList<Migration> defaultMigrations();

/// 数据表的建库、迁移与写入。
///
/// 本期（S1.6）只立基础设施：打开文件、开 WAL、按版本号跑迁移、批量写入。
/// **业务表不在这里**：`rules` 属 S2.9、`connection_records` 属 S3，
/// 各自作为新的迁移加进来。迁移机制存在的意义就是这个 ——
/// 给还没定的表结构提前定型，只会换来一次用不上的迁移。
///
/// 三条立约定死的规则：
///
/// - **迁移只前进**。版本号比代码支持的更高时直接拒绝打开，不尝试「降级」：
///   用旧代码去动新库，比打不开危险得多。
/// - **一批写入只用一个事务**。记录量很大，每条一次事务会把磁盘拖垮，
///   所以 `insertMany` 自带事务，不要求调用方记得手动包。
/// - **失败不留半个事务**。任何一行写失败都整体回滚，绝不留下「写了一半」的表。
class Database {
 public:
  Database();
  ~Database();

  Database(const Database&) = delete;
  Database& operator=(const Database&) = delete;

  /// 打开数据库，必要时建库并把迁移跑到最新。
  ///
  /// 父目录不存在时会创建。传进来的若不是 SQLite 文件，或者库里的版本号
  /// 高于本程序支持的版本，都会明确失败 —— 且**不会去动那个文件**：
  /// 那可能是用户的真实数据，也可能是另一个版本的程序建的库。
  ///
  /// `migrations` 留空表示用 `defaultMigrations()`；测试用自定义表来验证迁移推进本身。
  [[nodiscard]] Result<void> open(const QString& filePath, QList<Migration> migrations = {});

  void close();
  [[nodiscard]] bool isOpen() const;
  [[nodiscard]] QString filePath() const;

  /// 当前库的结构版本号。未打开时返回失败。
  [[nodiscard]] Result<int> schemaVersion() const;

  /// 迁移表里最高的版本号。
  [[nodiscard]] static int latestSchemaVersion(const QList<Migration>& migrations);

  /// 执行一条不取回结果的语句。
  [[nodiscard]] Result<void> execute(const QString& sql, const QVariantList& bindings = {});

  /// 执行查询，返回全部行；每行按绑定顺序给出。
  [[nodiscard]] Result<QList<QVariantList>> query(const QString& sql,
                                                  const QVariantList& bindings = {}) const;

  /// 批量写入：同一个事务里预编译一次、逐行执行。
  ///
  /// 所有行的绑定个数必须一致，否则整批拒绝 —— 个数不一致时 SQLite 会沿用上一行
  /// 留下的绑定值，写出「看起来成功但值是错的」记录，那比报错难查得多。
  [[nodiscard]] Result<int> insertMany(const QString& sql, const QList<QVariantList>& rows);

  [[nodiscard]] Result<QString> metaValue(const QString& key) const;
  [[nodiscard]] Result<void> setMetaValue(const QString& key, const QString& value);

  /// 开一个事务。没提交就析构等价于回滚。
  [[nodiscard]] Result<class Transaction> beginTransaction();

  // 供 Transaction 调用。直接调用也可以，但要自己保证配平。
  [[nodiscard]] Result<void> commitTransaction();
  [[nodiscard]] Result<void> rollbackTransaction();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// 事务守卫。
///
/// **析构时不提交**：忘记提交就等价于回滚。反过来（析构时自动提交）会让
/// 「中途 return」这种最常见的写法变成静默提交半个事务。
class Transaction {
 public:
  Transaction(Transaction&& other) noexcept;
  Transaction& operator=(Transaction&& other) noexcept;
  ~Transaction();

  Transaction(const Transaction&) = delete;
  Transaction& operator=(const Transaction&) = delete;

  [[nodiscard]] Result<void> commit();
  [[nodiscard]] Result<void> rollback();

 private:
  friend class Database;
  explicit Transaction(Database* database);

  Database* database_ = nullptr;
  bool finished_ = false;
};

}  // namespace baniphelper::core
