#include "IsolatedCapture.hpp"
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QElapsedTimer>
#include <QThread>
#include <QEventLoop>
#include <QTimer>
#include <QTemporaryDir>
#include <QSaveFile>
#include <thread>
#include <cstdio>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc == 3 && QString::fromLocal8Bit(argv[1]) == "--capture-preview-worker") {
        QFile input;
        input.open(stdin, QIODevice::ReadOnly);
        const auto request = QJsonDocument::fromJson(input.readLine()).object();
        const QString folder = QString::fromLocal8Bit(argv[2]);
        int requests = 0;
        while (true) {
            const auto command = input.readLine();
            if (command.isEmpty()) { QThread::msleep(10); continue; }
            if (request["camera_id"].toString() == "hang") { QThread::sleep(60); return 3; }
            const int sequence = command.trimmed().toInt();
            ++requests;
            cv::Mat frame(24, 24, CV_8UC3, cv::Scalar(requests, 90, 120));
            if (!cv::imwrite((folder + QString("/frame_%1.png").arg(sequence)).toStdString(), frame)) return 3;
            QJsonObject json;
            json["failure"] = 0;
            json["sequence"] = sequence;
            json["captured_at_ms"] = 1700000000000.0;
            QSaveFile output(folder + QString("/result_%1.json").arg(sequence));
            if (!output.open(QIODevice::WriteOnly) || output.write(QJsonDocument(json).toJson()) < 0 || !output.commit()) return 3;
        }
    }
    if (argc == 3 && QString::fromLocal8Bit(argv[1]) == "--capture-worker") {
        QFile input;
        input.open(stdin, QIODevice::ReadOnly);
        const auto request = QJsonDocument::fromJson(input.readAll()).object();
        const QString id = request["camera_id"].toString();
        if (id == "hang") { QThread::sleep(60); return 3; }
        if (id == "crash") return 3;
        if (id == "healthy" && !request["_prefer_onvif_snapshot"].toBool()) return 3;
        const QString folder = QString::fromLocal8Bit(argv[2]);
        cv::Mat frame(24, 24, CV_8UC3, cv::Scalar(40, 90, 120));
        if (!cv::imwrite((folder + "/frame.png").toStdString(), frame)) return 3;
        QFile output(folder + "/result.json");
        if (!output.open(QIODevice::WriteOnly)) return 3;
        output.write("{\"failure\":0,\"captured_at_ms\":1700000000000}");
        return 0;
    }
    InspectionTask task;
    task.device.cameraId = "hang";
    task.captureTimeoutMs = 300;
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    QElapsedTimer timer;
    timer.start();
    const auto timeout = isolatedCapture(task, cancel);
    if (timeout.failure != CaptureFailure::Timeout || !timeout.frame.empty() || timer.elapsed() > 2000) { fprintf(stderr, "timeout: %s\n", captureFailureName(timeout.failure)); return 1; }
    task.captureTimeoutMs = 5000;
    std::thread stopper([cancel]() { std::this_thread::sleep_for(std::chrono::milliseconds(150)); cancel->store(true); });
    timer.restart();
    const auto stopped = isolatedCapture(task, cancel);
    stopper.join();
    if (stopped.failure != CaptureFailure::Cancelled || !stopped.frame.empty() || timer.elapsed() > 2000) { fprintf(stderr, "cancel: %s\n", captureFailureName(stopped.failure)); return 1; }
    cancel->store(false);
    task.device.cameraId = "crash";
    if (isolatedCapture(task, cancel).failure != CaptureFailure::WorkerFailed) return 1;
    task.device.cameraId = "healthy";
    task.preferOnvifSnapshot = true;
    const auto healthy = isolatedCapture(task, cancel);
    if (!healthy.ok() || healthy.frame.at<cv::Vec3b>(0, 0) != cv::Vec3b(40, 90, 120)) { fprintf(stderr, "healthy: %s\n", captureFailureName(healthy.failure)); return 1; }
    {
        IsolatedPreview preview;
        auto first = preview.read(task, cancel);
        auto second = preview.read(task, cancel);
        if (!first.ok() || !second.ok() || first.frame.at<cv::Vec3b>(0, 0)[0] != 1 || second.frame.at<cv::Vec3b>(0, 0)[0] != 2) { fprintf(stderr, "preview: %s / %s\n", captureFailureName(first.failure), captureFailureName(second.failure)); return 1; }
    }
    {
        IsolatedPreview preview;
        auto hanging = task;
        hanging.device.cameraId = "hang";
        hanging.captureTimeoutMs = 300;
        const auto result = preview.read(hanging, cancel);
        if (result.failure != CaptureFailure::Timeout) { fprintf(stderr, "preview hang: %s\n", captureFailureName(result.failure)); return 1; }
    }
    QTemporaryDir output;
    InspectionService service(nullptr, 1);
    task.captureOnly = true;
    task.returnImages = true;
    task.outputDirectory = output.path();
    task.device.protocolType = CameraProtocolType::ONVIF_RTSP_STREAM;
    InspectionTask hung = task;
    hung.device.cameraId = "hang";
    hung.captureTimeoutMs = 300;
    QEventLoop loop;
    QTimer guard;
    guard.setSingleShot(true);
    int received = 0;
    bool valid = true, finished = false;
    QObject::connect(&service, &InspectionService::taskFinished, &loop, [&](const InspectionOutcome& outcome) {
        ++received;
        if (outcome.cameraId == "hang")
            valid &= outcome.errorCode == "CAPTURE_TIMEOUT" && !outcome.recognitionPerformed && outcome.originalPath.isEmpty();
        else valid &= outcome.errorCode.isEmpty() && !outcome.originalPath.isEmpty();
    });
    QObject::connect(&service, &InspectionService::finished, &loop, [&](bool) { finished = true; loop.quit(); });
    QObject::connect(&guard, &QTimer::timeout, &loop, &QEventLoop::quit);
    if (!output.isValid() || !service.start({hung, task})) return 1;
    guard.start(7000);
    loop.exec();
    if (!finished || !valid || received != 2) return 1;
    return 0;
}
