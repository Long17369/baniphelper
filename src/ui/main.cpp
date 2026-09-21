#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QString>

#include "core/version.h"

int main(int argc, char* argv[])
{
    QGuiApplication app(argc, argv);

    // 应用标识：配置目录与数据库目录都以它为准，必须在读取路径之前设好。
    app.setOrganizationName(QStringLiteral("BanIPHelper"));
    app.setApplicationName(QStringLiteral("BanIPHelper"));
    app.setApplicationVersion(QString::fromLatin1(baniphelper::core::versionString()));

    QQmlApplicationEngine engine;

    // QML 加载失败必须是显式失败，不允许留下一个「看起来启动了」的空进程。
    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &app,
        []() { QCoreApplication::exit(EXIT_FAILURE); },
        Qt::QueuedConnection);

    engine.loadFromModule("BanIPHelper", "Main");

    return app.exec();
}
