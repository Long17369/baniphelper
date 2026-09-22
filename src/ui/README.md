# 界面层

只依赖 `core/` 与 `platform/api/`，**不得**包含任何平台专有头。

- `main.cpp` — 入口，装配 QApplication、平台后端、托盘外壳与 QML 引擎
- `app_shell.h` / `app_shell.cpp` — 托盘外壳：图标、菜单、主窗口显隐、**退出路径**
- `assets/baniphelper.svg` — 图标，编进可执行文件（提权启动不会去读项目目录）
- `qml/` — QML 界面

界面选型为 Qt Quick，美观是界面面的第一优先级，具体落点见
[docs/architecture.md 第 2.2 节](../../docs/architecture.md)。

## 托盘与退出

界面主体是 Qt Quick，托盘是 Widgets 里的 `QSystemTrayIcon`
（`architecture.md` 第 1.3 节写着「Widgets 做托盘与辅助」），
因此入口用 `QApplication` 而不是 `QGuiApplication`。

三条不能破的规矩，理由与实测记录见
[docs/phases/01-foundation.md 第 3.6 节](../../docs/phases/01-foundation.md)：

1. **关闭窗口 = 收进托盘**，退出只在托盘菜单里；
2. **托盘不可用时关闭窗口即退出**，不留「既没图标又没窗口」的进程；
3. **退出收尾挂在 `aboutToQuit` 上**，而不是挂在菜单项上，
   这样任何导致事件循环结束的路径都会走一遍收尾，且只走一遍。

窗口的关闭事件由 `AppShell::eventFilter` 拦下，QML 不需要知道托盘存不存在。
