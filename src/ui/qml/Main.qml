import QtQuick

// 阶段一 S1.8 的验收目标是「托盘菜单可退出且进程干净结束」，界面本身仍是最小骨架。
// 深色主题、无边框自绘标题栏与卡片布局在阶段五 S5 落地，
// 届时本文件会被替换为 shell + 各功能页的结构。

Window {
    id: root

    width: 960
    height: 640
    visible: true
    title: qsTr("BanIPHelper")

    color: "#1e1f22"

    // 窗口的关闭按钮不在这里处理："关窗算什么" 由 C++ 外壳决定（托盘可用就收进托盘，
    // 不可用就直接退出），QML 不需要知道托盘存不存在。实现见 ui/app_shell.cpp。

    Column {
        anchors.centerIn: parent
        spacing: 10

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            color: "#e6e6e6"
            font.pixelSize: 18
            text: qsTr("BanIPHelper 骨架已就绪")
        }

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            color: "#9aa0a6"
            font.pixelSize: 13
            text: qsTr("关闭窗口会收进托盘，退出请用托盘图标的右键菜单")
        }
    }
}
