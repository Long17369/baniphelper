#pragma once

#include <QDateTime>
#include <QList>
#include <QString>

#include <cstdint>

#include "core/rule.h"
#include "core/types.h"

namespace baniphelper::core {

// ---------------------------------------------------------------------------
// 过滤器引擎
// ---------------------------------------------------------------------------

/// 一条规则在平台侧的下发结果。
struct AppliedRuleSummary {
  RuleId id;
  RuleAction action = RuleAction::Block;

  /// 展开出的平台过滤器条数。核心层靠它核对「展开是否与预期一致」，
  /// 不靠目测。数值为 0 且 `active` 为真属于缺陷，实现必须避免。
  int filterCount = 0;

  bool active = false;
};

/// 启动清理的结果。
struct CleanupReport {
  /// 被清掉的自家过滤器条数。
  int removedOwn = 0;

  /// 识别为**他方所有**因而跳过的条数。
  ///
  /// 这个字段必须存在且必须上报：它让「只清自家」这件事可核对。
  /// 归属判据只能是自有 provider 与 sublayer，**绝不允许按层全量清理**。
  int skippedForeign = 0;
};

// ---------------------------------------------------------------------------
// 连接监视
// ---------------------------------------------------------------------------

enum class ConnectionEventKind : std::uint8_t {
  Appeared,
  Disappeared,
  /// 已存在的连接发生了变化（例如计数更新）。
  Updated,
};

struct ConnectionEvent {
  ConnectionEventKind kind = ConnectionEventKind::Appeared;
  ConnectionSnapshot snapshot;
};

// ---------------------------------------------------------------------------
// 断连
// ---------------------------------------------------------------------------

struct KillFailure {
  ConnectionKey connection;
  /// 失败原因，中文，**不允许为空**。批量断连时逐条给出，不允许只报「部分失败」。
  QString reason;
};

struct KillReport {
  int requested = 0;
  int killed = 0;
  QList<KillFailure> failures;
};

// ---------------------------------------------------------------------------
// 会话事件
// ---------------------------------------------------------------------------

enum class SessionEventKind : std::uint8_t {
  Lock,
  Unlock,
  Suspend,
  Resume,
  Shutdown,
};

struct SessionEvent {
  SessionEventKind kind = SessionEventKind::Lock;
  QDateTime at;
};

// ---------------------------------------------------------------------------
// 路径
// ---------------------------------------------------------------------------

/// 平台约定的默认位置。只表示「默认在哪」，不保证已经存在。
struct Paths {
  /// 配置文件路径，JSON。
  QString configFile;

  /// 数据库文件路径，SQLite。
  QString databaseFile;

  /// 日志目录。
  QString logDirectory;
};

}  // namespace baniphelper::core
