// 日志子系统的检查（阶段一 S1.4）。
//
// 断言逐条对应验收标准「日志可滚动且不会无限增长」。因此重点有三处：
//
// 1. **滚动真的发生了**，而且滚出去的内容没丢（压缩后还能解回来）；
// 2. **超过保留期的文件真的被删了**，靠改文件时间制造「一周前」，不靠等待；
// 3. **级别过滤真的在过滤**，而不是「看着像过滤了」。
//
// 全部在 QTemporaryDir 里跑，不碰用户目录。

#include <QTest>

#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <iterator>

#include "core/error.h"
#include "core/log.h"

using namespace baniphelper::core;

namespace {

/// 列出目录里滚动出去的日志文件，按名字排序。
QStringList rotatedFileNames(const QString& directory, const QString& baseName) {
  const QDir dir(directory);
  return dir.entryList({baseName + QStringLiteral("-*.log"), baseName + QStringLiteral("-*.log.z")},
                       QDir::Files,
                       QDir::Name);
}

QByteArray readAllBytes(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    return {};
  }
  return file.readAll();
}

/// 把目录里所有滚动文件的内容拼起来。用来断言「滚出去的东西没丢」。
QByteArray allRotatedBytes(const QString& directory, const QString& baseName) {
  const QDir dir(directory);
  QByteArray collected;
  const QStringList names = rotatedFileNames(directory, baseName);
  for (const QString& name : names) {
    const QString path = dir.absoluteFilePath(name);
    if (name.endsWith(QLatin1String(".log.z"))) {
      collected += qUncompress(readAllBytes(path));
    } else {
      collected += readAllBytes(path);
    }
  }
  return collected;
}

/// 造一条长度可控的日志，便于把文件撑到滚动阈值。
QString filler(const QString& marker, int width) {
  QString text = marker;
  while (text.size() < width) {
    text += QLatin1Char('.');
  }
  return text;
}

}  // namespace

class LogTest : public QObject {
  Q_OBJECT

 private slots:
  void levelNamesCoverEveryLevel();
  void parseLevelAcceptsKnownSpellings();
  void parseLevelRejectsUnknown();
  void openRejectsBadOptions();
  void openCreatesMissingDirectory();
  void openTwiceFails();
  void rotatesWhenFileReachesThreshold();
  void rotatedFileIsCompressedAndRecoverable();
  void levelFilterDropsBelowThreshold();
  void expiredFilesAreDeleted();
  void qtMessagesAreCapturedIntoLog();
};

void LogTest::levelNamesCoverEveryLevel() {
  // 级别漏补名字的后果是日志里出现问号级别的记录，而按级别过滤正是日志存在的意义。
  QStringList names;
  for (const LogLevel level : kAllLogLevels) {
    const QString name = logLevelName(level);
    QVERIFY2(name != QLatin1String("?"), "有级别没有名字，说明 kAllLogLevels 与 logLevelName 脱节");
    QVERIFY2(!names.contains(name), "两个级别用了同一个名字，解析级别时会分不清");
    names.append(name);
  }
  QCOMPARE(names.size(), static_cast<int>(std::size(kAllLogLevels)));
}

void LogTest::parseLevelAcceptsKnownSpellings() {
  const auto info = parseLogLevel(QStringLiteral("info"));
  QVERIFY(info.hasValue());
  QCOMPARE(info.value(), LogLevel::Info);

  QVERIFY(parseLogLevel(QStringLiteral("  Trace ")).value() == LogLevel::Trace);
  QVERIFY(parseLogLevel(QStringLiteral("WARNING")).value() == LogLevel::Warn);
  QVERIFY(parseLogLevel(QStringLiteral("err")).value() == LogLevel::Error);
  QVERIFY(parseLogLevel(QStringLiteral("FATAL")).value() == LogLevel::Fatal);
}

void LogTest::parseLevelRejectsUnknown() {
  // 「写错了却悄悄按默认级别跑」是会让人白查半天的那种失败，必须明确报错。
  const auto bad = parseLogLevel(QStringLiteral("verbose"));
  QVERIFY(!bad.hasValue());
  QVERIFY(!bad.error().message.isEmpty());
  QVERIFY(bad.error().message.contains(QStringLiteral("verbose")));

  const auto empty = parseLogLevel(QStringLiteral("   "));
  QVERIFY(!empty.hasValue());
  QVERIFY(!empty.error().message.isEmpty());
}

void LogTest::openRejectsBadOptions() {
  QTemporaryDir temp;
  QVERIFY(temp.isValid());

  Logger logger;

  LogOptions emptyDirectory;
  emptyDirectory.directory = QString();
  const auto noDirectory = logger.open(emptyDirectory);
  QVERIFY(!noDirectory.hasValue());
  QVERIFY(!noDirectory.error().message.isEmpty());

  LogOptions zeroRotate;
  zeroRotate.directory = temp.path();
  zeroRotate.rotateBytes = 0;
  const auto badRotate = logger.open(zeroRotate);
  QVERIFY(!badRotate.hasValue());
  QVERIFY(badRotate.error().message.contains(QStringLiteral("0")));

  LogOptions zeroDays;
  zeroDays.directory = temp.path();
  zeroDays.retentionDays = 0;
  const auto badDays = logger.open(zeroDays);
  QVERIFY(!badDays.hasValue());
  QVERIFY(!badDays.error().message.isEmpty());
}

void LogTest::openCreatesMissingDirectory() {
  QTemporaryDir temp;
  QVERIFY(temp.isValid());

  const QString nested = temp.path() + QStringLiteral("/a/b/logs");
  QVERIFY(!QDir(nested).exists());

  Logger logger;
  LogOptions options;
  options.directory = nested;
  const auto opened = logger.open(options);
  QVERIFY2(opened.hasValue(), qPrintable(opened.hasValue() ? QString() : opened.error().message));
  QVERIFY(QDir(nested).exists());
  QVERIFY(logger.isOpen());
  QVERIFY(logger.currentFile().startsWith(QDir::cleanPath(nested)));
}

void LogTest::openTwiceFails() {
  QTemporaryDir temp;
  QVERIFY(temp.isValid());

  Logger logger;
  LogOptions options;
  options.directory = temp.path();
  QVERIFY(logger.open(options).hasValue());

  const auto again = logger.open(options);
  QVERIFY2(!again.hasValue(), "重复打开应当失败，而不是把原来的日志器悄悄换掉");
  QVERIFY(!again.error().message.isEmpty());
}

void LogTest::rotatesWhenFileReachesThreshold() {
  QTemporaryDir temp;
  QVERIFY(temp.isValid());

  Logger logger;
  LogOptions options;
  options.directory = temp.path();
  options.rotateBytes = 2048;
  options.compressRotated = false;
  options.level = LogLevel::Debug;
  QVERIFY(logger.open(options).hasValue());

  const QString first = filler(QStringLiteral("FIRST-LINE-MARKER"), 120);
  const QString last = filler(QStringLiteral("LAST-LINE-MARKER"), 120);
  logger.write(LogLevel::Info, first);
  for (int index = 0; index < 60; ++index) {
    logger.write(LogLevel::Debug, filler(QStringLiteral("MID-%1").arg(index), 120));
  }
  logger.write(LogLevel::Info, last);

  const QStringList rotated = rotatedFileNames(temp.path(), options.baseName);
  QVERIFY2(!rotated.isEmpty(), "写了约 7 KB 而阈值是 2 KB，应当已经滚动过至少一次");

  // 滚出去的内容不能丢：首尾两条都要能在「滚动文件 + 当前文件」里找到。
  QByteArray everything = allRotatedBytes(temp.path(), options.baseName);
  everything += readAllBytes(logger.currentFile());
  QVERIFY(everything.contains(first.toUtf8()));
  QVERIFY(everything.contains(last.toUtf8()));

  // 当前文件不该无限涨：它必须小于「阈值 + 一行」的量级。
  const qint64 currentSize = QFileInfo(logger.currentFile()).size();
  QVERIFY2(currentSize < options.rotateBytes + 512,
           qPrintable(QStringLiteral("当前文件 %1 字节，阈值 %2 字节")
                          .arg(currentSize)
                          .arg(options.rotateBytes)));

  logger.close();
}

void LogTest::rotatedFileIsCompressedAndRecoverable() {
  QTemporaryDir temp;
  QVERIFY(temp.isValid());

  Logger logger;
  LogOptions options;
  options.directory = temp.path();
  options.rotateBytes = 1024;
  options.compressRotated = true;
  options.level = LogLevel::Debug;
  QVERIFY(logger.open(options).hasValue());

  logger.write(LogLevel::Info, filler(QStringLiteral("SURVIVES-COMPRESSION"), 200));
  for (int index = 0; index < 30; ++index) {
    logger.write(LogLevel::Debug, filler(QStringLiteral("MID-%1").arg(index), 200));
  }
  logger.close();

  const QStringList names = rotatedFileNames(temp.path(), options.baseName);
  QVERIFY(!names.isEmpty());

  QDir dir(temp.path());
  int compressedCount = 0;
  for (const QString& name : names) {
    if (name.endsWith(QLatin1String(".log.z"))) {
      ++compressedCount;
      // 压缩件必须是能解回来的，否则等于把日志写丢了。
      const QByteArray raw = qUncompress(readAllBytes(dir.absoluteFilePath(name)));
      QVERIFY2(!raw.isEmpty(), "压缩文件解不回来");
    }
  }
  QVERIFY2(compressedCount > 0, "开了压缩却一个 .log.z 都没有");
  QVERIFY(allRotatedBytes(temp.path(), options.baseName)
              .contains(QByteArrayLiteral("SURVIVES-COMPRESSION")));
}

void LogTest::levelFilterDropsBelowThreshold() {
  QTemporaryDir temp;
  QVERIFY(temp.isValid());

  Logger logger;
  LogOptions options;
  options.directory = temp.path();
  options.level = LogLevel::Warn;
  QVERIFY(logger.open(options).hasValue());

  logger.write(LogLevel::Debug, QStringLiteral("SHOULD-NOT-APPEAR-DEBUG"));
  logger.write(LogLevel::Info, QStringLiteral("SHOULD-NOT-APPEAR-INFO"));
  logger.write(LogLevel::Error, QStringLiteral("SHOULD-APPEAR-ERROR"));

  const QByteArray content = readAllBytes(logger.currentFile());
  QVERIFY(!content.contains(QByteArrayLiteral("SHOULD-NOT-APPEAR-DEBUG")));
  QVERIFY(!content.contains(QByteArrayLiteral("SHOULD-NOT-APPEAR-INFO")));
  QVERIFY(content.contains(QByteArrayLiteral("SHOULD-APPEAR-ERROR")));

  // 门槛本身也要能改，否则配置项就成了摆设。
  logger.setLevel(LogLevel::Debug);
  logger.write(LogLevel::Debug, QStringLiteral("NOW-VISIBLE-DEBUG"));
  QVERIFY(readAllBytes(logger.currentFile()).contains(QByteArrayLiteral("NOW-VISIBLE-DEBUG")));
}

void LogTest::expiredFilesAreDeleted() {
  QTemporaryDir temp;
  QVERIFY(temp.isValid());

  const QString baseName = QStringLiteral("baniphelper");
  QDir dir(temp.path());

  // 造一个「一周前」的文件：直接改修改时间，不靠等待，也不依赖系统时钟。
  const QString oldPath = dir.absoluteFilePath(baseName + QStringLiteral("-20200101-000000.log"));
  const QString freshPath = dir.absoluteFilePath(baseName + QStringLiteral("-20260921-000000.log"));
  for (const QString& path : {oldPath, freshPath}) {
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QByteArrayLiteral("x"));
    file.close();
  }
  {
    QFile old(oldPath);
    QVERIFY(old.open(QIODevice::ReadWrite));
    QVERIFY(old.setFileTime(QDateTime::currentDateTime().addDays(-30),
                            QFileDevice::FileModificationTime));
    old.close();
  }

  Logger logger;
  LogOptions options;
  options.directory = temp.path();
  options.retentionDays = 7;
  QVERIFY(logger.open(options).hasValue());

  QVERIFY2(!QFile::exists(oldPath), "超过保留期的日志没有被删掉，日志就会一直堆下去");
  QVERIFY2(QFile::exists(freshPath), "保留期内的日志被误删了");
  logger.close();
}

void LogTest::qtMessagesAreCapturedIntoLog() {
  // 这条走的是**进程级**日志器与真实的 Qt 消息处理器。
  //
  // 为什么单独测：接管 Qt 消息是「没有控制台」这件事的唯一补救，
  // 而正常启动时往往一条 Qt 消息都不产生，端到端跑一遍证明不了这条路通。
  // 所以在这里主动造一条 qWarning，确认它真的落进了文件、且级别映射正确。
  QTemporaryDir temp;
  QVERIFY(temp.isValid());

  LogOptions options;
  options.directory = temp.path();
  options.level = LogLevel::Debug;
  QVERIFY(options.baseName == QStringLiteral("baniphelper"));

  QVERIFY(openLogging(options).hasValue());
  installQtMessageHandler();

  qWarning().noquote() << QStringLiteral("QT-HANDLER-MARKER");
  qDebug().noquote() << QStringLiteral("QT-DEBUG-MARKER");

  closeLogging();

  const QString path =
      QDir(temp.path()).absoluteFilePath(options.baseName + QStringLiteral(".log"));
  const QByteArray content = readAllBytes(path);

  QVERIFY2(content.contains(QByteArrayLiteral("QT-HANDLER-MARKER")),
           "Qt 自己的消息没有进日志，GUI 子系统下它们就彻底消失了");
  QVERIFY2(content.contains(QByteArrayLiteral("[Qt] ")),
           "Qt 消息缺少来源标记，事后分不清哪条是自己写的、哪条是 Qt 报的");
  QVERIFY2(content.contains(QByteArrayLiteral("[WARN ]")), "qWarning 应当映射成 WARN 级别");
  QVERIFY2(content.contains(QByteArrayLiteral("[DEBUG]")), "qDebug 应当映射成 DEBUG 级别");
}

QTEST_GUILESS_MAIN(LogTest)

#include "log_test.moc"
