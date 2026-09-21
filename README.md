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

本项目以 **GNU General Public License 第 3 版或更新版本**（`GPL-3.0-or-later`）发布，
全文见 [LICENSE](LICENSE)。第三方组件的许可与履行方式见
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md)。

本程序是自由软件：你可以按自由软件基金会发布的 GNU 通用公共许可证条款
重新分发或修改它，无论是第 3 版还是（由你选择的）任何更新版本。

本程序分发时希望它有用，但不提供任何担保，甚至不提供适销性
或特定用途适用性的默示担保。详情见 GNU 通用公共许可证。
