#include "core/log.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QStringList>
#include <QtGlobal>

#include <cstdlib>
#include <utility>

namespace baniphelper::core {
namespace {

/// 日志行的时间戳格式。带毫秒，方便和事件日志、抓包时间对齐。
constexpr char kLineStampFormat[] = "yyyy-MM-dd HH:mm:ss.zzz";

/// 滚动文件名里的时间戳格式。只到秒，可读性优先。
constexpr char kFileStampFormat[] = "yyyyMMdd-HHmmss";

/// 进程号。整个进程生命周期不变，取一次即可。
qint64 processId() {
  static const qint64 id = QCoreApplication::applicationPid();
  return id;
}

/// 拼一条日志行。
///
/// 级别名字补齐到 5 个字符，这样纵向读日志时级别是齐的。
QByteArray formatLine(LogLevel level, const QString& message) {
  const QString stamp = QDateTime::currentDateTime().toString(QLatin1String(kLineStampFormat));
  QString line =
      QStringLiteral("[%1] [%2] [pid %3] %4\n")
          .arg(stamp, logLevelName(level).leftJustified(5), QString::number(processId()), message);
  return line.toUtf8();
}

/// 生成不与现有文件冲突的滚动文件名。
///
/// 同一秒内连续滚动两次是常态（日志量异常时就会发生，测试里也刻意制造），
/// 因此重名时加序号后缀，而不是覆盖掉前一个 —— 覆盖等于丢日志。
QString uniqueRotatedPath(const QDir& dir,
                          const QString& baseName,
                          const QDateTime& stamp,
                          bool compressed) {
  const QString suffix = compressed ? QStringLiteral(".log.z") : QStringLiteral(".log");
  const QString prefix =
      baseName + QLatin1Char('-') + stamp.toString(QLatin1String(kFileStampFormat));
  QString candidate = dir.absoluteFilePath(prefix + suffix);
  int index = 1;
  while (QFile::exists(candidate)) {
    candidate = dir.absoluteFilePath(prefix + QLatin1Char('-') + QString::number(index) + suffix);
    ++index;
  }
  return candidate;
}

/// 列出滚动出去的全部日志文件（含已压缩的）。
QStringList listRotatedFiles(const QDir& dir, const QString& baseName) {
  const QStringList filters{baseName + QStringLiteral("-*.log"),
                            baseName + QStringLiteral("-*.log.z")};
  const QFileInfoList infos = dir.entryInfoList(filters, QDir::Files, QDir::Time);
  QStringList paths;
  paths.reserve(infos.size());
  for (const QFileInfo& info : infos) {
    paths.append(info.absoluteFilePath());
  }
  return paths;
}

}  // namespace

/// 日志器的全部可变状态。放在 `.cpp` 里，头文件就不必暴露 QFile 与 QMutex。
struct Logger::Impl {
  /// 串行化全部操作。日志可能来自任意线程（Qt 消息处理器、未来的 WFP 回调）。
  QMutex mutex;

  LogOptions options;
  QFile file;
  QString currentPath;
  qint64 currentBytes = 0;
  bool open = false;

  /// 滚动：把当前文件改名出去（必要时压缩），再开一个新的空文件。
  [[nodiscard]] Result<void> rotate();

  /// 删掉超过保留期的滚动文件。
  void cleanupExpired();

  /// 尽力追加一段原始字节。用于「滚动失败」这类自身故障的留痕。
  void appendBestEffort(const QString& text);
};

Result<void> Logger::Impl::rotate() {
  const QDateTime now = QDateTime::currentDateTime();
  const QDir dir(options.directory);

  if (file.isOpen()) {
    file.flush();
    file.close();
  }

  const QString plainPath = uniqueRotatedPath(dir, options.baseName, now, false);
  if (!QFile::rename(currentPath, plainPath)) {
    // 改名失败：把当前文件重新打开继续写。宁可文件超出滚动阈值，也不能把日志写丢。
    file.setFileName(currentPath);
    const bool reopened = file.open(QIODevice::ReadWrite | QIODevice::Append | QIODevice::Text);
    return Result<void>::fail(makeError(
        ErrorCode::Io,
        QStringLiteral("滚动日志时改名失败：%1%2")
            .arg(plainPath, reopened ? QString() : QStringLiteral("（且重新打开失败）"))));
  }

  if (options.compressRotated) {
    QFile plain(plainPath);
    if (plain.open(QIODevice::ReadOnly)) {
      const QByteArray raw = plain.readAll();
      plain.close();

      const QString packedPath = uniqueRotatedPath(dir, options.baseName, now, true);
      QFile packed(packedPath);
      if (packed.open(QIODevice::WriteOnly)) {
        const qint64 written = packed.write(qCompress(raw));
        packed.close();
        // 压缩确实写成功才删原件。半截的压缩文件比不压缩更糟：既占地方又读不出来。
        if (written > 0) {
          QFile::remove(plainPath);
        }
      }
    }
    // 压缩任何一步失败都保留未压缩的原件，不报错也不删 —— 日志没丢就是成功。
  }

  file.setFileName(currentPath);
  if (!file.open(QIODevice::ReadWrite | QIODevice::Truncate | QIODevice::Text)) {
    open = false;
    return Result<void>::fail(
        makeError(ErrorCode::Io, QStringLiteral("滚动后无法创建新的日志文件：") + currentPath));
  }
  currentBytes = 0;
  cleanupExpired();
  return Result<void>::ok();
}

void Logger::Impl::cleanupExpired() {
  if (options.retentionDays <= 0) {
    return;
  }
  const QDateTime threshold = QDateTime::currentDateTime().addDays(-options.retentionDays);
  const QDir dir(options.directory);
  const QStringList files = listRotatedFiles(dir, options.baseName);
  for (const QString& path : files) {
    if (QFileInfo(path).lastModified() < threshold) {
      QFile::remove(path);
    }
  }
}

void Logger::Impl::appendBestEffort(const QString& text) {
  if (!file.isOpen()) {
    return;
  }
  const QByteArray line = formatLine(LogLevel::Warn, text);
  if (file.write(line) > 0) {
    currentBytes += line.size();
  }
}

Logger::Logger() : impl_(std::make_unique<Impl>()) {}

Logger::~Logger() {
  close();
}

Result<void> Logger::open(const LogOptions& options) {
  QMutexLocker locker(&impl_->mutex);

  if (impl_->open) {
    return Result<void>::fail(
        makeError(ErrorCode::AlreadyExists, QStringLiteral("日志器已经打开，不能重复打开")));
  }
  if (options.directory.trimmed().isEmpty()) {
    return Result<void>::fail(
        makeError(ErrorCode::InvalidArgument, QStringLiteral("日志目录为空")));
  }
  if (options.baseName.trimmed().isEmpty()) {
    return Result<void>::fail(
        makeError(ErrorCode::InvalidArgument, QStringLiteral("日志文件名主干为空")));
  }
  if (options.rotateBytes <= 0) {
    return Result<void>::fail(
        makeError(ErrorCode::InvalidArgument,
                  QStringLiteral("滚动阈值必须为正数，当前是 %1 字节").arg(options.rotateBytes)));
  }
  if (options.retentionDays <= 0) {
    return Result<void>::fail(
        makeError(ErrorCode::InvalidArgument,
                  QStringLiteral("保留天数必须为正数，当前是 %1 天").arg(options.retentionDays)));
  }

  const QDir dir(QDir::cleanPath(options.directory));
  if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
    return Result<void>::fail(
        makeError(ErrorCode::Io, QStringLiteral("无法创建日志目录：") + dir.absolutePath()));
  }

  impl_->options = options;
  impl_->options.directory = dir.absolutePath();
  impl_->currentPath = dir.absoluteFilePath(options.baseName + QStringLiteral(".log"));
  impl_->file.setFileName(impl_->currentPath);
  if (!impl_->file.open(QIODevice::ReadWrite | QIODevice::Append | QIODevice::Text)) {
    return Result<void>::fail(
        makeError(ErrorCode::Io, QStringLiteral("无法打开日志文件：") + impl_->currentPath));
  }

  impl_->currentBytes = impl_->file.size();
  impl_->open = true;

  // 上次退出时可能正好卡在阈值上，先滚动一次再开始写，否则新日志会被塞进一个满文件里。
  if (impl_->currentBytes >= impl_->options.rotateBytes) {
    const Result<void> rotated = impl_->rotate();
    if (!rotated) {
      return rotated;
    }
  } else {
    impl_->cleanupExpired();
  }

  return Result<void>::ok();
}

void Logger::close() {
  QMutexLocker locker(&impl_->mutex);
  if (!impl_->open) {
    return;
  }
  if (impl_->file.isOpen()) {
    impl_->file.flush();
    impl_->file.close();
  }
  impl_->open = false;
  impl_->currentBytes = 0;
}

bool Logger::isOpen() const {
  QMutexLocker locker(&impl_->mutex);
  return impl_->open;
}

void Logger::setLevel(LogLevel level) {
  QMutexLocker locker(&impl_->mutex);
  impl_->options.level = level;
}

LogLevel Logger::level() const {
  QMutexLocker locker(&impl_->mutex);
  return impl_->options.level;
}

void Logger::write(LogLevel level, const QString& message) {
  QMutexLocker locker(&impl_->mutex);
  if (!impl_->open) {
    return;
  }
  if (level < impl_->options.level) {
    return;
  }

  const QByteArray line = formatLine(level, message);

  // 空文件不滚动：否则一条超长消息会把刚建好的文件立刻滚走，形成空转。
  if (impl_->currentBytes > 0 && impl_->currentBytes + line.size() > impl_->options.rotateBytes) {
    const Result<void> rotated = impl_->rotate();
    if (!rotated) {
      // 滚动失败这件事本身要留在日志里。写不进去也没办法，日志不能反过来中断业务。
      impl_->appendBestEffort(QStringLiteral("日志滚动失败：") + rotated.error().message);
    }
  }

  if (impl_->file.write(line) > 0) {
    impl_->currentBytes += line.size();
  }
  impl_->file.flush();
}

Result<void> Logger::flush() {
  QMutexLocker locker(&impl_->mutex);
  if (!impl_->open || !impl_->file.isOpen()) {
    return Result<void>::ok();
  }
  if (!impl_->file.flush()) {
    return Result<void>::fail(
        makeError(ErrorCode::Io, QStringLiteral("刷盘失败：") + impl_->file.errorString()));
  }
  return Result<void>::ok();
}

QString Logger::currentFile() const {
  QMutexLocker locker(&impl_->mutex);
  return impl_->open ? impl_->currentPath : QString();
}

QString logLevelName(LogLevel level) {
  switch (level) {
    case LogLevel::Trace:
      return QStringLiteral("TRACE");
    case LogLevel::Debug:
      return QStringLiteral("DEBUG");
    case LogLevel::Info:
      return QStringLiteral("INFO");
    case LogLevel::Warn:
      return QStringLiteral("WARN");
    case LogLevel::Error:
      return QStringLiteral("ERROR");
    case LogLevel::Fatal:
      return QStringLiteral("FATAL");
  }
  return QStringLiteral("?");
}

Result<LogLevel> parseLogLevel(const QString& text) {
  const QString normalized = text.trimmed().toUpper();
  if (normalized.isEmpty()) {
    return Result<LogLevel>::fail(
        makeError(ErrorCode::InvalidArgument, QStringLiteral("日志级别为空")));
  }
  for (const LogLevel level : kAllLogLevels) {
    if (logLevelName(level) == normalized) {
      return Result<LogLevel>::ok(level);
    }
  }
  // 常见变体顺手认掉，代价极低，能省掉一轮「配置写对了却说级别非法」的困惑。
  if (normalized == QLatin1String("WARNING")) {
    return Result<LogLevel>::ok(LogLevel::Warn);
  }
  if (normalized == QLatin1String("ERR")) {
    return Result<LogLevel>::ok(LogLevel::Error);
  }
  return Result<LogLevel>::fail(makeError(
      ErrorCode::InvalidArgument,
      QStringLiteral("无法识别的日志级别：%1，可用值为 trace、debug、info、warn、error、fatal")
          .arg(text)));
}

namespace {

/// 进程级日志器。
///
/// 用函数内静态量而不是全局对象，是为了让构造顺序无关紧要：
/// 任何一个翻译单元第一次用到它时才构造，避免静态初始化顺序问题。
Logger& processLogger() {
  static Logger logger;
  return logger;
}

}  // namespace

Result<void> openLogging(const LogOptions& options) {
  return processLogger().open(options);
}

void closeLogging() {
  processLogger().close();
}

bool isLoggingReady() {
  return processLogger().isOpen();
}

void logWrite(LogLevel level, const QString& message) {
  processLogger().write(level, message);
}

void setLoggingLevel(LogLevel level) {
  processLogger().setLevel(level);
}

void installQtMessageHandler() {
  qInstallMessageHandler(
      [](QtMsgType type, const QMessageLogContext& context, const QString& message) {
        LogLevel level = LogLevel::Info;
        switch (type) {
          case QtDebugMsg:
            level = LogLevel::Debug;
            break;
          case QtInfoMsg:
            level = LogLevel::Info;
            break;
          case QtWarningMsg:
            level = LogLevel::Warn;
            break;
          case QtCriticalMsg:
            level = LogLevel::Error;
            break;
          case QtFatalMsg:
            level = LogLevel::Fatal;
            break;
        }

        QString text = message;
        if (context.file != nullptr) {
          text += QStringLiteral("  (") + QString::fromUtf8(context.file) + QLatin1Char(':') +
                  QString::number(context.line) + QLatin1Char(')');
        }
        logWrite(level, QStringLiteral("[Qt] ") + text);

        // Qt 的契约是：装了处理器之后，致命消息由处理器负责终止。
        // 这里照做，否则 qFatal 会变成「记了一行然后继续跑」，比不记还危险。
        if (type == QtFatalMsg) {
          std::abort();
        }
      });
}

}  // namespace baniphelper::core
