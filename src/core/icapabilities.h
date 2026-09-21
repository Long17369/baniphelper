#pragma once

#include <QList>
#include <QString>

#include "core/capabilities.h"

namespace baniphelper::core {

/// 能力查询。
///
/// **这是「某个能力能不能用」的唯一判据来源。** 其他层不得自行猜测，
/// 也不得用 try 一下再回退的方式探测：那等于把「不支持」当成异常路径，
/// 界面就无法在操作前给出禁用状态。
///
/// 契约要点：
///
/// - `isSupported` 的返回值与后端实际行为必须一致，不允许声明了却做不到；
/// - 不支持的能力，其对应接口方法必须返回 `ErrorCode::NotSupported`，
///   且错误信息要说清「什么做不到」与「为什么」；
/// - 已支持的能力，`unsupportedReason` 返回空字符串。
class ICapabilities {
 public:
  ICapabilities() = default;
  virtual ~ICapabilities() = default;

  ICapabilities(const ICapabilities&) = delete;
  ICapabilities& operator=(const ICapabilities&) = delete;
  ICapabilities(ICapabilities&&) = delete;
  ICapabilities& operator=(ICapabilities&&) = delete;

  [[nodiscard]] virtual bool isSupported(Capability capability) const = 0;

  /// 已声明支持的全部能力，供设置页与故障排查展示。
  [[nodiscard]] virtual QList<Capability> supported() const = 0;

  /// 不支持的原因；已支持时为空字符串。
  [[nodiscard]] virtual QString unsupportedReason(Capability capability) const = 0;

  /// 后端标识，例如 `win`、`linux`、`memory`。写进日志，便于判断跑的是哪个后端。
  [[nodiscard]] virtual QString backendName() const = 0;
};

}  // namespace baniphelper::core
