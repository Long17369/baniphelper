# 第三方组件与许可

本文件说明 BanIPHelper 所使用的第三方组件、各自的许可，以及履行这些许可的具体方式。

## 1 本项目自身的许可

本项目以 **GNU General Public License 第 3 版或更新版本**（`GPL-3.0-or-later`）发布，
全文见仓库根目录的 [LICENSE](LICENSE)。

选择 GPLv3 而不是宽松许可，不是偏好问题，而是被依赖钉住的
（见第 2 节里标为「仅 GPLv3」的两个模块）：

- 用了 `Qt Charts` 或 `Qt HTTP Server` 中的任何一个，整个程序就必须按 GPLv3 分发。
- 反过来，项目按 GPLv3 发布时，LGPLv3 的那些模块的额外义务被更强的条款自动覆盖。

## 2 Qt

版本 **6.9.3**，取自官方开源发行版的 `mingw_64` 套件。

### 2.1 模块

| 模块 | 用在哪 | 许可 |
| --- | --- | --- |
| `Qt Core`、`Qt Gui`、`Qt Qml`、`Qt Quick`、`Qt Network`、`Qt Test` | 已链接 | LGPLv3 |
| `Qt Quick Controls` | 规划中，界面 | LGPLv3 |
| `Qt Svg` | 规划中，界面图标 | LGPLv3 |
| `Qt Sql` | 规划中，数据存储 | LGPLv3 |
| `Qt Charts` | 规划中，实时流量曲线（S5） | **仅商业 或 GPLv3** |
| `Qt HTTP Server` | 规划中，远程界面（S6） | **仅商业 或 GPLv3** |

「仅商业 或 GPLv3」是 Qt 官方文档的原话措辞：这些模块不提供 LGPLv3 这一档。
另外 Qt Charts 自 Qt 6.10 起已被官方废弃，建议改用 Qt Graphs；
本项目当前锁定在 6.9.3，不受影响，但推进到 S5 时应重新评估。

### 2.2 工具

`moc`、`uic`、`rcc`、`windeployqt`、Qt Creator 等工具按
**GPLv3 加 Qt GPL Exception 1.0** 提供。该例外条款的作用正是让「用这些工具构建」
不传染被构建的代码，因此构建期使用它们不产生额外义务。

### 2.3 履行方式

对 LGPLv3 模块，本项目做到：

1. **动态链接**。Qt 以动态库形式使用，运行时由可执行文件同目录下的 DLL 提供，
   没有静态链接进可执行文件。
2. **随附许可全文**。GPLv3 全文见 [LICENSE](LICENSE)，
   LGPLv3 全文见 [LICENSES/LGPL-3.0.txt](LICENSES/LGPL-3.0.txt)。
3. **不阻碍替换**。未对 Qt 库做完整性校验、签名锁定或任何阻止用户替换这些 DLL 的处理。
4. **未修改 Qt 源码**。因此不存在发布 Qt 修改版的义务。
   Qt 源码可从 <https://www.qt.io/download-open-source> 获取。

## 3 MinGW 运行库

构建产物旁边会随附 MinGW-w64 的运行库（如 `libgcc_s_seh-1.dll`、`libstdc++-6.dll`、
`libwinpthread-1.dll`）。它们按 **GPLv3 加 GCC Runtime Library Exception** 提供，
该例外同样使「使用这些运行库」不传染被编译的代码。

## 4 微软 Windows SDK

本项目**不再分发**任何 Windows SDK 文件。仅在编写 WFP 相关代码时，
把 SDK 头文件 `fwpmu.h` 里的若干 GUID **常量取值**抄进了
`src/platform/win/wfp_guids.h`（GUID 是接口的 ABI 事实，属于取值而非受版权保护的表达），
并在该文件里标注了来源。SDK 头文件本身既不入库也不随程序分发。

## 5 其他

本项目不使用任何需要在界面上展示归属的字体、图标集或图形素材。
若将来引入，应补记于此。
