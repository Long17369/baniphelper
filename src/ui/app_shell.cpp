#include "ui/app_shell.h"

#include <QAction>
#include <QCoreApplication>
#include <QEvent>
#include <QIcon>
#include <QJsonValue>
#include <QMenu>
#include <QSignalBlocker>
#include <QSystemTrayIcon>
#include <QWindow>

#include "core/config_descriptors.h"
#include "core/iconfig.h"
#include "core/log.h"
#include "core/platform_backend.h"
#include "core/result.h"

namespace baniphelper::ui {
namespace {

/// 退出即撤销开关的配置键。
///
/// 用函数内静态量而不是命名空间级常量：字符串要在运行期从 `config_descriptors.h`
/// 的键名构造，放在命名空间作用域会引入一次静态初始化。
const QString& revokeOnExitKey() {
  static const QString key = QString::fromLatin1(baniphelper::core::kConfigKeyShutdownRevokeOnExit);
  return key;
}

}  // namespace

using baniphelper::core::closeLogging;
using baniphelper::core::IConfig;
using baniphelper::core::LogLevel;
using baniphelper::core::logWrite;
using baniphelper::core::PlatformBackend;
using baniphelper::core::Result;

AppShell::AppShell(PlatformBackend& backend, IConfig& config, QObject* parent)
    : QObject(parent), backend_(backend), config_(config) {
  // 收尾挂在 aboutToQuit 上，而不是挂在「托盘菜单点了退出」上：
  // 事件循环结束的方式不止一种（菜单、关闭窗口、QML 加载失败后的 exit、别处的 quit），
  // 挂在信号上才能保证无论走哪条路收尾都发生，且只发生一次。
  QCoreApplication* application = QCoreApplication::instance();
  if (application != nullptr) {
    connect(application, &QCoreApplication::aboutToQuit, this, &AppShell::cleanup);
  }
  logWrite(LogLevel::Debug, QStringLiteral("托盘外壳已建立（无托盘也能跑：此时关闭窗口即为退出）"));
}

AppShell::~AppShell() {
  // 事件循环还没结束就被析构时（启动早期出错直接返回），收尾不能漏。
  cleanup();
}

bool AppShell::start() {
  if (!QSystemTrayIcon::isSystemTrayAvailable()) {
    // 不把「托盘不可用」当致命错误：主体功能不依赖托盘，
    // 因为取不到一个外壳图标就不让程序启动，是拿外壳换掉了全部功能。
    logWrite(LogLevel::Warn, QStringLiteral("系统托盘不可用，关闭窗口即为退出"));
    return false;
  }

  // 图标同时用于托盘与窗口。资源没打进来时加载会失败，
  // 结果是托盘上一个空白方块 —— 用户根本找不到它，也就找不到退出入口，
  // 所以这条要明确记下来，不能只留一个空图标。
  const QIcon icon(QStringLiteral(":/baniphelper.svg"));
  if (icon.isNull()) {
    logWrite(LogLevel::Error,
             QStringLiteral("图标资源加载失败（:/baniphelper.svg），托盘上会是一个空白方块"));
  }

  buildMenu();

  tray_ = new QSystemTrayIcon(icon, this);
  tray_->setToolTip(QStringLiteral("BanIPHelper"));
  tray_->setContextMenu(menu_);

  connect(
      tray_, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        // 单击给 Trigger、双击给 DoubleClick，两个都认才符合大多数人的直觉。
        if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) {
          showWindow();
        }
      });

  tray_->show();
  logWrite(LogLevel::Info, QStringLiteral("托盘图标已就绪"));
  return true;
}

bool AppShell::trayAvailable() const {
  return tray_ != nullptr && tray_->isVisible();
}

void AppShell::setMainWindow(QWindow* window) {
  window_ = window;
  if (window == nullptr) {
    // QML 根对象不是窗口时，显示与隐藏都无从谈起。这不是崩溃，但必须留痕：
    // 否则现象只是「点托盘没反应」，而这句日志能直接指出原因。
    logWrite(LogLevel::Warn, QStringLiteral("QML 根对象不是窗口，主界面将无法显示与隐藏"));
    return;
  }

  // 关闭事件由外壳决定怎么处理，见 eventFilter。
  window->installEventFilter(this);
}

bool AppShell::eventFilter(QObject* watched, QEvent* event) {
  if (window_.isNull() || watched != window_.data() || event->type() != QEvent::Close) {
    return QObject::eventFilter(watched, event);
  }

  // 不把事件放行：放行就意味着窗口真的关掉（QML 的 Window 会被隐藏并发出 closing）。
  // 到这里就已经决定了要收进托盘还是退出，再放行只会多一次状态变化。
  hideToTray();
  return true;
}

void AppShell::showWindow() {
  if (window_.isNull()) {
    logWrite(LogLevel::Warn, QStringLiteral("主窗口还没准备好，无法显示"));
    return;
  }

  // 先记住它原来是不是藏着的：一次双击会给来 Trigger 与 DoubleClick 两个事件，
  // 每来一个就记一行日志会把日志写成流水账。
  const bool wasVisible = window_->isVisible();

  // 最小化状态下直接 show() 不会把窗口带回前台，必须先摘掉最小化标记。
  //
  // `QWindow::setWindowState` 只接受**单个**状态位，所以这里按「原本是不是最大化」
  // 决定回到哪一个状态：回 `WindowNoState` 会把最大化的窗口缩成原来的大小，
  // 那是「唤起之后窗口变了形」这类说不清的小毛病。
  const Qt::WindowStates state = window_->windowState();
  if (state.testFlag(Qt::WindowMinimized)) {
    window_->setWindowState(state.testFlag(Qt::WindowMaximized) ? Qt::WindowMaximized
                                                                : Qt::WindowNoState);
  }

  window_->show();
  window_->raise();
  // 由托盘点击或二次启动触发时，Windows 允许前台激活；
  // Qt 在拿不到前台权限时会退化成任务栏闪烁，那也是「有反应」，比什么都没有好。
  window_->requestActivate();

  if (!wasVisible) {
    // 记成 Info：二次启动的唤醒器自己不写日志（它没有日志器），
    // 这一行就是「双击到底有没有效」的唯一证据，默认级别下也必须看得见。
    logWrite(LogLevel::Info, QStringLiteral("主界面已显示"));
  }
}

void AppShell::hideToTray() {
  if (!trayAvailable()) {
    // 托盘没建起来的时候收进托盘，等于让程序消失得无影无踪，只能退出。
    logWrite(LogLevel::Info, QStringLiteral("托盘不可用，关闭窗口即退出"));
    shutdown();
    return;
  }

  if (!window_.isNull()) {
    const bool wasVisible = window_->isVisible();
    window_->hide();
    if (wasVisible) {
      // 留一行痕迹。用户事后问「程序怎么不见了」时，这一行是唯一能回答它的东西：
      // 收进托盘不是退出，进程还在。
      logWrite(LogLevel::Info, QStringLiteral("主界面已收进托盘，进程仍在运行"));
    }
  }

  if (!trayHintShown_ && tray_ != nullptr) {
    trayHintShown_ = true;
    // 只弹一次：每次收进托盘都弹就成了骚扰。
    // 这条提示的作用是让用户知道「程序还在，右键托盘可退出」，少了它，
    // 一个没见过托盘程序的人只能去任务管理器里杀进程 —— 而杀进程不会撤销过滤器。
    tray_->showMessage(QStringLiteral("BanIPHelper 仍在运行"),
                       QStringLiteral("程序已收进托盘。右键托盘图标可以退出，"
                                      "退出时会按设置处理已下发的封禁规则。"),
                       QSystemTrayIcon::Information,
                       5000);
  }
}

void AppShell::shutdown() {
  logWrite(LogLevel::Info, QStringLiteral("收到退出请求"));
  // 只负责请求结束事件循环，实际收尾统一走 aboutToQuit → cleanup。
  //
  // 托盘菜单与窗口事件都只可能在事件循环跑起来之后发生，
  // 因此这里不必担心「事件循环还没开始就 quit」那种什么也不会发生的情况。
  QCoreApplication::quit();
}

void AppShell::cleanup() {
  if (cleanedUp_) {
    return;
  }
  cleanedUp_ = true;

  logWrite(LogLevel::Info, QStringLiteral("开始退出收尾"));

  if (tray_ != nullptr) {
    // 先让图标消失：万一后面哪一步卡住，用户至少看得出程序已经在退。
    tray_->hide();
  }

  // 一、按设置决定要不要撤销已下发的过滤器。
  //
  // 读不到设置时按「撤销」处理：撤销是回到原状，属于安全的那一侧。
  // 反过来默认「保留」的话，一次读取失败就会把用户的网络继续封着。
  if (backend_.filterEngine == nullptr) {
    logWrite(LogLevel::Error, QStringLiteral("过滤器引擎不存在，退出时无法撤销过滤器"));
  } else if (revokeOnExit(true)) {
    const Result<void> revoked = backend_.filterEngine->revokeAll();
    if (!revoked) {
      logWrite(LogLevel::Error,
               QStringLiteral("撤销全部过滤器失败：%1").arg(revoked.error().message));
    } else {
      logWrite(LogLevel::Info, QStringLiteral("已撤销本程序下发的全部过滤器"));
    }
  } else {
    logWrite(LogLevel::Warn,
             QStringLiteral("按设置为退出时不撤销过滤器：已下发的封禁会继续在内核里生效，"
                            "要等下次启动时的清理才会被收掉"));
  }

  // 二、关引擎。接口明确写了「close 不负责撤销」，撤销在上一步已经按设置做过。
  if (backend_.filterEngine != nullptr) {
    const Result<void> closed = backend_.filterEngine->close();
    if (!closed) {
      logWrite(LogLevel::Error,
               QStringLiteral("关闭过滤器引擎失败：%1").arg(closed.error().message));
    }
  }

  // 三、先停接收端，再放掉所有权。
  //
  // 顺序反过来的话，新实例可能在中间那一刻连进来却没人应答：
  // 它按契约报「唤起失败」然后退出，而这边正好也退了 —— 结果两个都没了。
  if (backend_.singleInstance != nullptr) {
    backend_.singleInstance->stopListening();
    backend_.singleInstance->release();
  }

  logWrite(LogLevel::Info, QStringLiteral("退出收尾完成"));
  closeLogging();
}

void AppShell::buildMenu() {
  menu_ = new QMenu();

  showAction_ = menu_->addAction(QStringLiteral("显示主界面"));
  connect(showAction_, &QAction::triggered, this, &AppShell::showWindow);

  menu_->addSeparator();

  revokeOnExitAction_ = menu_->addAction(QStringLiteral("退出时撤销全部过滤器"));
  revokeOnExitAction_->setCheckable(true);
  // 先让勾选状态对上配置里的真实值，再接信号：
  // 否则建菜单时的那次 setChecked 会被当成用户操作，把配置写一遍。
  syncRevokeOnExitAction();
  connect(revokeOnExitAction_, &QAction::toggled, this, &AppShell::writeRevokeOnExit);

  menu_->addSeparator();

  QAction* quitAction = menu_->addAction(QStringLiteral("退出"));
  connect(quitAction, &QAction::triggered, this, &AppShell::shutdown);
}

void AppShell::syncRevokeOnExitAction() {
  if (revokeOnExitAction_ == nullptr) {
    return;
  }

  // 这里只改界面，不写配置 —— 读一次写一次迟早绕成回环。
  const QSignalBlocker blocker(revokeOnExitAction_);
  revokeOnExitAction_->setChecked(revokeOnExit(true));
}

void AppShell::writeRevokeOnExit(bool enabled) {
  const Result<void> saved = config_.setValue(revokeOnExitKey(), QJsonValue(enabled));
  if (!saved) {
    logWrite(LogLevel::Error,
             QStringLiteral("保存「退出时撤销全部过滤器」失败：%1").arg(saved.error().message));
    // 写失败时配置会回滚内存里的值，勾选状态也必须跟着退回去：
    // 否则界面上显示「已关」，实际退出时仍然会撤销，两边各说一套。
    syncRevokeOnExitAction();
    return;
  }

  if (enabled) {
    logWrite(LogLevel::Info, QStringLiteral("已打开「退出时撤销全部过滤器」"));
  } else {
    logWrite(LogLevel::Warn,
             QStringLiteral("已关闭「退出时撤销全部过滤器」：本程序下发的封禁会在退出后继续生效"));
  }
}

bool AppShell::revokeOnExit(bool fallback) const {
  const Result<QJsonValue> value = config_.value(revokeOnExitKey());
  if (!value) {
    logWrite(LogLevel::Warn,
             QStringLiteral("读取「退出时撤销全部过滤器」失败，本次按%1处理：%2")
                 .arg(fallback ? QStringLiteral("撤销") : QStringLiteral("保留"),
                      value.error().message));
    return fallback;
  }

  if (value.value().type() != QJsonValue::Bool) {
    logWrite(LogLevel::Warn,
             QStringLiteral("「退出时撤销全部过滤器」不是布尔值，本次按%1处理")
                 .arg(fallback ? QStringLiteral("撤销") : QStringLiteral("保留")));
    return fallback;
  }

  return value.value().toBool();
}

}  // namespace baniphelper::ui
