#pragma once

#include <QString>

#include "core/platform_backend.h"

namespace baniphelper::core {

/// 用内存实现拼出一个完整的后端。
///
/// 两个消费方：契约测试（作为真实后端的对照物），
/// 以及界面开发（不必先有真实实现就能跑起来，见阶段五的风险与对策）。
///
/// `singleInstanceName` 留空表示用默认名。
[[nodiscard]] PlatformBackend makeMemoryBackend(const QString& singleInstanceName = QString());

}  // namespace baniphelper::core
