#pragma once

#include <QString>

#include <cstdint>
#include <memory>

#include "core/result.h"

namespace baniphelper::core {

/// 日志级别。数值按严重程度递增，比较时直接用大小即可。
enum class LogLevel : std::uint8_t {
  Trace = 0,
  Debug = 1,
  Info = 2,
  Warn = 3,
  Error = 4,
  /// 表示「进程即将终止」的严重程度。写入它本身不会终止进程，是否终止由调用方决定。
  Fatal = 5,
};

/// 全部级别的枚举清单。
///
/// 用途与 `kAllErrorCodes` 相同：**新增级别时必须同步加到这里**，
/// 否则测试里的覆盖检查会失败。漏补的后果是日志里出现问号级别的记录，
/// 而按级别过滤正是日志存在的意义。
inline constexpr LogLevel kAllLogLevels[] = {
    LogLevel::Trace,
    LogLevel::Debug,
    LogLevel::Info,
    LogLevel::Warn,
    LogLevel::Error,
    LogLevel::Fatal,
};

/// 级别名字，写进日志用，例如 `INFO`。未知值返回 `?`。
[[nodiscard]] QString logLevelName(LogLevel level);

/// 解析级别名字，大小写不敏感，`WARNING` 这类常见变体也认。
///
/// 认不出来时返回失败，而不是退回默认级别：配置里把级别写错了，
/// 应该当场看见，而不是表面正常、实际按别的级别在跑。
[[nodiscard]] Result<LogLevel> parseLogLevel(const QString& text);

/// 日志文件的滚动与保留参数。
struct LogOptions {
  /// 日志目录，绝对路径。不存在时由日志器创建。
  QString directory;

  /// 文件名主干。当前文件是 `<主干>.log`，滚动出去的是 `<主干>-<时间戳>.log[.z]`。
  QString baseName = QStringLiteral("baniphelper");

  /// 当前文件写到这个字节数就滚动。
  qint64 rotateBytes = 4 * 1024 * 1024;

  /// 只保留最近这么多天，按文件修改时间判断。
  int retentionDays = 7;

  /// 滚动出去的文件是否压缩。压缩用 zlib，扩展名 `.log.z`。
  ///
  /// 开启后磁盘占用大致是纯文本的十分之一上下，这是「只按时间保留、不设量上限」
  /// 这个取舍成立的前提：压缩后一周的量通常只有几 MB。
  bool compressRotated = true;

  /// 低于此级别的消息直接丢弃。
  LogLevel level = LogLevel::Info;
};

/// 日志器。所有公开方法都是线程安全的。
///
/// 写失败不抛异常、也不中断业务：日志是观测手段，不是业务本身。
/// 但**打开失败必须让调用方看见**，那说明路径或权限有问题，
/// 继续跑下去会变成「什么都记不下来却毫无提示」。
class Logger {
 public:
  Logger();
  ~Logger();

  Logger(const Logger&) = delete;
  Logger& operator=(const Logger&) = delete;

  [[nodiscard]] Result<void> open(const LogOptions& options);
  void close();
  [[nodiscard]] bool isOpen() const;

  void setLevel(LogLevel level);
  [[nodiscard]] LogLevel level() const;

  /// 写一条日志。低于级别门槛的、以及日志器没打开时的调用，都会被丢弃。
  void write(LogLevel level, const QString& message);

  /// 把缓冲区刷到磁盘。`write` 本身逐条刷盘，这里用于关闭前的显式确认。
  [[nodiscard]] Result<void> flush();

  /// 当前日志文件的绝对路径。未打开时返回空串。
  [[nodiscard]] QString currentFile() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// 打开进程级日志器。已经打开过时返回失败，而不是悄悄替换掉原来的。
[[nodiscard]] Result<void> openLogging(const LogOptions& options);

/// 关闭进程级日志器并刷盘。可重复调用。
void closeLogging();

/// 进程级日志器是否已就绪。
[[nodiscard]] bool isLoggingReady();

/// 写一条进程级日志。未初始化时直接丢弃。
///
/// 为什么不在这里上报失败：调用点遍布业务代码，为了「日志还没起来」让每处都判错是本末倒置。
/// 真正的失误在 `openLogging` 那一步已经报过，不该让业务替它反复买单。
void logWrite(LogLevel level, const QString& message);

/// 调整进程级日志器的级别。
///
/// 应当在 `openLogging` 之后调用：打开时会按传入的 `LogOptions` 覆盖级别，
/// 之前设的值不会保留。
void setLoggingLevel(LogLevel level);

/// 把 Qt 自身的消息接管进日志。
///
/// 为什么必须做：程序是 GUI 子系统、没有控制台，Qt 内部消息
/// （QML 加载失败、插件缺失、字体回退、资源找不到）原本直接消失。
/// 排查时看不到这些，就只能靠重跑一遍猜。
void installQtMessageHandler();

}  // namespace baniphelper::core
