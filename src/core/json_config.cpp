#include "core/json_config.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QMutex>
#include <QMutexLocker>
#include <QSaveFile>

#include <cmath>
#include <utility>

namespace baniphelper::core {
namespace {

/// 校验一个值是否满足描述符。
///
/// 只有「是」与「否」两种结论，**不做任何夹逼**：把 99999 悄悄改成上限值，
/// 会让用户以为自己设置成功了。范围不合就明确拒绝，并说清允许的范围。
Result<void> validateValue(const ConfigDescriptor& descriptor, const QJsonValue& value) {
  const QString key = descriptor.key;

  switch (descriptor.type) {
    case ConfigValueType::Boolean:
      if (!value.isBool()) {
        return Result<void>::fail(
            makeError(ErrorCode::InvalidArgument, QStringLiteral("配置项 %1 需要布尔值").arg(key)));
      }
      return Result<void>::ok();

    case ConfigValueType::Integer: {
      if (!value.isDouble()) {
        return Result<void>::fail(
            makeError(ErrorCode::InvalidArgument, QStringLiteral("配置项 %1 需要整数").arg(key)));
      }
      const double number = value.toDouble();
      if (std::floor(number) != number) {
        return Result<void>::fail(makeError(
            ErrorCode::InvalidArgument,
            QStringLiteral("配置项 %1 需要整数，当前是 %2").arg(key, QString::number(number))));
      }
      if (descriptor.minValue.has_value() && number < descriptor.minValue.value()) {
        return Result<void>::fail(makeError(
            ErrorCode::InvalidArgument,
            QStringLiteral("配置项 %1 不得小于 %2，当前是 %3")
                .arg(key, QString::number(descriptor.minValue.value()), QString::number(number))));
      }
      if (descriptor.maxValue.has_value() && number > descriptor.maxValue.value()) {
        return Result<void>::fail(makeError(
            ErrorCode::InvalidArgument,
            QStringLiteral("配置项 %1 不得大于 %2，当前是 %3")
                .arg(key, QString::number(descriptor.maxValue.value()), QString::number(number))));
      }
      return Result<void>::ok();
    }

    case ConfigValueType::String:
      if (!value.isString()) {
        return Result<void>::fail(
            makeError(ErrorCode::InvalidArgument, QStringLiteral("配置项 %1 需要字符串").arg(key)));
      }
      return Result<void>::ok();

    case ConfigValueType::StringList: {
      if (!value.isArray()) {
        return Result<void>::fail(makeError(ErrorCode::InvalidArgument,
                                            QStringLiteral("配置项 %1 需要字符串数组").arg(key)));
      }
      const QJsonArray array = value.toArray();
      for (const QJsonValue& item : array) {
        if (!item.isString()) {
          return Result<void>::fail(
              makeError(ErrorCode::InvalidArgument,
                        QStringLiteral("配置项 %1 的数组里出现了非字符串元素").arg(key)));
        }
      }
      return Result<void>::ok();
    }

    case ConfigValueType::Enum: {
      if (!value.isString()) {
        return Result<void>::fail(
            makeError(ErrorCode::InvalidArgument, QStringLiteral("配置项 %1 需要字符串").arg(key)));
      }
      const QString text = value.toString();
      if (!descriptor.enumValues.contains(text)) {
        return Result<void>::fail(
            makeError(ErrorCode::InvalidArgument,
                      QStringLiteral("配置项 %1 只接受 %2，当前是 %3")
                          .arg(key, descriptor.enumValues.join(QStringLiteral("、")), text)));
      }
      return Result<void>::ok();
    }
  }

  return Result<void>::fail(
      makeError(ErrorCode::Internal,
                QStringLiteral("配置项 %1 的值类型未归类，属于描述符表未覆盖的情况").arg(key)));
}

/// 检查描述符表本身是否自洽。
///
/// 描述符写错的后果很隐蔽：键重复会让其中一项永远读不到默认值，
/// 默认值本身不合法会让「删掉配置重建」直接产出坏配置。
/// 与其等运行时出怪现象，不如在打开时一次查清。
Result<void> checkDescriptors(const QList<ConfigDescriptor>& descriptors) {
  QStringList seen;
  for (const ConfigDescriptor& descriptor : descriptors) {
    if (descriptor.key.trimmed().isEmpty()) {
      return Result<void>::fail(
          makeError(ErrorCode::InvalidArgument, QStringLiteral("描述符表里有一项的键为空")));
    }
    if (seen.contains(descriptor.key)) {
      return Result<void>::fail(
          makeError(ErrorCode::InvalidArgument,
                    QStringLiteral("描述符表里有重复的键：%1").arg(descriptor.key)));
    }
    seen.append(descriptor.key);

    if (descriptor.title.trimmed().isEmpty() || descriptor.description.trimmed().isEmpty()) {
      return Result<void>::fail(
          makeError(ErrorCode::InvalidArgument,
                    QStringLiteral("配置项 %1 缺少标题或说明，界面会显示一个没解释的输入框")
                        .arg(descriptor.key)));
    }
    if (descriptor.type == ConfigValueType::Enum && descriptor.enumValues.isEmpty()) {
      return Result<void>::fail(
          makeError(ErrorCode::InvalidArgument,
                    QStringLiteral("枚举型配置项 %1 没有候选值").arg(descriptor.key)));
    }

    const Result<void> valid = validateValue(descriptor, descriptor.defaultValue);
    if (!valid) {
      return Result<void>::fail(makeError(ErrorCode::InvalidArgument,
                                          QStringLiteral("配置项 %1 的默认值不合法：%2")
                                              .arg(descriptor.key, valid.error().message)));
    }
  }
  return Result<void>::ok();
}

}  // namespace

struct JsonConfig::Impl {
  mutable QMutex mutex;

  QString path;
  QList<ConfigDescriptor> descriptors;
  QHash<QString, QJsonValue> values;

  QHash<SubscriptionId, ConfigChangeSink> sinks;
  SubscriptionId nextSubscription = 1;

  QString origin = QStringLiteral("core");
  QStringList notes;
  bool open = false;

  [[nodiscard]] const ConfigDescriptor* descriptor(const QString& key) const {
    for (const ConfigDescriptor& candidate : descriptors) {
      if (candidate.key == key) {
        return &candidate;
      }
    }
    return nullptr;
  }

  [[nodiscard]] QJsonObject buildSnapshot() const {
    QJsonObject object;
    for (const ConfigDescriptor& candidate : descriptors) {
      object.insert(candidate.key, values.value(candidate.key, candidate.defaultValue));
    }
    return object;
  }

  /// 原子落盘。
  ///
  /// 用 `QSaveFile` 而不是「直接写目标文件」：后者在写一半时崩溃会留下半个 JSON，
  /// 下次启动就得走「文件坏了」的自愈路径 —— 那是给极端情况准备的，不该被自己制造。
  [[nodiscard]] Result<void> persist() {
    const QDir parent = QFileInfo(path).absoluteDir();
    if (!parent.exists() && !parent.mkpath(QStringLiteral("."))) {
      return Result<void>::fail(
          makeError(ErrorCode::Io, QStringLiteral("无法创建配置目录：") + parent.absolutePath()));
    }

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
      return Result<void>::fail(
          makeError(ErrorCode::Io,
                    QStringLiteral("无法写入配置文件：%1（%2）").arg(path, file.errorString())));
    }

    const QByteArray bytes = QJsonDocument(buildSnapshot()).toJson(QJsonDocument::Indented);
    if (file.write(bytes) != bytes.size()) {
      file.cancelWriting();
      return Result<void>::fail(
          makeError(ErrorCode::Io,
                    QStringLiteral("写入配置文件不完整：%1（%2）").arg(path, file.errorString())));
    }
    if (!file.commit()) {
      return Result<void>::fail(
          makeError(ErrorCode::Io,
                    QStringLiteral("提交配置文件失败：%1（%2）").arg(path, file.errorString())));
    }
    return Result<void>::ok();
  }
};

JsonConfig::JsonConfig() : impl_(std::make_unique<Impl>()) {}

JsonConfig::~JsonConfig() = default;

Result<void> JsonConfig::open(const QString& filePath, QList<ConfigDescriptor> descriptors) {
  QMutexLocker locker(&impl_->mutex);

  if (impl_->open) {
    return Result<void>::fail(
        makeError(ErrorCode::AlreadyExists, QStringLiteral("配置已经打开，不能重复打开")));
  }
  if (filePath.trimmed().isEmpty()) {
    return Result<void>::fail(
        makeError(ErrorCode::InvalidArgument, QStringLiteral("配置文件路径为空")));
  }
  if (descriptors.isEmpty()) {
    return Result<void>::fail(makeError(
        ErrorCode::InvalidArgument,
        QStringLiteral("配置描述符表为空。没有描述符就没有任何一项配置可读写，多半是调用方漏传")));
  }

  const Result<void> tableOk = checkDescriptors(descriptors);
  if (!tableOk) {
    return tableOk;
  }

  impl_->descriptors = std::move(descriptors);
  impl_->path = QDir::cleanPath(filePath);
  impl_->notes.clear();
  impl_->values.clear();

  QJsonObject fromFile;
  bool needPersist = false;

  if (QFileInfo::exists(impl_->path)) {
    QFile file(impl_->path);
    if (!file.open(QIODevice::ReadOnly)) {
      return Result<void>::fail(makeError(
          ErrorCode::Io,
          QStringLiteral("无法读取配置文件：%1（%2）").arg(impl_->path, file.errorString())));
    }
    const QByteArray raw = file.readAll();
    file.close();

    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(raw, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
      const QString stamp =
          QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
      const QString badPath = impl_->path + QStringLiteral(".bad-") + stamp;
      const bool moved = QFile::rename(impl_->path, badPath);
      impl_->notes.append(
          QStringLiteral("配置文件不是合法的 JSON 对象（%1），已保留为 %2 并按默认值重建")
              .arg(parseError.errorString(),
                   moved ? badPath : QStringLiteral("（改名也失败了，原内容已被覆盖）")));
      needPersist = true;
    } else {
      fromFile = document.object();
    }
  } else {
    impl_->notes.append(QStringLiteral("配置文件不存在，已按默认值重建：%1").arg(impl_->path));
    needPersist = true;
  }

  QStringList unknownKeys;
  for (const ConfigDescriptor& descriptor : impl_->descriptors) {
    if (!fromFile.contains(descriptor.key)) {
      impl_->values.insert(descriptor.key, descriptor.defaultValue);
      needPersist = true;
      continue;
    }
    const QJsonValue candidate = fromFile.value(descriptor.key);
    const Result<void> valid = validateValue(descriptor, candidate);
    if (!valid) {
      impl_->values.insert(descriptor.key, descriptor.defaultValue);
      impl_->notes.append(QStringLiteral("配置项 %1 的值不合法（%2），已改回默认值")
                              .arg(descriptor.key, valid.error().message));
      needPersist = true;
      continue;
    }
    impl_->values.insert(descriptor.key, candidate);
  }

  for (const QString& key : fromFile.keys()) {
    if (impl_->descriptor(key) == nullptr) {
      unknownKeys.append(key);
    }
  }
  if (!unknownKeys.isEmpty()) {
    impl_->notes.append(QStringLiteral("忽略了无法识别的配置键：%1").arg(unknownKeys.join(", ")));
  }

  impl_->open = true;
  if (needPersist) {
    const Result<void> saved = impl_->persist();
    if (!saved) {
      impl_->open = false;
      return saved;
    }
  }
  return Result<void>::ok();
}

QStringList JsonConfig::recoveryNotes() const {
  QMutexLocker locker(&impl_->mutex);
  return impl_->notes;
}

void JsonConfig::setOrigin(const QString& origin) {
  QMutexLocker locker(&impl_->mutex);
  impl_->origin = origin;
}

QString JsonConfig::filePath() const {
  QMutexLocker locker(&impl_->mutex);
  return impl_->path;
}

bool JsonConfig::isOpen() const {
  QMutexLocker locker(&impl_->mutex);
  return impl_->open;
}

QJsonObject JsonConfig::snapshot() const {
  QMutexLocker locker(&impl_->mutex);
  if (!impl_->open) {
    return {};
  }
  return impl_->buildSnapshot();
}

Result<QJsonValue> JsonConfig::value(const QString& key) const {
  QMutexLocker locker(&impl_->mutex);
  if (!impl_->open) {
    return Result<QJsonValue>::fail(
        makeError(ErrorCode::Internal, QStringLiteral("配置还没有打开")));
  }
  if (impl_->descriptor(key) == nullptr) {
    return Result<QJsonValue>::fail(
        makeError(ErrorCode::InvalidArgument, QStringLiteral("未知的配置项：%1").arg(key)));
  }
  return Result<QJsonValue>::ok(impl_->values.value(key));
}

Result<void> JsonConfig::setValue(const QString& key, const QJsonValue& newValue) {
  QList<ConfigChangeSink> sinks;
  ConfigChange change;

  {
    QMutexLocker locker(&impl_->mutex);
    if (!impl_->open) {
      return Result<void>::fail(makeError(ErrorCode::Internal, QStringLiteral("配置还没有打开")));
    }
    const ConfigDescriptor* descriptor = impl_->descriptor(key);
    if (descriptor == nullptr) {
      return Result<void>::fail(
          makeError(ErrorCode::InvalidArgument, QStringLiteral("未知的配置项：%1").arg(key)));
    }
    const Result<void> valid = validateValue(*descriptor, newValue);
    if (!valid) {
      return valid;
    }

    const QJsonValue previous = impl_->values.value(key);
    impl_->values.insert(key, newValue);

    const Result<void> saved = impl_->persist();
    if (!saved) {
      // 落盘失败就回滚内存值。让内存与磁盘各说一套，下次重启时用户会觉得设置「自己变回去了」。
      impl_->values.insert(key, previous);
      return saved;
    }

    change.changedKeys = QStringList{key};
    change.snapshot = impl_->buildSnapshot();
    change.origin = impl_->origin;
    sinks = impl_->sinks.values();
  }

  // 在锁外回调：订阅方很可能反过来读配置，持锁调用就是死锁。
  for (const ConfigChangeSink& sink : sinks) {
    sink(change);
  }
  return Result<void>::ok();
}

Result<void> JsonConfig::applyPatch(const QJsonObject& patch) {
  QList<ConfigChangeSink> sinks;
  ConfigChange change;

  {
    QMutexLocker locker(&impl_->mutex);
    if (!impl_->open) {
      return Result<void>::fail(makeError(ErrorCode::Internal, QStringLiteral("配置还没有打开")));
    }
    if (patch.isEmpty()) {
      return Result<void>::fail(
          makeError(ErrorCode::InvalidArgument, QStringLiteral("批量写入的内容为空")));
    }

    // 先全量校验，再统一落地。中途发现不合法就整体拒绝，
    // 否则配置会停在一半，而调用方还以为是原子操作。
    for (auto it = patch.constBegin(); it != patch.constEnd(); ++it) {
      const ConfigDescriptor* descriptor = impl_->descriptor(it.key());
      if (descriptor == nullptr) {
        return Result<void>::fail(makeError(ErrorCode::InvalidArgument,
                                            QStringLiteral("未知的配置项：%1").arg(it.key())));
      }
      const Result<void> valid = validateValue(*descriptor, it.value());
      if (!valid) {
        return valid;
      }
    }

    QHash<QString, QJsonValue> previous;
    QStringList changedKeys;
    for (auto it = patch.constBegin(); it != patch.constEnd(); ++it) {
      previous.insert(it.key(), impl_->values.value(it.key()));
      impl_->values.insert(it.key(), it.value());
      changedKeys.append(it.key());
    }

    const Result<void> saved = impl_->persist();
    if (!saved) {
      for (auto it = previous.constBegin(); it != previous.constEnd(); ++it) {
        impl_->values.insert(it.key(), it.value());
      }
      return saved;
    }

    change.changedKeys = changedKeys;
    change.snapshot = impl_->buildSnapshot();
    change.origin = impl_->origin;
    sinks = impl_->sinks.values();
  }

  for (const ConfigChangeSink& sink : sinks) {
    sink(change);
  }
  return Result<void>::ok();
}

Result<void> JsonConfig::resetToDefaults() {
  QList<ConfigChangeSink> sinks;
  ConfigChange change;

  {
    QMutexLocker locker(&impl_->mutex);
    if (!impl_->open) {
      return Result<void>::fail(makeError(ErrorCode::Internal, QStringLiteral("配置还没有打开")));
    }

    const QHash<QString, QJsonValue> previous = impl_->values;
    for (const ConfigDescriptor& descriptor : impl_->descriptors) {
      impl_->values.insert(descriptor.key, descriptor.defaultValue);
    }

    const Result<void> saved = impl_->persist();
    if (!saved) {
      impl_->values = previous;
      return saved;
    }

    // 整体替换，所以变更键列表为空 —— 由 ConfigChange 的注释约定。
    change.changedKeys = QStringList();
    change.snapshot = impl_->buildSnapshot();
    change.origin = impl_->origin;
    sinks = impl_->sinks.values();
  }

  for (const ConfigChangeSink& sink : sinks) {
    sink(change);
  }
  return Result<void>::ok();
}

QList<ConfigDescriptor> JsonConfig::descriptors() const {
  QMutexLocker locker(&impl_->mutex);
  return impl_->descriptors;
}

Result<SubscriptionId> JsonConfig::subscribe(ConfigChangeSink sink) {
  QMutexLocker locker(&impl_->mutex);
  if (!sink) {
    return Result<SubscriptionId>::fail(
        makeError(ErrorCode::InvalidArgument, QStringLiteral("变更回调为空")));
  }
  const SubscriptionId id = impl_->nextSubscription;
  ++impl_->nextSubscription;
  impl_->sinks.insert(id, std::move(sink));
  return Result<SubscriptionId>::ok(id);
}

Result<void> JsonConfig::unsubscribe(SubscriptionId id) {
  QMutexLocker locker(&impl_->mutex);
  if (id == kInvalidSubscription) {
    return Result<void>::fail(
        makeError(ErrorCode::InvalidArgument, QStringLiteral("订阅号 0 不是有效订阅")));
  }
  if (impl_->sinks.remove(id) == 0) {
    // 静默成功会让「订阅已经失效」这件事永远查不出来。
    return Result<void>::fail(
        makeError(ErrorCode::NotFound, QStringLiteral("没有这个订阅号：%1").arg(id)));
  }
  return Result<void>::ok();
}

}  // namespace baniphelper::core
