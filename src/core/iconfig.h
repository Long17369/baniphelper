#pragma once

#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QString>

#include <functional>

#include "core/config.h"
#include "core/result.h"
#include "core/types.h"

namespace baniphelper::core {

/// 配置变更的回调。可能在非界面线程上被调用，消费方自行切线程。
using ConfigChangeSink = std::function<void(const ConfigChange&)>;

/// 配置读写。
///
/// 契约要点：
///
/// - **写操作立即持久化**。不存在「改完还要记得保存」的中间态，
///   否则崩溃一次就会丢掉用户刚做的设置；
/// - 写入前必须按描述符校验类型与取值范围，**不合法就拒绝，不要夹逼到边界值**；
/// - 不认识的键返回 `InvalidArgument`，不允许悄悄新增一个隐式配置项；
/// - 删掉配置文件后，下一次读取必须能用默认值重建，且默认值来自 `descriptors()`；
/// - 界面侧、WebUI 侧共用同一份配置**单一数据源**，两侧都只通过本接口读写。
class IConfig {
 public:
  IConfig() = default;
  virtual ~IConfig() = default;

  IConfig(const IConfig&) = delete;
  IConfig& operator=(const IConfig&) = delete;
  IConfig(IConfig&&) = delete;
  IConfig& operator=(IConfig&&) = delete;

  /// 全量快照，供 WebUI、导出与备份使用。
  [[nodiscard]] virtual QJsonObject snapshot() const = 0;

  [[nodiscard]] virtual Result<QJsonValue> value(const QString& key) const = 0;

  [[nodiscard]] virtual Result<void> setValue(const QString& key, const QJsonValue& value) = 0;

  /// 批量写入，用于 WebUI 一次提交多项改动。
  /// 语义是**全成功或全不改**：中途有一项不合法就整体拒绝，不能让配置停在一半。
  [[nodiscard]] virtual Result<void> applyPatch(const QJsonObject& patch) = 0;

  /// 恢复默认值。属于危险操作，调用方必须先走二次确认与审计。
  [[nodiscard]] virtual Result<void> resetToDefaults() = 0;

  /// 全部配置项的描述。界面与 WebUI 靠它自动生成表单，
  /// 目的是让「新增一项配置」不会漏掉任何一侧。
  [[nodiscard]] virtual QList<ConfigDescriptor> descriptors() const = 0;

  [[nodiscard]] virtual Result<SubscriptionId> subscribe(ConfigChangeSink sink) = 0;

  [[nodiscard]] virtual Result<void> unsubscribe(SubscriptionId id) = 0;
};

}  // namespace baniphelper::core
