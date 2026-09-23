#include <QApplication>
#include "MainWindow.hpp"

int main(int argc, char *argv[]) {
    // Force OpenCV FFmpeg backend to use RTSP over TCP with zero latency, low delay & no buffering
    qputenv("OPENCV_FFMPEG_CAPTURE_OPTIONS", "fflags;nobuffer|flags;low_delay|framedrop|max_delay;0|rtsp_transport;tcp|stimeout;3000000");

    QApplication app(argc, argv);
    
    // Set High DPI scaling policy for modern Windows displays
    #if QT_VERSION >= QT_VERSION_CHECK(5, 6, 0)
    QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    #endif

    MainWindow window;
    window.show();

    return app.exec();
}

