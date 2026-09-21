// 配置子系统的检查（阶段一 S1.5）。
//
// 断言逐条对应 `IConfig` 注释里立的契约，以及验收标准「删掉配置文件可自动重建默认值」：
//
// 1. **写入立即落盘**，重启后读得回来；
// 2. **不合法就拒绝**，而不是夹逼到边界值或悄悄用默认值顶替；
// 3. 不认识的键、不认识的类型一律拒绝，不新增隐式配置项；
// 4. 文件被删掉、被写坏两种情况都能自愈，且**自愈动作留得下痕迹**；
// 5. 变更通知带齐键、快照与来源；退订不存在的号要报错而不是静默成功。
//
// 全部在 QTemporaryDir 里跑，不碰 `%APPDATA%`。

#include <QTest>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include "core/config.h"
#include "core/config_descriptors.h"
#include "core/error.h"
#include "core/json_config.h"

using namespace baniphelper::core;

namespace {

ConfigDescriptor descriptor(const QString& key, ConfigValueType type, const QJsonValue& fallback) {
  ConfigDescriptor item;
  item.key = key;
  item.type = type;
  item.title = QStringLiteral("标题 %1").arg(key);
  item.description = QStringLiteral("说明 %1").arg(key);
  item.defaultValue = fallback;
  return item;
}

/// 一张小而字段齐全的表：五种类型、一段范围、一组枚举候选值。
/// 用它把校验的每条分支单独压到，不必依赖真实配置表的取值。
QList<ConfigDescriptor> testDescriptors() {
  ConfigDescriptor flag = descriptor(QStringLiteral("t.flag"), ConfigValueType::Boolean, false);

  ConfigDescriptor count = descriptor(QStringLiteral("t.count"), ConfigValueType::Integer, 3.0);
  count.minValue = 1.0;
  count.maxValue = 10.0;

  ConfigDescriptor text =
      descriptor(QStringLiteral("t.text"), ConfigValueType::String, QStringLiteral("x"));

  ConfigDescriptor list =
      descriptor(QStringLiteral("t.list"), ConfigValueType::StringList, QJsonValue(QJsonArray{}));

  ConfigDescriptor mode =
      descriptor(QStringLiteral("t.mode"), ConfigValueType::Enum, QStringLiteral("a"));
  mode.enumValues = QStringList{QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")};

  return QList<ConfigDescriptor>{flag, count, text, list, mode};
}

QString fileIn(const QTemporaryDir& dir) {
  return QDir(dir.path()).absoluteFilePath(QStringLiteral("config.json"));
}

QByteArray readBytes(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    return {};
  }
  return file.readAll();
}

}  // namespace

class ConfigTest : public QObject {
  Q_OBJECT

 private slots:
  void defaultTableIsSelfConsistent();
  void missingFileIsRebuiltFromDefaults();
  void deletedFileIsRebuiltFromDefaults();
  void unknownKeyIsRejected();
  void wrongTypeIsRejected();
  void nonIntegerIsRejected();
  void outOfRangeIsRejectedWithoutClamping();
  void enumRejectsValueOutsideCandidates();
  void applyPatchIsAllOrNothing();
  void changeIsPersistedAndReloaded();
  void changeNotificationCarriesKeysSnapshotAndOrigin();
  void unsubscribeUnknownIdFails();
  void resetToDefaultsBroadcastsEmptyKeyList();
  void brokenJsonIsKeptAsideAndRebuilt();
  void invalidValueInFileFallsBackToDefaultWithNote();
  void unknownKeysInFileAreIgnoredWithNote();
  void brokenDescriptorTableIsRejected();
};

void ConfigTest::defaultTableIsSelfConsistent() {
  const QList<ConfigDescriptor> table = defaultConfigDescriptors();
  QVERIFY(!table.isEmpty());

  // 键不能重复：重复会让其中一项永远读不到默认值。
  QStringList seen;
  for (const ConfigDescriptor& item : table) {
    QVERIFY(!item.key.trimmed().isEmpty());
    QVERIFY2(!seen.contains(item.key), qPrintable(QStringLiteral("重复键：") + item.key));
    seen.append(item.key);
    // 界面直接显示这两项，留空等于给用户一个没解释的输入框。
    QVERIFY(!item.title.trimmed().isEmpty());
    QVERIFY(!item.description.trimmed().isEmpty());
    if (item.type == ConfigValueType::Enum) {
      QVERIFY(!item.enumValues.isEmpty());
    }
  }

  // 日志组是 S1.5 唯一真正被消费的一组，必须在表里。
  QVERIFY(seen.contains(QString::fromLatin1(kConfigKeyLogLevel)));
  QVERIFY(seen.contains(QString::fromLatin1(kConfigKeyLogRotateBytes)));
  QVERIFY(seen.contains(QString::fromLatin1(kConfigKeyLogRetentionDays)));
  QVERIFY(seen.contains(QString::fromLatin1(kConfigKeyLogCompressRotated)));

  // 「改一次日志级别不必重新编译」是这一组的核心价值：级别必须标成不必重启。
  bool levelNeedsRestart = true;
  for (const ConfigDescriptor& item : table) {
    if (item.key == QString::fromLatin1(kConfigKeyLogLevel)) {
      levelNeedsRestart = item.requiresRestart;
    }
  }
  QVERIFY2(!levelNeedsRestart, "日志级别标成了需要重启，那它就白做成可配置的了");
}

void ConfigTest::missingFileIsRebuiltFromDefaults() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());

  const QString path = fileIn(dir);
  QVERIFY(!QFile::exists(path));

  JsonConfig config;
  const auto opened = config.open(path, testDescriptors());
  QVERIFY2(opened.hasValue(), qPrintable(opened.hasValue() ? QString() : opened.error().message));

  // 验收项：文件不存在要能重建，而且是真的写下去了，不是只放在内存里。
  QVERIFY(QFile::exists(path));
  QCOMPARE(config.value(QStringLiteral("t.count")).value().toDouble(), 3.0);
  QVERIFY(!config.recoveryNotes().isEmpty());
}

void ConfigTest::deletedFileIsRebuiltFromDefaults() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const QString path = fileIn(dir);

  {
    JsonConfig config;
    QVERIFY(config.open(path, testDescriptors()).hasValue());
    QVERIFY(config.setValue(QStringLiteral("t.text"), QJsonValue(QStringLiteral("改过的值")))
                .hasValue());
    QCOMPARE(config.value(QStringLiteral("t.text")).value().toString(), QStringLiteral("改过的值"));
  }

  // 阶段一验收项原文：删掉配置文件可自动重建默认值。
  QVERIFY(QFile::remove(path));

  JsonConfig reopened;
  QVERIFY(reopened.open(path, testDescriptors()).hasValue());
  QCOMPARE(reopened.value(QStringLiteral("t.text")).value().toString(), QStringLiteral("x"));
  QVERIFY(QFile::exists(path));
}

void ConfigTest::unknownKeyIsRejected() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());

  JsonConfig config;
  QVERIFY(config.open(fileIn(dir), testDescriptors()).hasValue());

  const auto rejected = config.setValue(QStringLiteral("t.nothing"), QJsonValue(1));
  QVERIFY2(!rejected.hasValue(), "不认识的键必须拒绝，否则等于悄悄长出一个隐式配置项");
  QVERIFY(!rejected.error().message.isEmpty());
  QVERIFY(rejected.error().message.contains(QStringLiteral("t.nothing")));

  const auto read = config.value(QStringLiteral("t.nothing"));
  QVERIFY(!read.hasValue());
}

void ConfigTest::wrongTypeIsRejected() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());

  JsonConfig config;
  QVERIFY(config.open(fileIn(dir), testDescriptors()).hasValue());

  const auto boolAsInteger = config.setValue(QStringLiteral("t.count"), QJsonValue(true));
  QVERIFY(!boolAsInteger.hasValue());
  QVERIFY(!boolAsInteger.error().message.isEmpty());

  const auto stringAsBoolean =
      config.setValue(QStringLiteral("t.flag"), QJsonValue(QStringLiteral("true")));
  QVERIFY(!stringAsBoolean.hasValue());

  const auto numberAsString = config.setValue(QStringLiteral("t.text"), QJsonValue(5));
  QVERIFY(!numberAsString.hasValue());

  const auto stringAsList =
      config.setValue(QStringLiteral("t.list"), QJsonValue(QStringLiteral("a")));
  QVERIFY(!stringAsList.hasValue());
}

void ConfigTest::nonIntegerIsRejected() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());

  JsonConfig config;
  QVERIFY(config.open(fileIn(dir), testDescriptors()).hasValue());

  const auto fractional = config.setValue(QStringLiteral("t.count"), QJsonValue(2.5));
  QVERIFY2(!fractional.hasValue(), "整数型配置项接受了小数");
  QVERIFY(!fractional.error().message.isEmpty());
}

void ConfigTest::outOfRangeIsRejectedWithoutClamping() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());

  JsonConfig config;
  QVERIFY(config.open(fileIn(dir), testDescriptors()).hasValue());

  const auto tooBig = config.setValue(QStringLiteral("t.count"), QJsonValue(9999));
  QVERIFY(!tooBig.hasValue());
  QVERIFY(tooBig.error().message.contains(QStringLiteral("10")));

  const auto tooSmall = config.setValue(QStringLiteral("t.count"), QJsonValue(0));
  QVERIFY(!tooSmall.hasValue());

  // 关键：拒绝之后原值必须原封不动。悄悄夹逼到边界值会让用户以为设置成功了。
  QCOMPARE(config.value(QStringLiteral("t.count")).value().toDouble(), 3.0);
}

void ConfigTest::enumRejectsValueOutsideCandidates() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());

  JsonConfig config;
  QVERIFY(config.open(fileIn(dir), defaultConfigDescriptors()).hasValue());

  const auto rejected = config.setValue(QString::fromLatin1(kConfigKeyLogLevel),
                                        QJsonValue(QStringLiteral("verbose")));
  QVERIFY2(!rejected.hasValue(), "枚举型配置项接受了候选值之外的取值");
  QVERIFY(rejected.error().message.contains(QStringLiteral("verbose")));

  QVERIFY(
      config.setValue(QString::fromLatin1(kConfigKeyLogLevel), QJsonValue(QStringLiteral("debug")))
          .hasValue());
}

void ConfigTest::applyPatchIsAllOrNothing() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());

  JsonConfig config;
  QVERIFY(config.open(fileIn(dir), testDescriptors()).hasValue());

  QJsonObject patch;
  patch.insert(QStringLiteral("t.text"), QStringLiteral("新的值"));
  patch.insert(QStringLiteral("t.count"), 9999);  // 越界，整批都该被拒
  const auto rejected = config.applyPatch(patch);
  QVERIFY(!rejected.hasValue());

  // 关键：合法的项也不能落地，否则配置会停在一半。
  QCOMPARE(config.value(QStringLiteral("t.text")).value().toString(), QStringLiteral("x"));
  QCOMPARE(config.value(QStringLiteral("t.count")).value().toDouble(), 3.0);

  QJsonObject good;
  good.insert(QStringLiteral("t.text"), QStringLiteral("新的值"));
  good.insert(QStringLiteral("t.count"), 7);
  QVERIFY(config.applyPatch(good).hasValue());
  QCOMPARE(config.value(QStringLiteral("t.text")).value().toString(), QStringLiteral("新的值"));
  QCOMPARE(config.value(QStringLiteral("t.count")).value().toDouble(), 7.0);
}

void ConfigTest::changeIsPersistedAndReloaded() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const QString path = fileIn(dir);

  {
    JsonConfig config;
    QVERIFY(config.open(path, testDescriptors()).hasValue());
    QVERIFY(config.setValue(QStringLiteral("t.count"), QJsonValue(9)).hasValue());
  }

  // 写在接口上是「立即持久化」，所以这里换一个实例读同一份文件。
  JsonConfig reloaded;
  QVERIFY(reloaded.open(path, testDescriptors()).hasValue());
  QCOMPARE(reloaded.value(QStringLiteral("t.count")).value().toDouble(), 9.0);

  // 落盘形态：扁平对象，键就是描述符的键。
  const QJsonObject onDisk = QJsonDocument::fromJson(readBytes(path)).object();
  QCOMPARE(onDisk.value(QStringLiteral("t.count")).toDouble(), 9.0);
}

void ConfigTest::changeNotificationCarriesKeysSnapshotAndOrigin() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());

  JsonConfig config;
  QVERIFY(config.open(fileIn(dir), testDescriptors()).hasValue());

  int calls = 0;
  ConfigChange received;
  const auto id = config.subscribe([&](const ConfigChange& change) {
    ++calls;
    received = change;
  });
  QVERIFY(id.hasValue());

  config.setOrigin(QStringLiteral("webui"));
  QVERIFY(
      config.setValue(QStringLiteral("t.text"), QJsonValue(QStringLiteral("通知我"))).hasValue());

  QCOMPARE(calls, 1);
  QCOMPARE(received.changedKeys, QStringList{QStringLiteral("t.text")});
  QCOMPARE(received.origin, QStringLiteral("webui"));
  // 快照要带齐全部项，订阅方不必为了拿一个值再读一遍。
  QCOMPARE(received.snapshot.value(QStringLiteral("t.count")).toDouble(), 3.0);
  QCOMPARE(received.snapshot.value(QStringLiteral("t.text")).toString(), QStringLiteral("通知我"));

  QVERIFY(config.unsubscribe(id.value()).hasValue());

  // 退订之后不该再有回调。
  QVERIFY(
      config.setValue(QStringLiteral("t.text"), QJsonValue(QStringLiteral("再来一次"))).hasValue());
  QCOMPARE(calls, 1);
}

void ConfigTest::unsubscribeUnknownIdFails() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());

  JsonConfig config;
  QVERIFY(config.open(fileIn(dir), testDescriptors()).hasValue());

  const auto unknown = config.unsubscribe(12345);
  QVERIFY2(!unknown.hasValue(), "退订一个不存在的号必须报错，静默成功会让失效的订阅查不出来");
  QVERIFY(!unknown.error().message.isEmpty());

  const auto zero = config.unsubscribe(kInvalidSubscription);
  QVERIFY(!zero.hasValue());

  const auto emptySink = config.subscribe(ConfigChangeSink());
  QVERIFY(!emptySink.hasValue());
}

void ConfigTest::resetToDefaultsBroadcastsEmptyKeyList() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());

  JsonConfig config;
  QVERIFY(config.open(fileIn(dir), testDescriptors()).hasValue());
  QVERIFY(config.setValue(QStringLiteral("t.count"), QJsonValue(9)).hasValue());
  QVERIFY(config.setValue(QStringLiteral("t.text"), QJsonValue(QStringLiteral("改过"))).hasValue());

  bool called = false;
  ConfigChange received;
  const auto id = config.subscribe([&](const ConfigChange& change) {
    called = true;
    received = change;
  });
  QVERIFY(id.hasValue());

  QVERIFY(config.resetToDefaults().hasValue());

  QVERIFY(called);
  // 整体替换约定用空列表表示，而不是把每个键都列一遍。
  QVERIFY(received.changedKeys.isEmpty());
  QCOMPARE(received.snapshot.value(QStringLiteral("t.count")).toDouble(), 3.0);
  QCOMPARE(received.snapshot.value(QStringLiteral("t.text")).toString(), QStringLiteral("x"));
}

void ConfigTest::brokenJsonIsKeptAsideAndRebuilt() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const QString path = fileIn(dir);

  QFile file(path);
  QVERIFY(file.open(QIODevice::WriteOnly));
  file.write(QByteArrayLiteral("{ 这不是合法的 JSON"));
  file.close();

  JsonConfig config;
  const auto opened = config.open(path, testDescriptors());
  QVERIFY2(opened.hasValue(), "文件写坏了应当自愈，而不是让程序起不来");

  QVERIFY(!config.recoveryNotes().isEmpty());
  bool mentioned = false;
  for (const QString& note : config.recoveryNotes()) {
    mentioned = mentioned || note.contains(QStringLiteral("JSON"));
  }
  QVERIFY2(mentioned, "自愈动作没有留下说明，用户只会看到设置莫名变回去了");

  // 原内容必须保留下来：丢掉用户的设置是不可接受的。
  const QStringList bad =
      QDir(dir.path()).entryList(QStringList{QStringLiteral("config.json.bad-*")}, QDir::Files);
  QVERIFY2(!bad.isEmpty(), "写坏的原文件没有被保留");
  QVERIFY(readBytes(QDir(dir.path()).absoluteFilePath(bad.first()))
              .contains(QByteArrayLiteral("这不是合法的 JSON")));

  QCOMPARE(config.value(QStringLiteral("t.count")).value().toDouble(), 3.0);
}

void ConfigTest::invalidValueInFileFallsBackToDefaultWithNote() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const QString path = fileIn(dir);

  QFile file(path);
  QVERIFY(file.open(QIODevice::WriteOnly));
  file.write(QByteArrayLiteral(R"({"t.count": 9999, "t.text": "保留我"})"));
  file.close();

  JsonConfig config;
  QVERIFY(config.open(path, testDescriptors()).hasValue());

  QCOMPARE(config.value(QStringLiteral("t.count")).value().toDouble(), 3.0);
  // 同一次打开里的其他合法项不受影响。
  QCOMPARE(config.value(QStringLiteral("t.text")).value().toString(), QStringLiteral("保留我"));

  bool mentioned = false;
  for (const QString& note : config.recoveryNotes()) {
    mentioned = mentioned || note.contains(QStringLiteral("t.count"));
  }
  QVERIFY(mentioned);
}

void ConfigTest::unknownKeysInFileAreIgnoredWithNote() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const QString path = fileIn(dir);

  QFile file(path);
  QVERIFY(file.open(QIODevice::WriteOnly));
  file.write(QByteArrayLiteral(R"({"t.text": "ok", "future.thing": 1})"));
  file.close();

  JsonConfig config;
  QVERIFY(config.open(path, testDescriptors()).hasValue());
  QCOMPARE(config.value(QStringLiteral("t.text")).value().toString(), QStringLiteral("ok"));

  bool mentioned = false;
  for (const QString& note : config.recoveryNotes()) {
    mentioned = mentioned || note.contains(QStringLiteral("future.thing"));
  }
  QVERIFY2(mentioned, "忽略了文件里的键却没留下记录");

  // 而且不能把它写回文件：写回去等于承认了一个不存在的配置项。
  QVERIFY(!readBytes(path).contains(QByteArrayLiteral("future.thing")));
}

void ConfigTest::brokenDescriptorTableIsRejected() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());

  // 重复键
  QList<ConfigDescriptor> duplicated = testDescriptors();
  duplicated.append(
      descriptor(QStringLiteral("t.text"), ConfigValueType::String, QStringLiteral("又一份")));
  JsonConfig first;
  const auto duplicateResult = first.open(fileIn(dir), duplicated);
  QVERIFY(!duplicateResult.hasValue());
  QVERIFY(!duplicateResult.error().message.isEmpty());

  // 默认值自身不合法
  ConfigDescriptor bad = descriptor(QStringLiteral("t.bad"), ConfigValueType::Integer, 999.0);
  bad.minValue = 1.0;
  bad.maxValue = 10.0;
  JsonConfig second;
  const auto badDefault = second.open(fileIn(dir), QList<ConfigDescriptor>{bad});
  QVERIFY2(!badDefault.hasValue(), "默认值越界的描述符表应当当场被拒，而不是等重建时产出坏配置");

  // 标题或说明缺失
  ConfigDescriptor noTitle = descriptor(QStringLiteral("t.quiet"), ConfigValueType::Boolean, false);
  noTitle.description.clear();
  JsonConfig third;
  QVERIFY(!third.open(fileIn(dir), QList<ConfigDescriptor>{noTitle}).hasValue());

  // 空表
  JsonConfig fourth;
  QVERIFY(!fourth.open(fileIn(dir), QList<ConfigDescriptor>{}).hasValue());
}

QTEST_GUILESS_MAIN(ConfigTest)

#include "config_test.moc"
