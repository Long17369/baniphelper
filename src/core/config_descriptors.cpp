#include "core/config_descriptors.h"

#include <QJsonValue>
#include <QString>
#include <QStringList>

#include "core/log.h"

namespace baniphelper::core {
namespace {

ConfigDescriptor makeDescriptor(const QString& key,
                                ConfigValueType type,
                                const QString& title,
                                const QString& description,
                                const QJsonValue& defaultValue) {
  ConfigDescriptor descriptor;
  descriptor.key = key;
  descriptor.type = type;
  descriptor.title = title;
  descriptor.description = description;
  descriptor.defaultValue = defaultValue;
  return descriptor;
}

/// 日志组。
///
/// 这四个值在 S1.4 里是写死的常量，加进来之后才真正可调 ——
/// 也就是「改一次日志级别不必重新编译一次」。
QList<ConfigDescriptor> logDescriptors() {
  QList<ConfigDescriptor> list;

  ConfigDescriptor level =
      makeDescriptor(QString::fromLatin1(kConfigKeyLogLevel),
                     ConfigValueType::Enum,
                     QStringLiteral("日志级别"),
                     QStringLiteral("低于该级别的日志会被丢弃。排查问题时改成 debug，"
                                    "平时用 info，否则日志会被流水账淹没。"),
                     QJsonValue(QStringLiteral("info")));
  level.enumValues = QStringList{QStringLiteral("trace"),
                                 QStringLiteral("debug"),
                                 QStringLiteral("info"),
                                 QStringLiteral("warn"),
                                 QStringLiteral("error"),
                                 QStringLiteral("fatal")};
  // 级别是运行时可调的，不必重启：这也是变更广播最直接的一个用例。
  level.requiresRestart = false;
  list.append(level);

  ConfigDescriptor rotate =
      makeDescriptor(QString::fromLatin1(kConfigKeyLogRotateBytes),
                     ConfigValueType::Integer,
                     QStringLiteral("单个日志文件的滚动阈值（字节）"),
                     QStringLiteral("当前文件写到这个大小就滚动，滚动后立刻压缩。"
                                    "默认 4 MB。"),
                     QJsonValue(static_cast<double>(4 * 1024 * 1024)));
  rotate.minValue = 64.0 * 1024.0;
  rotate.maxValue = 64.0 * 1024.0 * 1024.0;
  rotate.requiresRestart = true;
  list.append(rotate);

  ConfigDescriptor retention =
      makeDescriptor(QString::fromLatin1(kConfigKeyLogRetentionDays),
                     ConfigValueType::Integer,
                     QStringLiteral("日志保留天数"),
                     QStringLiteral("超过这个天数的滚动文件会被删除。默认 7 天。"),
                     QJsonValue(7.0));
  retention.minValue = 1.0;
  retention.maxValue = 365.0;
  retention.requiresRestart = true;
  list.append(retention);

  ConfigDescriptor compress =
      makeDescriptor(QString::fromLatin1(kConfigKeyLogCompressRotated),
                     ConfigValueType::Boolean,
                     QStringLiteral("滚动后的日志是否压缩"),
                     QStringLiteral("压缩用 zlib，扩展名变为 .log.z，占用约为纯文本的十分之一。"
                                    "关掉后可以直接用编辑器打开历史日志。"),
                     QJsonValue(true));
  compress.requiresRestart = true;
  list.append(compress);

  return list;
}

}  // namespace

QList<ConfigDescriptor> defaultConfigDescriptors() {
  return logDescriptors();
}

}  // namespace baniphelper::core
