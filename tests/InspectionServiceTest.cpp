#include "InspectionService.hpp"
#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <QTemporaryDir>
#include <QFile>
#include <QSet>
#include <QJsonDocument>
#include <cmath>
#include <thread>
#include <chrono>
#include <iostream>

class FakeSource : public ICameraSource {
public:
    FakeSource(std::atomic<int>& active, std::atomic<int>& peak, int identity)
        : active(active), peak(peak), identity(identity) {}
    bool open() override { return true; }
    bool isOpened() const override { return true; }
    void close() override {}
    std::string getSourceName() const override { return "fake"; }
    bool grabFrame(cv::Mat& frame) override {
        const int current = ++active;
        int old = peak.load();
        while (old < current && !peak.compare_exchange_weak(old, current)) {}
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        frame = cv::Mat(16, 16, CV_8UC3, cv::Scalar(identity, identity, identity));
        --active;
        return true;
    }
private:
    std::atomic<int>& active;
    std::atomic<int>& peak;
    int identity;
};

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir output;
    if (!output.isValid()) return 1;
    std::atomic<int> active{0}, peak{0};
    InspectionService service(nullptr, 2, [&](const InspectionTask& task) -> CameraSourcePtr {
        if (task.device.cameraId == "9") throw std::runtime_error("Uncalibrated camera acquisition must never start");
        if (task.device.cameraId == "0") return nullptr;
        return std::make_shared<FakeSource>(active, peak, task.device.cameraId.toInt());
    });
    QList<InspectionTask> tasks;
    for (int i = 0; i < 10; ++i) {
        InspectionTask task;
        task.device.cameraId = QString::number(i);
        task.device.protocolType = CameraProtocolType::ONVIF_RTSP_STREAM;
        if (i != 9) {
            task.device.hasCalibration = task.device.perspectiveEnabled = true;
            task.device.calibratedWidth = task.device.calibratedHeight = 16;
            task.device.pt0_x=8; task.device.pt0_y=1;
            task.device.pt1_x=14; task.device.pt1_y=8;
            task.device.pt2_x=8; task.device.pt2_y=14;
            task.device.pt3_x=1; task.device.pt3_y=8;
            task.device.centerX=task.device.centerY=8;
            task.device.scalePoints=QJsonArray{QJsonObject{{"x",3},{"y",12},{"ratio",0}},QJsonObject{{"x",12},{"y",12},{"ratio",1}}};
        }
        task.outputDirectory = output.path();
        tasks.append(task);
    }
    int count = 0;
    QSet<QString> received;
    bool valid = true, cancelled = false, timedOut = false;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, [&]() { timedOut = true; loop.quit(); });
    QObject::connect(&service, &InspectionService::taskFinished, &loop, [&](const InspectionOutcome& out) {
        ++count;
        const QString expected = out.cameraId == "9" ? "CALIBRATION_REQUIRED" :
            out.cameraId == "0" ? "UNSUPPORTED_PROTOCOL" : "TEMPLATE_DATABASE_FAILED";
        if (out.errorCode != expected)
            std::cerr << out.errorCode.toStdString() << ": " << out.errorMessage.toStdString() << "\n";
        valid &= out.errorCode == expected;
        valid &= QFile::exists(output.path() + "/" + out.taskId + "/result.json");
        valid &= out.capturedFrame.empty();
        QFile timingFile(output.path() + "/" + out.taskId + "/timings.json");
        valid &= timingFile.open(QIODevice::ReadOnly);
        const auto metrics = QJsonDocument::fromJson(timingFile.readAll()).object();
        valid &= metrics["task_id"].toString() == out.taskId;
        valid &= metrics["camera_id"].toString() == out.cameraId;
        valid &= metrics["timings_ms"].toObject() == out.timings;
        for (const auto* key : {"queue_ms", "capture_ms", "recognition_queue_ms", "recognition_ms",
                                "storage_queue_ms", "image_save_ms", "result_save_ms", "total_ms"}) {
            valid &= out.timings[key].isDouble() && std::isfinite(out.timings[key].toDouble()) && out.timings[key].toDouble() >= 0;
        }
        valid &= out.timings["database_write_ms"].isNull();
        double measured = 0;
        for (const auto* key : {"queue_ms", "capture_ms", "recognition_queue_ms", "recognition_ms",
                                "storage_queue_ms", "image_save_ms", "result_save_ms"})
            measured += out.timings[key].toDouble();
        valid &= out.timings["total_ms"].toDouble() + 0.01 >= measured;
        if (out.cameraId != "0" && out.cameraId != "9") valid &= out.timings["capture_ms"].toDouble() >= 25;
        if (out.cameraId.toInt() >= 3) valid &= out.timings["queue_ms"].toDouble() >= 25;
        if (out.cameraId == "0" || out.cameraId == "9") valid &= out.originalPath.isEmpty();
        else {
            const cv::Mat image = cv::imread(out.originalPath.toStdString());
            valid &= !image.empty() && image.rows == 16 && image.cols == 16;
            const int identity = out.cameraId.toInt();
            if (!image.empty()) valid &= cv::norm(image, cv::Mat(16, 16, CV_8UC3, cv::Scalar(identity, identity, identity)), cv::NORM_INF) == 0;
        }
        valid &= !received.contains(out.cameraId) && out.cameraId.toInt() >= 0 && out.cameraId.toInt() < tasks.size();
        received.insert(out.cameraId);
    });
    QObject::connect(&service, &InspectionService::finished, &loop, [&](bool stopped) { cancelled = stopped; loop.quit(); });
    if (!service.start(tasks) || service.start(tasks)) return 1;
    timeout.start(10000);
    loop.exec();
    if (timedOut || !valid || cancelled || count != 10 || peak > 2 || peak < 1) {
        std::cerr << "First pass: valid=" << valid << " count=" << count << " peak=" << peak << " timeout=" << timedOut << "\n";
        return 1;
    }
    count = 0;
    if (!service.start(tasks)) return 1;
    service.stop();
    timeout.start(10000);
    loop.exec();
    if (timedOut || !cancelled || count != 0 || service.isRunning()) return 1;
    QFile blocked(output.path() + "/blocked");
    if (!blocked.open(QIODevice::WriteOnly)) return 1;
    blocked.close();
    auto task = tasks[0];
    task.outputDirectory = blocked.fileName();
    InspectionService storageTest(nullptr, 1, [](const InspectionTask&) -> CameraSourcePtr { return nullptr; });
    QString storageError;
    QObject::connect(&storageTest, &InspectionService::taskFinished, &loop,
                     [&](const InspectionOutcome& out) { storageError = out.errorCode; });
    QObject::connect(&storageTest, &InspectionService::finished, &loop, [&]() { loop.quit(); });
    storageTest.start({task});
    timeout.start(10000);
    loop.exec();
    if (timedOut || storageError != "STORAGE_FAILED") return 1;
    std::cout << "Bounded pipeline, device identity, persisted failures, unsupported protocol, storage failure, cancellation and restart passed\n";
    return 0;
}
