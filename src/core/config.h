#pragma once

#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace baniphelper::core {

/// 配置值的类型。用于校验与界面自动生成表单。
enum class ConfigValueType : std::uint8_t {
  Boolean,
  Integer,
  String,
  StringList,
  /// 取值限定在 `ConfigDescriptor::enumValues` 之内。
  Enum,
};

/// 一项配置的描述。
///
/// 配置项必须自带描述，原因有两条：一是 WebUI 要求「一切可配置」，
/// 靠硬编码表单一定漏项；二是配置项散落在代码里读写而不集中描述，就没人能回答
/// 「一共有哪些配置」。
struct ConfigDescriptor {
  /// 点号分层，例如 `webui.listen_port`。
  QString key;

  ConfigValueType type = ConfigValueType::String;

  /// 中文标题与说明，界面与 WebUI 直接显示，因此不允许为空。
  QString title;
  QString description;

  /// 默认值。删掉配置文件后按它重建。
  QJsonValue defaultValue;

  /// 数值型配置的取值范围。
  std::optional<double> minValue;
  std::optional<double> maxValue;

  /// `Enum` 型配置的候选值。
  QStringList enumValues;

  /// 改动后是否需要重启才生效。界面据此提示，避免用户以为没生效。
  bool requiresRestart = false;

  /// 是否属于敏感信息（例如 WebUI 的令牌）。为真时界面与日志都必须遮蔽。
  bool securitySensitive = false;
};

/// 一次配置变更。
struct ConfigChange {
  /// 被改动的键。整体替换（例如恢复默认值）时为空列表。
  QStringList changedKeys;

  /// 变更后的全量快照，避免订阅方为了拿一个值再去读一遍。
  QJsonObject snapshot;

  /// 变更来源，供审计：`ui`、`webui`、`core`、`import`。
  QString origin;
};

}  // namespace baniphelper::core
