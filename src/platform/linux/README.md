# 平台实现：Linux

**占位目录，尚未实现。**

按 [docs/architecture.md 第 12 节](../../../docs/architecture.md) 的约定，新增平台等于
「新增一个 `platform/<os>/` 目录并通过同一套契约测试」，不允许改动 `core/`。

届时预期：封禁走 nftables/iptables，连接枚举读 `/proc/net`，字节统计读 `/proc/net/dev`，
自启动走 systemd user unit。具体步骤见
[docs/phases/08-portability.md](../../../docs/phases/08-portability.md)。
