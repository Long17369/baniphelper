#pragma once

#include <QString>

#include <cstdint>

namespace baniphelper::core {

/// 能力位。
///
/// 各平台能做到的事并不一样（例如某些平台缺少等价的连接中断手段），
/// 因此每个后端在启动时必须声明一个能力位集合。核心层据此调整行为，
/// 界面据此**显式禁用**做不到的操作。
///
/// 铁律：**声明与实际行为必须一致**。做不到就如实声明为不支持，
/// 不允许「声明支持、调用时报错」，也不允许「静默失败」。
enum class Capability : std::uint32_t {
  /// 按规则下发 IPv4 过滤器。
  FilterIPv4 = 1U << 0,
  /// 按规则下发 IPv6 过滤器。
  FilterIPv6 = 1U << 1,
  /// 立即中断已建立的 IPv4 TCP 连接。
  KillTcpV4 = 1U << 2,
  /// 立即中断已建立的 IPv6 TCP 连接。
  KillTcpV6 = 1U << 3,
  /// 读取 TCP 字节计数。
  TrafficStatsTcp = 1U << 4,
  /// 读取 UDP 字节计数。做不到时只能统计流数，**字节数必须显示为不可用**。
  TrafficStatsUdp = 1U << 5,
  /// 连接发现是事件驱动的。不具备时上层退化为轮询，并如实标注精度差异。
  EventDrivenConnections = 1U << 6,
  /// 能枚举进程并解析可执行文件标识。
  ProcessEnumeration = 1U << 7,
  /// 能订阅会话级系统事件（锁屏、挂起、关机）。
  SessionEvents = 1U << 8,
  /// 能注册与撤销开机自启。
  AutoStart = 1U << 9,
  /// 能保证单实例。
  SingleInstance = 1U << 10,
  /// 能检测并申请提权。
  Elevation = 1U << 11,
  /// 能列举并清理上次运行残留的自有过滤器。
  OrphanCleanup = 1U << 12,
};

/// 供日志与调试使用的英文代号。
[[nodiscard]] const char* capabilityName(Capability capability) noexcept;

/// 供界面显示能力的中文名。
[[nodiscard]] QString capabilityDisplayName(Capability capability);

/// 不支持时给用户看的补充说明：说清「缺了这个会怎样」。
/// 界面禁用某项功能时必须带上它，否则用户只看到一个灰按钮，无法判断严重性。
[[nodiscard]] QString capabilityImpact(Capability capability);

}  // namespace baniphelper::core
