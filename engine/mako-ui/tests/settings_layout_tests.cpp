/* SPDX-License-Identifier: GPL-3.0-or-later */

#include <QApplication>
#include <QEventLoop>
#include <QFile>
#include <QDir>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTimer>
#include <QUrl>
#include <QTemporaryDir>

#include "utils.hpp"

#include <iostream>
#include <stdexcept>

namespace {
QStringList layoutWarnings;

void messages(QtMsgType, const QMessageLogContext&, const QString& message) {
    if (message.contains(QStringLiteral("recursive rearrange")) ||
            message.contains(QStringLiteral("Binding loop")))
        layoutWarnings.append(message);
    std::cerr << message.toStdString() << '\n';
}

void require(const bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

void settle() {
    QEventLoop loop;
    QTimer::singleShot(50, &loop, &QEventLoop::quit);
    loop.exec();
}

void testDesktopStyleSelection() {
    QTemporaryDir directory;
    require(directory.isValid(), "cannot create desktop-style fixture");
    const auto module = directory.filePath(QStringLiteral("org/kde/desktop"));
    require(QDir().mkpath(module), "cannot create desktop-style module directory");
    QFile qmldir(module + "/qmldir");
    require(qmldir.open(QIODevice::WriteOnly), "cannot create desktop-style module marker");
    qmldir.close();
    const auto previousTheme = qgetenv("QT_QPA_PLATFORMTHEME");
    const auto previousStyle = qgetenv("QT_QUICK_CONTROLS_STYLE");
    const auto previousConfig = qgetenv("QT_QUICK_CONTROLS_CONF");
    qputenv("QT_QPA_PLATFORMTHEME", "qt6ct");
    qunsetenv("QT_QUICK_CONTROLS_STYLE");
    qunsetenv("QT_QUICK_CONTROLS_CONF");
    const QStringList imports{directory.path()};
    require(mako::ui::desktopQuickStyle(imports) == "org.kde.desktop",
        "qt6ct did not select the installed widget-theme bridge");
    require(mako::ui::desktopQuickStyle({}).isEmpty(),
        "missing desktop-style module prevented ordinary Qt startup");
    qputenv("QT_QUICK_CONTROLS_STYLE", "Fusion");
    require(mako::ui::desktopQuickStyle(imports).isEmpty(),
        "automatic desktop style overrode an explicit Quick style");
    qunsetenv("QT_QUICK_CONTROLS_STYLE");
    qputenv("QT_QUICK_CONTROLS_CONF", "/custom/qtquickcontrols2.conf");
    require(mako::ui::desktopQuickStyle(imports).isEmpty(),
        "automatic desktop style overrode an explicit controls configuration");
    qunsetenv("QT_QUICK_CONTROLS_CONF");
    qputenv("QT_QPA_PLATFORMTHEME", "gtk3");
    require(mako::ui::desktopQuickStyle(imports).isEmpty(),
        "qt6ct bridge selection changed another platform theme");
    const auto restore = [](const char* name, const QByteArray& value) {
        if (value.isNull()) qunsetenv(name);
        else qputenv(name, value);
    };
    restore("QT_QPA_PLATFORMTHEME", previousTheme);
    restore("QT_QUICK_CONTROLS_STYLE", previousStyle);
    restore("QT_QUICK_CONTROLS_CONF", previousConfig);
}
}

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    qInstallMessageHandler(messages);
    try {
        testDesktopStyleSelection();
        QQmlApplicationEngine engine;
        const auto desktopStyle = mako::ui::desktopQuickStyle(engine.importPathList());
        if (!desktopStyle.isEmpty())
            QQuickStyle::setStyle(desktopStyle);
        const auto directory = QUrl::fromLocalFile(
            QString::fromUtf8(MAKO_UI_PANES_DIRECTORY)).toString();
        engine.loadData((QStringLiteral(R"QML(
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ")QML") + directory + QStringLiteral(R"QML("
ApplicationWindow {
    width: 700
    height: 550
    visible: true
    property string helpText: "A longer translated explanation that wraps onto multiple lines while the settings panel changes width."
    ScrollView {
        id: scroll
        anchors.fill: parent
        contentWidth: availableWidth
        ColumnLayout {
            width: scroll.availableWidth
            Group {
                name: "Frame Generation"
                GroupEntry {
                    objectName: "entry"
                    title: "Adaptive Frame Generation (Restart)"
                    compactRestartMarker: true
                    description: helpText
                    experimentalLabel: "Experimental"
                    SpinBox { objectName: "control"; Layout.alignment: Qt.AlignRight; from: 0; to: 1000; editable: true }
                }
                GroupEntry {
                    title: "Rendering quality"
                    description: helpText
                    ComboBox { Layout.fillWidth: true; model: ["Quality", "Performance"] }
                }
                GroupEntry {
                    title: "Enable frame generation"
                    description: helpText
                    CheckBox { Layout.alignment: Qt.AlignRight }
                }
            }
        }
    }
}
)QML")).toUtf8());
        require(!engine.rootObjects().empty(), "cannot load settings layout fixture");
        auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().front());
        require(window, "settings fixture is not a window");
        auto* entry = window->findChild<QQuickItem*>(QStringLiteral("entry"));
        auto* control = window->findChild<QQuickItem*>(QStringLiteral("control"));
        require(entry && control, "settings row or control is missing");
        for (const int width : {700, 400, 960, 520, 700}) {
            window->resize(width, 550);
            settle();
            const auto origin = control->mapToItem(entry, QPointF{});
            require(entry->height() > 0 && control->width() > 0,
                "settings row collapsed during resizing");
            require(origin.x() >= 0 && origin.x() + control->width() <= entry->width() + 1,
                "settings control escaped its row");
            window->setProperty("helpText", QStringLiteral("Short help."));
            settle();
            window->setProperty("helpText", QStringLiteral(
                "A much longer translated description which must wrap without feeding its assigned width back into its parent layout's size hints."));
            settle();
        }
        require(layoutWarnings.empty(), "settings rows entered a recursive layout or binding loop");
    } catch (const std::exception& error) {
        std::cerr << "MAKO Renderer: settings layout test failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
