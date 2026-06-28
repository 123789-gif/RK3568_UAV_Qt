#include "ui/control_panel_window.h"

#include <QApplication>
#include <QCoreApplication>

int main(int argc, char *argv[])
{
#if QT_VERSION >= QT_VERSION_CHECK(5, 6, 0)
    // 禁用高DPI缩放，保证窗口坐标与 xrandr 实际像素坐标一致
    QCoreApplication::setAttribute(Qt::AA_DisableHighDpiScaling);
#endif

    QApplication app(argc, argv);

    ControlPanelWindow window;
    window.showWindows();

    return app.exec();
}
