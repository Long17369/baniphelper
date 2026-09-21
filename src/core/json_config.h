#pragma once

#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QString>
#include <QStringList>

#include <memory>

#include "core/config.h"
#include "core/iconfig.h"
#include "core/result.h"

namespace baniphelper::core {

/// 配置的 JSON 实现。
///
/// ## 磁盘形态与 `snapshot()` 完全一致
///
/// 都是**一个扁平对象，键就是描述符里的点号键**，例如 `"log.level": "info"`。
///
/// 刻意不做「磁盘上嵌套、接口上扁平」的转换：两种表示一旦并存，
/// 导出、备份和磁盘内容就会悄悄不一致，而排查问题时最怕这种不一致。
/// 代价是手工编辑时不如嵌套好看 —— 但这个文件的主要编辑入口是界面，不是编辑器。
///
/// ## 两条自愈路径都留痕
///
/// 打开配置时有两类坏情况，都不会静默处理：
///
/// - **文件内容不是合法 JSON**：原文件改名为 `<文件名>.bad-<时间戳>` 保留，
///   然后按默认值重建。丢掉用户的设置是不可接受的，直接拒绝启动也不行。
/// - **文件里有描述符没定义的键**：忽略，并把键名记进 `recoveryNotes()`。
///   这类键多半来自更高版本或手工编辑，写回去等于承认一个不存在的配置项。
///
/// 自愈动作会写进 `recoveryNotes()`，调用方应当把它记进日志 ——
/// 否则用户只会看到「设置怎么都变回去了」而找不到原因。
class JsonConfig final : public IConfig {
 public:
  JsonConfig();
  ~JsonConfig() override;

  JsonConfig(const JsonConfig&) = delete;
  JsonConfig& operator=(const JsonConfig&) = delete;

  /// 打开配置。
  ///
  /// 文件不存在时按 `descriptors` 的默认值重建并立刻落盘，
  /// 这是阶段一验收项「删掉配置文件可自动重建默认值」的落点。
  [[nodiscard]] Result<void> open(const QString& filePath, QList<ConfigDescriptor> descriptors);

  /// 打开过程中发生过的自愈动作。空列表表示一切正常。
  [[nodiscard]] QStringList recoveryNotes() const;

  /// 声明后续写操作的来源，写进变更广播的 `origin`。
  ///
  /// `IConfig::setValue` 没有来源参数，这是冻结接口留下的一个缺口。
  /// 真实调用方（S6 的 WebUI）需要区分来源时必须先解决它 ——
  /// 长期靠一个具体类的扩展方法顶着，等于接口说了不算。
  /// 见 `docs/phases/01-foundation.md` 第 3.4 节。
  void setOrigin(const QString& origin);

  /// 配置文件的落盘路径。未打开时为空串。
  [[nodiscard]] QString filePath() const;

  /// 已经打开过。
  [[nodiscard]] bool isOpen() const;

  // ---- IConfig ----

  [[nodiscard]] QJsonObject snapshot() const override;
  [[nodiscard]] Result<QJsonValue> value(const QString& key) const override;
  [[nodiscard]] Result<void> setValue(const QString& key, const QJsonValue& value) override;
  [[nodiscard]] Result<void> applyPatch(const QJsonObject& patch) override;
  [[nodiscard]] Result<void> resetToDefaults() override;
  [[nodiscard]] QList<ConfigDescriptor> descriptors() const override;
  [[nodiscard]] Result<SubscriptionId> subscribe(ConfigChangeSink sink) override;
  [[nodiscard]] Result<void> unsubscribe(SubscriptionId id) override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace baniphelper::core
