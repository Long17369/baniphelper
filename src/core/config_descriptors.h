#pragma once

#include <QList>

#include "core/config.h"

namespace baniphelper::core {

/// 全部配置项的描述表。
///
/// 这张表是**配置的唯一清单**：界面与 WebUI 靠它自动生成表单，落盘也只写它定义的键。
/// 漏加一项的后果不是「少个输入框」，而是那一项根本无法被设置。
///
/// ## 只放真被消费的项
///
/// `architecture.md` 第 7.2 节列了全部规划中的配置，但其中多数所属的功能还没做
/// （`webui.*` 属 S6、`whitelist_*` 属 S4、`record_*` 属 S3）。
/// 它们**暂不放进本表**：一个能被设置却什么也不影响的开关，比没有这个开关更糟 ——
/// 用户会以为改动生效了。这与能力声明遵循同一条原则：做不到就不要声明。
///
/// 每一项随其功能落地时加进来，并在阶段文档里记一笔。
[[nodiscard]] QList<ConfigDescriptor> defaultConfigDescriptors();

/// 日志组的键。集中在这里，避免各处手写字符串拼错。
inline constexpr char kConfigKeyLogLevel[] = "log.level";
inline constexpr char kConfigKeyLogRotateBytes[] = "log.rotate_bytes";
inline constexpr char kConfigKeyLogRetentionDays[] = "log.retention_days";
inline constexpr char kConfigKeyLogCompressRotated[] = "log.compress_rotated";

/// 退出行为组的键。
///
/// `architecture.md` 第 7.2 节里它写作 `revoke_on_exit`，这里按 S1.5 定下的
/// 「键名带命名空间前缀」规则改为 `shutdown.revoke_on_exit`，改由已在第 3.4 节记过。
inline constexpr char kConfigKeyShutdownRevokeOnExit[] = "shutdown.revoke_on_exit";

}  // namespace baniphelper::core
