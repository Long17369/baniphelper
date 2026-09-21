import QtQuick

// 阶段一 S1.1 的验收目标只是一个能起来的窗口。
// 深色主题、无边框自绘标题栏与卡片布局在阶段五 S5 落地，
// 届时本文件会被替换为 shell + 各功能页的结构。

Window {
    id: root

    width: 960
    height: 640
    visible: true
    title: qsTr("BanIPHelper")

    color: "#1e1f22"

    Text {
        anchors.centerIn: parent
        color: "#e6e6e6"
        font.pixelSize: 18
        text: qsTr("BanIPHelper 骨架已就绪")
    }
}
