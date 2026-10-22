#include <QApplication>
#include "UiTheme.hpp"
#include <QThreadPool>
#include "MainWindow.hpp"
#include "AccessControl.hpp"
#include "IsolatedCapture.hpp"

int main(int argc, char *argv[]) {
    const int workerExit = runCaptureWorker(argc, argv);
    if (workerExit >= 0) return workerExit;
    // Force OpenCV FFmpeg backend to use RTSP over TCP with zero latency, low delay & no buffering
    qputenv("OPENCV_FFMPEG_CAPTURE_OPTIONS", "fflags;nobuffer|flags;low_delay|framedrop|max_delay;0|rtsp_transport;tcp|stimeout;3000000");

    QApplication app(argc, argv);
    app.setStyleSheet(UiTheme::scrollAndTableStyle());
    
    // Set High DPI scaling policy for modern Windows displays
    #if QT_VERSION >= QT_VERSION_CHECK(5, 6, 0)
    QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    #endif

    constexpr int logoutExitCode = 1001;
    for (;;) {
        QApplication::setQuitOnLastWindowClosed(false);
        if (!authenticateApplication()) return 0;
        int result;
        {
            MainWindow window;
            QObject::connect(&window, &MainWindow::logoutRequested, &app, [&app, logoutExitCode] { app.exit(logoutExitCode); });
            window.showMaximized();
            result = app.exec();
        } // Drain session workers before changing identity or allowing a new login.
        QThreadPool::globalInstance()->waitForDone();
        AccessControl::beginGui();
        if (result != logoutExitCode) return result;
    }
}

