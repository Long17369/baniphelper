# 平台实现：Windows

本期唯一实现。承载下列接口的 Windows 版本（头文件在 S1.2 冻结）：

| 接口 | 本平台的落地手段 |
| --- | --- |
| `IFilterEngine` | WFP，`fwpuclnt` |
| `ITargetResolver` | 可执行文件的 NT 设备路径 |
| `IConnMonitor` | IP Helper 端点表 |
| `ITrafficStats` | TCP 走 eStats，UDP 走 ETW |
| `IKiller` | `SetTcpEntry`，初期仅 IPv4 |
| `ISessionMonitor` | ETW |
| `IPrivilege` | 管理员令牌检查 |
| `ISingleInstance` | 命名 Mutex |
| `IAutoStart` | 计划任务 |
| `IPaths` | `%APPDATA%\BanIPHelper` |

两条硬性要求：

1. 本目录的实现必须先通过**平台无关的契约测试**，上层才允许调用
2. 本目录的实现**不得**被其他平台实现引用，公共代码只能下沉到 `core/` 或 `platform/api/`
