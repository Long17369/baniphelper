#pragma once

#include "core/result.h"
#include "platform/api/ipaths.h"

namespace baniphelper::core {

/// 配置与数据目录的 Windows 实现，全部落在 `%APPDATA%\BanIPHelper` 之下。
///
/// 取目录时先问系统（`SHGetFolderPathW(CSIDL_APPDATA)`），再退回环境变量 `%APPDATA%`：
/// 环境变量可能被改掉或整个缺失，而系统给出的答案是权威的。
/// 两条路都失败才算失败，且错误信息会写明两条都试过了。
///
/// 不用更新的 `SHGetKnownFolderPath`：它要 `FOLDERID_RoamingAppData` 这个 GUID 符号，
/// 而 MinGW 只给声明不给定义，得额外引一个库进去。为一个目录路径多挂依赖不划算。
///
/// 不用 `%LOCALAPPDATA%`：配置与数据库要跟着用户漫游，日志也放在一起便于一次带走排查。
/// 代价是漫游配置下日志会参与同步，这是已知的取舍（见 `docs/phases/01-foundation.md` S1.4）。
class WinPaths final : public IPaths {
 public:
  WinPaths() = default;

  WinPaths(const WinPaths&) = delete;
  WinPaths& operator=(const WinPaths&) = delete;

  [[nodiscard]] Result<Paths> resolve() const override;
  [[nodiscard]] Result<void> ensureDirectories() override;
};

}  // namespace baniphelper::core
