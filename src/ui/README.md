# 界面层

只依赖 `core/` 与 `platform/api/`，**不得**包含任何平台专有头。

- `main.cpp` — 入口，装配 QGuiApplication 与 QML 引擎
- `qml/` — QML 界面

界面选型为 Qt Quick，美观是界面面的第一优先级，具体落点见
[docs/architecture.md 第 2.2 节](../../docs/architecture.md)。
