#pragma once

#include <QString>

#include "core/capabilities.h"
#include "core/icapabilities.h"
#include "core/result.h"

namespace baniphelper::core {

/// 取用某项能力前的统一入口。
///
/// **任何依赖某项能力的代码路径都必须先经过这里**，不允许「先试着调用，失败了再说」：
/// 那样做出来的效果是用户点下去才知道不行，而且失败原因常常在中途被吞掉。
///
/// `operation` 是给用户看的动作名，例如「中断 IPv6 连接」，失败时会被写进说明里。
/// 因此调用方要写具体动作，不要写「操作失败」这种没有信息的词。
///
/// 返回值保证：失败必定是 `ErrorCode::NotSupported`，且 `message` 非空且可读。
/// 即使传入的 `ICapabilities` 实现没给出原因，这里也会补上通用的后果说明，
/// 保证不存在「失败了但说不清为什么」的路径。
[[nodiscard]] Result<void> requireCapability(const ICapabilities& capabilities,
                                             Capability capability,
                                             const QString& operation);

}  // namespace baniphelper::core
