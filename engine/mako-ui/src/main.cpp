/* SPDX-License-Identifier: GPL-3.0-or-later */

#include <QIcon>
#include <QApplication>
#include <QQuickStyle>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QUrl>
#include <exception>
#include <iostream>

#include "backend.hpp"
#include "localization.hpp"
#include "utils.hpp"

using namespace mako::ui;

int main(int argc, char* argv[]) try {
    // Desktop Qt Quick styles can delegate painting to the user's QStyle
    // (for example qt6ct/Kvantum), which requires QApplication.
    const QApplication app(argc, argv);
    QGuiApplication::setWindowIcon(QIcon(":/rsc/io.github.eugeniosegala.mako.png"));
    QGuiApplication::setOrganizationName("MAKO");
    QGuiApplication::setOrganizationDomain("io.github.eugeniosegala");
    QGuiApplication::setApplicationName("mako-ui");
    QGuiApplication::setApplicationDisplayName("MAKO Renderer");

    QQmlApplicationEngine engine;
    const auto desktopStyle = desktopQuickStyle(engine.importPathList());
    if (!desktopStyle.isEmpty())
        QQuickStyle::setStyle(desktopStyle);
    Backend backend;
    Localization localization;

    engine.rootContext()->setContextProperty("backend", &backend);
    engine.rootContext()->setContextProperty("localization", &localization);
    engine.load("qrc:/rsc/UI.qml");

    return QGuiApplication::exec();
} catch (const std::exception& error) {
    std::cerr << "MAKO Renderer: configuration UI failed: " << error.what() << '\n';
    return 1;
}
