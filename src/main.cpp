// wickfs — Smart on-demand Google Drive virtual filesystem & LFU/LRU cache manager.

#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QStringList>
#include <QUrl>

#include "backend.h"
#include "cli.h"
#include "theme.h"
#include "translator.h"

int main(int argc, char *argv[]) {
    QStringList rawArgs;
    rawArgs.reserve(argc);
    for (int i = 0; i < argc; ++i)
        rawArgs.append(QString::fromLocal8Bit(argv[i]));

    if (isCliInvocation(rawArgs))
        return runCli(argc, argv);

    if (qEnvironmentVariable("QT_QPA_PLATFORM") == QLatin1String("offscreen"))
        qunsetenv("QT_QPA_PLATFORMTHEME");

    QGuiApplication app(argc, argv);
    app.setApplicationName("wickfs");
    app.setApplicationVersion("0.1.0");

    // Matches the window to wickfs.desktop, so the compositor (Wayland app_id)
    // and the launcher pick up the installed icon.
    app.setDesktopFileName("wickfs");
    app.setWindowIcon(QIcon::fromTheme("wickfs", QIcon(":/wickfs.svg")));

    QQuickStyle::setStyle("Material");

    // Installed before anything is translated, and every qsTr() binding is
    // re-evaluated when the Omarchy language changes.
    Translator translator;
    app.installTranslator(&translator);

    Theme theme;
    Backend backend;

    // Accept either `wickfs manifest.json` or `wickfs open manifest.json`.
    const QStringList args = app.arguments();
    QString openPath;
    if (args.size() >= 2) {
        if (args.at(1) == "open" && args.size() >= 3)
            openPath = args.at(2);
        else if (args.at(1) != "open" && !args.at(1).startsWith('-'))
            openPath = args.at(1);
    }
    if (!openPath.isEmpty())
        backend.loadManifestFile(openPath);

    QQmlApplicationEngine engine;
    QObject::connect(&translator, &Translator::changed, &engine, &QQmlEngine::retranslate);
    engine.rootContext()->setContextProperty("translator", &translator);
    engine.rootContext()->setContextProperty("theme", &theme);
    engine.rootContext()->setContextProperty("backend", &backend);
    engine.load(QUrl("qrc:/Main.qml"));
    if (engine.rootObjects().isEmpty())
        return 1;

    return app.exec();
}
