#include "core/capabilities.h"

namespace baniphelper::core {

const char* capabilityName(Capability capability) noexcept {
  switch (capability) {
    case Capability::FilterIPv4:
      return "FilterIPv4";
    case Capability::FilterIPv6:
      return "FilterIPv6";
    case Capability::KillTcpV4:
      return "KillTcpV4";
    case Capability::KillTcpV6:
      return "KillTcpV6";
    case Capability::TrafficStatsTcp:
      return "TrafficStatsTcp";
    case Capability::TrafficStatsUdp:
      return "TrafficStatsUdp";
    case Capability::EventDrivenConnections:
      return "EventDrivenConnections";
    case Capability::ProcessEnumeration:
      return "ProcessEnumeration";
    case Capability::SessionEvents:
      return "SessionEvents";
    case Capability::AutoStart:
      return "AutoStart";
    case Capability::SingleInstance:
      return "SingleInstance";
    case Capability::Elevation:
      return "Elevation";
    case Capability::OrphanCleanup:
      return "OrphanCleanup";
  }
  return "Unknown";
}

QString capabilityDisplayName(Capability capability) {
  switch (capability) {
    case Capability::FilterIPv4:
      return QStringLiteral("IPv4 封禁");
    case Capability::FilterIPv6:
      return QStringLiteral("IPv6 封禁");
    case Capability::KillTcpV4:
      return QStringLiteral("IPv4 立即断连");
    case Capability::KillTcpV6:
      return QStringLiteral("IPv6 立即断连");
    case Capability::TrafficStatsTcp:
      return QStringLiteral("TCP 流量统计");
    case Capability::TrafficStatsUdp:
      return QStringLiteral("UDP 流量统计");
    case Capability::EventDrivenConnections:
      return QStringLiteral("事件驱动连接发现");
    case Capability::ProcessEnumeration:
      return QStringLiteral("进程枚举");
    case Capability::SessionEvents:
      return QStringLiteral("系统会话事件");
    case Capability::AutoStart:
      return QStringLiteral("开机自启");
    case Capability::SingleInstance:
      return QStringLiteral("单实例");
    case Capability::Elevation:
      return QStringLiteral("提权");
    case Capability::OrphanCleanup:
      return QStringLiteral("残留过滤器清理");
  }
  return QStringLiteral("未知能力");
}

QString capabilityImpact(Capability capability) {
  switch (capability) {
    case Capability::FilterIPv4:
      return QStringLiteral("无法按规则阻断 IPv4 通信，本工具失去主要用途");
    case Capability::FilterIPv6:
      return QStringLiteral("无法阻断 IPv6 通信，双栈环境下封禁会被绕过");
    case Capability::KillTcpV4:
      return QStringLiteral("只能阻止新连接，已建立的 IPv4 连接要等它自行结束");
    case Capability::KillTcpV6:
      return QStringLiteral("IPv6 只能靠丢包卡死，断开不即时，资源不立即释放");
    case Capability::TrafficStatsTcp:
      return QStringLiteral("看不到 TCP 流量，只能看到连接存在与否");
    case Capability::TrafficStatsUdp:
      return QStringLiteral("UDP 只能统计流数与地址，字节数会显示为不可用");
    case Capability::EventDrivenConnections:
      return QStringLiteral("连接发现退化为轮询，短连接可能被漏掉");
    case Capability::ProcessEnumeration:
      return QStringLiteral("无法按程序圈定范围，也不能列出可选目标");
    case Capability::SessionEvents:
      return QStringLiteral("无法感知锁屏与挂起，降频与恢复时机只能靠猜");
    case Capability::AutoStart:
      return QStringLiteral("无法随系统启动，每次都要手工打开");
    case Capability::SingleInstance:
      return QStringLiteral("可能重复启动，从而重复下发过滤器");
    case Capability::Elevation:
      return QStringLiteral("无法检测权限是否足够，失败原因会难以判断");
    case Capability::OrphanCleanup:
      return QStringLiteral("上次异常退出残留的过滤器无法清理，重启后旧规则仍然生效");
  }
  return QStringLiteral("影响未知，需要补充说明");
}

}  // namespace baniphelper::core
