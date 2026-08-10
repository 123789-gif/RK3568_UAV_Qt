#include "ui/control_panel_window.h"
#include "system/process_signal_bridge.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDebug>

#include <cstdlib>

int main(int argc, char *argv[])
{
#if QT_VERSION >= QT_VERSION_CHECK(5, 6, 0)
    // 禁用高DPI缩放，保证窗口坐标与 xrandr 实际像素坐标一致
    QCoreApplication::setAttribute(Qt::AA_DisableHighDpiScaling);
#endif

    QApplication app(argc, argv);

    ProcessSignalBridge signalBridge;
    if (!signalBridge.isInstalled()) {
        qCritical() << "Unable to install process signal bridge:"
                    << signalBridge.errorString();
        return EXIT_FAILURE;
    }

    bool shutdownRequested = false;
    QObject::connect(&signalBridge, &ProcessSignalBridge::terminationRequested,
                     &app, [&app, &shutdownRequested](int signalNumber) {
        if (shutdownRequested) {
            return;
        }
        shutdownRequested = true;
        qInfo() << "Received signal" << signalNumber
                << "- requesting graceful Qt shutdown";
        app.quit();
    });

    ControlPanelWindow window;
    window.showWindows();

    return app.exec();
}
