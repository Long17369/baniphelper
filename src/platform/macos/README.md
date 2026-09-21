# 平台实现：macOS

**占位目录，尚未实现。**

按 [docs/architecture.md 第 12 节](../../../docs/architecture.md) 的约定，新增平台等于
「新增一个 `platform/<os>/` 目录并通过同一套契约测试」，不允许改动 `core/`。

注意：本平台的 `IKiller` 很可能没有等价能力，按能力协商如实声明为不可用，
界面显式禁用，**禁止静默失败**。具体步骤见
[docs/phases/08-portability.md](../../../docs/phases/08-portability.md)。
