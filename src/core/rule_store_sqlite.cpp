// 规则持久化的 SQLite 实现（阶段二 S2.9）。
//
// 表结构见 `database.cpp` 的迁移 2。这里只做三件事：把 `Rule` 与行互转、校验、通知订阅者。
//
// ⚠️ 读出来也必须校验，而且**不许宽松解释**。库里的行有可能被手改过、或者由别的版本写下，
// 遇到不认识的域或方式时按「大概是这个意思」去理解，后果是**按错的语义封禁** ——
// 那比这条规则不生效危险得多。

#include "core/rule_store_sqlite.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QList>
#include <QStringList>
#include <QVariant>
#include <QVariantList>

#include <utility>

#include "core/error.h"
#include "core/rule_model.h"

namespace baniphelper::core {
namespace {

/// 时间列的统一格式：**UTC** + 毫秒。
///
/// `expire_at` 的到期判断走字符串比较（SQL 里 `<=`），所以格式与时区都必须统一 ——
/// 混进一个本地时间就会有规则在该到期的时候不到期。
QString toStored(const QDateTime& value) {
  if (!value.isValid()) {
    return QString();
  }
  return value.toUTC().toString(Qt::ISODateWithMs);
}

QDateTime fromStored(const QString& value) {
  if (value.isEmpty()) {
    return QDateTime();
  }
  return QDateTime::fromString(value, Qt::ISODateWithMs);
}

QString encodeConditions(const QList<MatchCondition>& conditions) {
  QJsonArray array;
  for (const MatchCondition& condition : conditions) {
    QJsonObject object;
    object.insert(QStringLiteral("domain"), condition.domain);
    object.insert(QStringLiteral("mode"), condition.mode);
    object.insert(QStringLiteral("negate"), condition.negate);

    QJsonArray values;
    for (const QString& value : condition.values) {
      values.append(value);
    }
    object.insert(QStringLiteral("values"), values);
    array.append(object);
  }
  return QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact));
}

Result<QList<MatchCondition>> decodeConditions(const QString& text) {
  QJsonParseError parseError{};
  const QJsonDocument document = QJsonDocument::fromJson(text.toUtf8(), &parseError);
  if (parseError.error != QJsonParseError::NoError || !document.isArray()) {
    return Result<QList<MatchCondition>>::fail(makeError(
        ErrorCode::Io,
        QStringLiteral("规则的条件不是合法的 JSON 数组：%1").arg(parseError.errorString())));
  }

  QList<MatchCondition> conditions;
  const QJsonArray array = document.array();
  for (const QJsonValue& entry : array) {
    if (!entry.isObject()) {
      return Result<QList<MatchCondition>>::fail(
          makeError(ErrorCode::Io, QStringLiteral("规则的条件里有一项不是对象")));
    }
    const QJsonObject object = entry.toObject();
    MatchCondition condition;
    condition.domain = object.value(QStringLiteral("domain")).toString();
    condition.mode = object.value(QStringLiteral("mode")).toString();
    condition.negate = object.value(QStringLiteral("negate")).toBool(false);
    for (const QJsonValue& value : object.value(QStringLiteral("values")).toArray()) {
      condition.values.append(value.toString());
    }
    conditions.append(condition);
  }
  return Result<QList<MatchCondition>>::ok(conditions);
}

/// 一行的列顺序，查询与取值都以此为准，免得两处各写一份列名。
const char* const kColumns =
    "id, action, conditions, enabled, expire_at, note, schema, "
    "created_at";

Rule ruleFromRow(const QVariantList& row) {
  Rule rule;
  rule.id = row.at(0).toString();
  rule.action = row.at(1).toInt() == static_cast<int>(RuleAction::Allow) ? RuleAction::Allow
                                                                         : RuleAction::Block;
  const Result<QList<MatchCondition>> conditions = decodeConditions(row.at(2).toString());
  if (conditions) {
    rule.conditions = conditions.value();
  } else {
    // 条件解不出来也照样构造出来，让上层那道 validateRule 统一报错 ——
    // 这样「坏成什么样」只有一处出口。
    rule.conditions.clear();
  }
  rule.enabled = row.at(3).toInt() != 0;
  rule.expireAt = fromStored(row.at(4).toString());
  rule.note = row.at(5).toString();
  rule.schema = row.at(6).toInt();
  rule.createdAt = fromStored(row.at(7).toString());
  return rule;
}

/// 校验一条从库里读出来的规则，并把失败包装成能定位到行的话。
Result<void> validateLoaded(const Rule& rule) {
  const Result<void> valid = validateRule(rule);
  if (valid) {
    return Result<void>::ok();
  }
  return Result<void>::fail(makeError(valid.error().code,
                                      QStringLiteral("规则「%1」无法加载：%2。它可能被手工改过，"
                                                     "或者由别的版本写入 —— 请修正或删除它，"
                                                     "按错的语义封禁比这条规则不生效危险得多")
                                          .arg(rule.id, valid.error().message),
                                      valid.error().nativeCode,
                                      valid.error().nativeSource));
}

}  // namespace

SqliteRuleStore::SqliteRuleStore(Database& database) : database_(database) {}

SqliteRuleStore::~SqliteRuleStore() = default;

Result<QList<Rule>> SqliteRuleStore::list() const {
  const Result<QList<QVariantList>> rows =
      database_.query(QStringLiteral("SELECT %1 FROM rules ORDER BY created_at, id")
                          .arg(QString::fromLatin1(kColumns)));
  if (!rows) {
    return Result<QList<Rule>>::fail(
        makeError(rows.error().code,
                  QStringLiteral("读取规则列表失败：%1").arg(rows.error().message),
                  rows.error().nativeCode,
                  rows.error().nativeSource));
  }

  QList<Rule> rules;
  for (const QVariantList& row : rows.value()) {
    const Rule rule = ruleFromRow(row);
    const Result<void> valid = validateLoaded(rule);
    if (!valid) {
      return Result<QList<Rule>>::fail(valid.error());
    }
    rules.append(rule);
  }
  return Result<QList<Rule>>::ok(rules);
}

Result<Rule> SqliteRuleStore::find(const RuleId& id) const {
  const Result<QList<QVariantList>> rows = database_.query(
      QStringLiteral("SELECT %1 FROM rules WHERE id = ?").arg(QString::fromLatin1(kColumns)),
      QVariantList{id});
  if (!rows) {
    return Result<Rule>::fail(
        makeError(rows.error().code,
                  QStringLiteral("读取规则「%1」失败：%2").arg(id, rows.error().message),
                  rows.error().nativeCode,
                  rows.error().nativeSource));
  }
  if (rows.value().isEmpty()) {
    return Result<Rule>::fail(
        makeError(ErrorCode::NotFound, QStringLiteral("没有标识为「%1」的规则").arg(id)));
  }

  const Rule rule = ruleFromRow(rows.value().first());
  const Result<void> valid = validateLoaded(rule);
  if (!valid) {
    return Result<Rule>::fail(valid.error());
  }
  return Result<Rule>::ok(rule);
}

Result<void> SqliteRuleStore::upsert(const Rule& rule) {
  // 入库前校验：取值解不出来、域或方式不认识、条件为空、同域重复，一律拒绝。
  const Result<void> valid = validateRule(rule);
  if (!valid) {
    return Result<void>::fail(makeError(
        valid.error().code,
        QStringLiteral("规则「%1」没有通过校验，未写入：%2").arg(rule.id, valid.error().message),
        valid.error().nativeCode,
        valid.error().nativeSource));
  }
  if (rule.id.trimmed().isEmpty()) {
    return Result<void>::fail(
        makeError(ErrorCode::InvalidArgument, QStringLiteral("规则的标识不允许为空")));
  }

  // `created_at` 归存储层所有：更新时保留库里那个。
  // 它是「同具体度则先建的优先」的依据，被改写的话规则之间的先后会在每次编辑后漂移。
  QDateTime createdAt = rule.createdAt;
  const Result<QList<QVariantList>> existing = database_.query(
      QStringLiteral("SELECT created_at FROM rules WHERE id = ?"), QVariantList{rule.id});
  if (!existing) {
    return Result<void>::fail(
        makeError(existing.error().code,
                  QStringLiteral("写入规则前查询失败：%1").arg(existing.error().message),
                  existing.error().nativeCode,
                  existing.error().nativeSource));
  }
  if (!existing.value().isEmpty() && !existing.value().first().isEmpty()) {
    createdAt = fromStored(existing.value().first().first().toString());
  }
  if (!createdAt.isValid()) {
    createdAt = QDateTime::currentDateTimeUtc();
  }

  const QString now = toStored(QDateTime::currentDateTimeUtc());
  const Result<void> written =
      database_.execute(QStringLiteral("INSERT OR REPLACE INTO rules (%1, updated_at) "
                                       "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)")
                            .arg(QString::fromLatin1(kColumns)),
                        QVariantList{rule.id,
                                     static_cast<int>(rule.action),
                                     encodeConditions(rule.conditions),
                                     rule.enabled ? 1 : 0,
                                     toStored(rule.expireAt),
                                     rule.note,
                                     rule.schema,
                                     toStored(createdAt),
                                     now});
  if (!written) {
    return Result<void>::fail(
        makeError(written.error().code,
                  QStringLiteral("写入规则「%1」失败：%2").arg(rule.id, written.error().message),
                  written.error().nativeCode,
                  written.error().nativeSource));
  }

  notify(rule.id);
  return Result<void>::ok();
}

Result<void> SqliteRuleStore::remove(const RuleId& id) {
  // 先问一句「本来在不在」，为的是只在真的删掉了东西时通知订阅者 ——
  // 幂等调用不该让界面白刷一次。
  const Result<QList<QVariantList>> existing =
      database_.query(QStringLiteral("SELECT id FROM rules WHERE id = ?"), QVariantList{id});
  if (!existing) {
    return Result<void>::fail(
        makeError(existing.error().code,
                  QStringLiteral("删除规则前查询失败：%1").arg(existing.error().message),
                  existing.error().nativeCode,
                  existing.error().nativeSource));
  }

  const Result<void> removed =
      database_.execute(QStringLiteral("DELETE FROM rules WHERE id = ?"), QVariantList{id});
  if (!removed) {
    return Result<void>::fail(
        makeError(removed.error().code,
                  QStringLiteral("删除规则「%1」失败：%2").arg(id, removed.error().message),
                  removed.error().nativeCode,
                  removed.error().nativeSource));
  }

  if (!existing.value().isEmpty()) {
    notify(id);
  }
  return Result<void>::ok();
}

Result<void> SqliteRuleStore::clear() {
  // 批量删除要**逐条**通知：订阅者（界面、将来的 WebUI）按标识刷新，
  // 给不出一个「全部」的假标识，那会让它们没法对应到自己手上的数据。
  const Result<QList<QVariantList>> ids = database_.query(QStringLiteral("SELECT id FROM rules"));
  if (!ids) {
    return Result<void>::fail(
        makeError(ids.error().code,
                  QStringLiteral("清空规则前查询失败：%1").arg(ids.error().message),
                  ids.error().nativeCode,
                  ids.error().nativeSource));
  }

  const Result<void> removed = database_.execute(QStringLiteral("DELETE FROM rules"));
  if (!removed) {
    return Result<void>::fail(
        makeError(removed.error().code,
                  QStringLiteral("清空规则失败：%1").arg(removed.error().message),
                  removed.error().nativeCode,
                  removed.error().nativeSource));
  }

  for (const QVariantList& row : ids.value()) {
    notify(row.first().toString());
  }
  return Result<void>::ok();
}

Result<QList<Rule>> SqliteRuleStore::expired(const QDateTime& now) const {
  if (!now.isValid()) {
    return Result<QList<Rule>>::fail(
        makeError(ErrorCode::InvalidArgument, QStringLiteral("判定过期需要一个有效的时刻")));
  }

  const Result<QList<QVariantList>> rows = database_.query(
      QStringLiteral("SELECT %1 FROM rules "
                     "WHERE enabled <> 0 AND expire_at IS NOT NULL AND expire_at <= ? "
                     "ORDER BY expire_at, id")
          .arg(QString::fromLatin1(kColumns)),
      QVariantList{toStored(now)});
  if (!rows) {
    return Result<QList<Rule>>::fail(
        makeError(rows.error().code,
                  QStringLiteral("查询已到期规则失败：%1").arg(rows.error().message),
                  rows.error().nativeCode,
                  rows.error().nativeSource));
  }

  QList<Rule> rules;
  for (const QVariantList& row : rows.value()) {
    const Rule rule = ruleFromRow(row);
    const Result<void> valid = validateLoaded(rule);
    if (!valid) {
      return Result<QList<Rule>>::fail(valid.error());
    }
    rules.append(rule);
  }
  return Result<QList<Rule>>::ok(rules);
}

Result<SubscriptionId> SqliteRuleStore::subscribe(RuleChangeSink sink) {
  if (!sink) {
    return Result<SubscriptionId>::fail(
        makeError(ErrorCode::InvalidArgument,
                  QStringLiteral("订阅规则变更需要一个可调用的处理函数；"
                                 "传空会让变更悄悄丢掉")));
  }
  const SubscriptionId id = nextSubscription_++;
  sinks_.insert(id, std::move(sink));
  return Result<SubscriptionId>::ok(id);
}

Result<void> SqliteRuleStore::unsubscribe(SubscriptionId id) {
  // 重复退订视为成功：撤销流程可能被调用两次，报错只会把调用方搅乱。
  sinks_.remove(id);
  return Result<void>::ok();
}

void SqliteRuleStore::notify(const RuleId& id) const {
  // 先拷一份再回调：处理函数里可能退订或再订阅，直接在迭代中改容器是未定义行为。
  const QList<RuleChangeSink> sinks = sinks_.values();
  for (const RuleChangeSink& sink : sinks) {
    sink(id);
  }
}

}  // namespace baniphelper::core
