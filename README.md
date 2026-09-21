# BanIPHelper

按进程、地址、端口与网段封禁网络连接的系统级工具。

- 数据面走 Windows 过滤平台，**不加载自有驱动**，只有一个提权托盘进程。
- 覆盖 IPv4 与 IPv6、TCP 与 UDP、出站与入站，并支持立即中断已建立的 IPv4 连接。
- 能观测：谁连了谁、每个连接多少流量。

## 文档

| 文档 | 内容 |
| --- | --- |
| [docs/README.md](docs/README.md) | 流程总纲：阶段划分、并行策略、里程碑 |
| [docs/handoff.md](docs/handoff.md) | 当前状态与下一步（隔一段时间回来先看这个） |
| [docs/architecture.md](docs/architecture.md) | 架构、技术选型与决策记录 |
| [docs/phases/](docs/phases/) | 各阶段的具体步骤清单 |

## 构建

需要 Qt 6.9 及以上的 MinGW 套件、CMake 3.21 及以上、Ninja。
配置前设置两个环境变量，预设文件通过工具链文件读它们：

| 变量 | 内容 |
| --- | --- |
| `BANIPHELPER_QTDIR` | Qt 套件根目录，形如 `<Qt>/6.9.3/mingw_64` |
| `BANIPHELPER_MINGW_BIN` | 同一套件的 MinGW `bin` 目录，形如 `<Qt>/Tools/mingw1310_64/bin` |

然后：

```powershell
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

构建后会把 Qt 运行库部署到可执行文件旁边，双击即可运行，不需要再配 `PATH`。
程序带提权清单，启动时会请求管理员权限。

## 许可

Copyright (C) 2026 Long17369

本项目以 **GNU General Public License 第 3 版或更新版本**（`GPL-3.0-or-later`）发布，
全文见 [LICENSE](LICENSE) —— 该文件只放本项目自己的许可，署名也在其中。

本程序是自由软件：你可以按自由软件基金会发布的 GNU 通用公共许可证条款
重新分发或修改它，无论是第 3 版还是（由你选择的）任何更新版本。

本程序分发时希望它有用，但不提供任何担保，甚至不提供适销性
或特定用途适用性的默示担保。详情见 GNU 通用公共许可证。

### 第三方组件的许可

本项目用到的第三方组件及其许可**集中列在这里**，不另外拆文件。

| 组件 | 用在哪 | 许可 |
| --- | --- | --- |
| Qt 6.9.3：`Core`、`Gui`、`Qml`、`Quick`、`Network`、`Test` | 已链接 | [LGPLv3](https://www.gnu.org/licenses/lgpl-3.0.txt) |
| Qt：`Quick Controls`、`Svg`、`Sql` | 规划中 | [LGPLv3](https://www.gnu.org/licenses/lgpl-3.0.txt) |
| Qt：`Charts`（S5 流量曲线）、`HTTP Server`（S6 远程界面） | 规划中 | **仅商业或 [GPLv3](https://www.gnu.org/licenses/gpl-3.0.txt)** |
| Qt 工具：`moc`、`uic`、`rcc`、`windeployqt`、Qt Creator | 构建期 | GPLv3 加 [Qt GPL Exception 1.0](https://doc.qt.io/qt-6/qt-gpl-exception-1-0.html) |
| MinGW-w64 运行库 | 随程序分发 | GPLv3 加 [GCC Runtime Library Exception](https://www.gnu.org/licenses/gcc-exception-3.1.html) |

两点说明：

- **为什么本项目必须是 GPLv3**：`QtCharts` 与 `QtHTTPServer` 没有 LGPLv3 那一档，
  官方文档的措辞是「仅商业许可，或者 GPLv3」。用了其中任何一个，整个程序就得按 GPLv3 分发。
  另外 Qt Charts 自 6.10 起已被官方废弃，建议改用 Qt Graphs；本项目锁定 6.9.3，暂不受影响。
- **Qt 工具为什么不传染**：`Qt GPL Exception 1.0` 的作用正是让「用这些工具构建」
  不传染被构建的代码。GCC 的运行时例外同理。

对 LGPLv3 的模块，本项目做到：以动态库方式使用（Qt DLL 与可执行文件并列，未静态链接）；
未修改 Qt 源码；未对 Qt 库做完整性校验或签名锁定，用户可自行替换。
项目整体按 GPLv3 发布，已覆盖 LGPLv3 的额外义务。

本项目不分发任何 Windows SDK 文件。仅在编写 WFP 相关代码时，把 SDK 头 `fwpmu.h` 里
若干 GUID 的**常量取值**抄进了 `src/platform/win/wfp_guids.h`（GUID 是接口的 ABI 事实），
并在该文件里标注了来源；SDK 头本身既不入库也不随程序分发。
