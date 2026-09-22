#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QWindow>

// 托盘相关的三个类型只以指针形式出现，前置声明就够，
// 免得把整个 Widgets 头交给每一个包含本头的文件。
class QAction;
class QMenu;
class QSystemTrayIcon;

namespace baniphelper::core {
class IConfig;
struct PlatformBackend;
}  // namespace baniphelper::core

namespace baniphelper::ui {

/// 托盘外壳：图标、菜单、主窗口的显隐，以及**退出路径**。
///
/// 托盘程序最常见的坏结局是「找不到退出入口」：窗口一关就躲进托盘，而托盘图标又不显眼，
/// 用户只能去任务管理器里杀进程 —— 而杀进程恰恰不会走撤销过滤器的路径。
/// 因此这里有四条硬约束：
///
/// 1. 托盘不可用时（`QSystemTrayIcon::isSystemTrayAvailable()` 为假）**关闭窗口就是退出**。
///    绝不允许出现「没有托盘图标、窗口也不见了」的进程；
/// 2. 第一次收进托盘时弹一次气泡，明确告诉用户程序还在托盘里、右键能退出；
/// 3. 退出收尾挂在 `aboutToQuit` 上，而不是挂在「托盘菜单点了退出」上。
///    这样无论事件循环因何结束（菜单、关闭窗口、别处的 quit），收尾都只发生一次且一定会发生；
/// 4. 收尾按「撤销 → 关引擎 → 停接收端 → 释放单实例 → 写日志 → 关日志」的顺序执行，
///    每一步失败都只记录不中断：要撤销的撤销了、该关的关了，才算干净退出。
class AppShell final : public QObject {
  Q_OBJECT

 public:
  AppShell(core::PlatformBackend& backend, core::IConfig& config, QObject* parent = nullptr);
  ~AppShell() override;

  AppShell(const AppShell&) = delete;
  AppShell& operator=(const AppShell&) = delete;

  /// 建立托盘图标与菜单。返回 false 表示托盘不可用，调用方应退回「关窗即退出」。
  ///
  /// 之所以不把「不可用」当成致命错误：程序的主体功能不依赖托盘，
  /// 因为取不到托盘图标就不让启动，是拿一个外壳去换掉全部功能。
  [[nodiscard]] bool start();

  /// 托盘是否真的建起来了。
  [[nodiscard]] bool trayAvailable() const;

  /// 记下主窗口。QML 加载出来之后调用；窗口销毁时会自动置空。
  void setMainWindow(QWindow* window);

  /// 拦下主窗口的关闭事件，按「收进托盘还是退出」处理。
  ///
  /// 放在事件过滤器里而不是 QML 的 `onClosing` 里，是为了让「关窗算什么」这个策略
  /// 只有一处定义，QML 不需要知道托盘存不存在。
  bool eventFilter(QObject* watched, QEvent* event) override;

 public slots:
  /// 显示主窗口。既有实例被唤起时也走它，因此必须能在窗口隐藏时把界面重新拿出来。
  void showWindow();

  /// 收进托盘。托盘不可用时改为直接退出 —— 这是第 1 条硬约束的落点。
  void hideToTray();

  /// 退出。收尾交给 `aboutToQuit`，这里只负责请求结束事件循环。
  void shutdown();

 private:
  /// 退出收尾。幂等：事件循环结束只会触发一次，但兜底路径可能再来一次。
  void cleanup();

  void buildMenu();

  /// 让菜单勾选状态与配置里的真实值一致。
  void syncRevokeOnExitAction();

  /// 把开关写回配置。写失败要把界面上的勾退回去，否则用户以为改成了。
  void writeRevokeOnExit(bool enabled);

  /// 读开关。读不到时返回 `fallback` 并已记过日志。
  [[nodiscard]] bool revokeOnExit(bool fallback) const;

  core::PlatformBackend& backend_;
  core::IConfig& config_;

  QPointer<QWindow> window_;

  QSystemTrayIcon* tray_ = nullptr;
  QMenu* menu_ = nullptr;
  QAction* showAction_ = nullptr;
  QAction* revokeOnExitAction_ = nullptr;

  /// 气泡提示只弹一次。每次收进托盘都弹，会变成骚扰。
  bool trayHintShown_ = false;

  /// 收尾是否已经做过。
  bool cleanedUp_ = false;
};

}  // namespace baniphelper::ui
