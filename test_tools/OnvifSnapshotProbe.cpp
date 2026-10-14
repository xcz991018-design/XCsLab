#include "OnvifSoap.hpp"
#include "IsolatedCapture.hpp"
#include "MySqlRepository.hpp"
#include "DeviceCodec.hpp"
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QFile>
#include <QSaveFile>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTextStream>
#include <opencv2/imgcodecs.hpp>

int main(int argc, char** argv) {
    const int workerExit = runCaptureWorker(argc, argv);
    if (workerExit >= 0) return workerExit;
    QCoreApplication app(argc, argv);
    QCommandLineParser cli;
    cli.setApplicationDescription("Read-only ONVIF snapshot capability and latency probe (no RTSP fallback)");
    cli.addHelpOption();
    cli.addOption(QCommandLineOption("pipeline", "Test one device through the production patrol pipeline"));
    cli.addOptions({QCommandLineOption("host", "Camera IP", "ip", "192.168.110.11"),
                   QCommandLineOption("devices", "Read credentials from registry JSON instead of database", "path"),
                   QCommandLineOption("port", "Override ONVIF HTTP port", "port"),
                   QCommandLineOption("count", "Downloads per profile", "count", "5"),
                   QCommandLineOption("output", "Report and snapshot folder", "path", "test_outputs/onvif_snapshot_probe")});
    cli.process(app);
    QTextStream log(stdout);
    const QString host = cli.value("host");
    bool countOk;
    const int count = cli.value("count").toInt(&countOk);
    if (!countOk || count < 1 || count > 20) return 2;
    QJsonObject registry;
    try {
        if (cli.isSet("devices")) {
            QFile file(cli.value("devices"));
            if (!file.open(QIODevice::ReadOnly)) { log << "Cannot read registry\n"; return 2; }
            registry = QJsonDocument::fromJson(file.readAll()).object();
        } else registry = MySqlRepository().loadRegistry();
    } catch (...) { log << "Cannot read database registry; verify saved database settings\n"; return 2; }
    DiscoveredCameraItem device;
    bool found = false;
    for (const auto& value : registry["devices"].toArray()) {
        auto item = deviceFromJson(value.toObject());
        if (item.ipOrSn == host) { device = item; found = true; break; }
    }
    if (!found) { log << "Target camera absent from registry\n"; return 2; }
    int port = device.port;
    if (cli.isSet("port")) port = cli.value("port").toInt();
    if (port < 1 || port > 65535) return 2;
    QDir output(cli.value("output"));
    if (!QDir().mkpath(output.absolutePath())) return 2;
    if (cli.isSet("pipeline")) {
        InspectionTask task;
        task.device = device;
        task.device.port = port;
        task.preferOnvifSnapshot = true;
        task.outputDirectory = output.absolutePath();
        task.runMode = GaugeRunMode::Production;
        InspectionService service(nullptr, 1);
        QEventLoop loop;
        int exitCode = 1;
        QObject::connect(&service, &InspectionService::taskFinished, &loop, [&](const InspectionOutcome& out) {
            log << "Production capture method: " << out.captureMethod << " error: " << out.errorCode
                << " capture_ms: " << out.timings["capture_ms"].toDouble()
                << " recognition_ms: " << out.timings["recognition_ms"].toDouble()
                << " total_ms: " << out.timings["total_ms"].toDouble()
                << " success: " << out.result.success << " value: " << out.result.value
                << " original: " << out.originalPath << "\n";
            exitCode = out.errorCode.isEmpty() && out.result.success ? 0 : 1;
        });
        QObject::connect(&service, &InspectionService::finished, &loop, &QEventLoop::quit);
        service.start({task});
        loop.exec();
        return exitCode;
    }
    QJsonObject report{{"host", host}, {"port", port}, {"tested_at", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}};
    auto saveReport = [&]() {
        QSaveFile file(output.filePath("report.json"));
        const auto bytes = QJsonDocument(report).toJson();
        return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
    };
    QElapsedTimer setup; setup.start();
    QUrl endpoint; endpoint.setScheme("http"); endpoint.setHost(host); endpoint.setPort(port); endpoint.setPath("/onvif/device_service");
    QString error;
    auto data = OnvifSoap::request(endpoint, "http://www.onvif.org/ver10/device/wsdl/GetCapabilities",
        "<tds:GetCapabilities><tds:Category>Media</tds:Category></tds:GetCapabilities>", device.username, device.password, error);
    QString media;
    QXmlStreamReader xml(data);
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement() && xml.name() == "Media") {
            while (xml.readNextStartElement()) {
                if (xml.name() == "XAddr") media = xml.readElementText(); else xml.skipCurrentElement();
            }
        }
    }
    if (media.isEmpty()) {
        report["status"] = "CAPABILITIES_FAILED"; report["error"] = error.isEmpty() ? "No Media XAddr" : error;
        saveReport(); log << "GetCapabilities failed: " << report["error"].toString() << "\n"; return 1;
    }
    QUrl mediaUrl = endpoint.resolved(QUrl(media));
    if (mediaUrl.host() != host) { report["status"] = "MEDIA_HOST_MISMATCH"; saveReport(); return 1; }
    data = OnvifSoap::request(mediaUrl, "http://www.onvif.org/ver10/media/wsdl/GetProfiles", "<trt:GetProfiles/>", device.username, device.password, error);
    QStringList tokens;
    QXmlStreamReader profiles(data);
    while (!profiles.atEnd()) {
        profiles.readNext();
        if (profiles.isStartElement() && profiles.name() == "Profiles") {
            auto token = profiles.attributes().value("token").toString();
            if (!token.isEmpty() && !tokens.contains(token)) tokens.append(token);
        }
    }
    report["setup_ms"] = double(setup.nsecsElapsed()) / 1e6;
    if (tokens.isEmpty()) { report["status"] = "PROFILES_FAILED"; report["error"] = error; saveReport(); log << "GetProfiles failed\n"; return 1; }
    QJsonArray results;
    int successes = 0;
    for (int profile = 0; profile < tokens.size(); ++profile) {
        for (int attempt = 0; attempt < count; ++attempt) {
            QElapsedTimer total; total.start();
            data = OnvifSoap::request(mediaUrl, "http://www.onvif.org/ver10/media/wsdl/GetSnapshotUri",
                QString("<trt:GetSnapshotUri><trt:ProfileToken>%1</trt:ProfileToken></trt:GetSnapshotUri>").arg(OnvifSoap::escape(tokens[profile])), device.username, device.password, error);
            QJsonObject sample{{"profile_index", profile}, {"attempt", attempt + 1}, {"uri_ms", double(total.nsecsElapsed()) / 1e6}};
            QUrl uri = mediaUrl.resolved(QUrl(OnvifSoap::element(data, "Uri")));
            if (data.isEmpty() || OnvifSoap::element(data, "Uri").isEmpty() || uri.host() != host || (uri.scheme() != "http" && uri.scheme() != "https")) {
                sample["status"] = "SNAPSHOT_URI_FAILED";
                sample["total_ms"] = double(total.nsecsElapsed()) / 1e6;
                sample["error"] = error.isEmpty() ? "No valid same-camera HTTP snapshot URI" : error;
                results.append(sample); log << "Profile " << profile << ": GetSnapshotUri failed\n"; break;
            }
            uri.setUserInfo(QString());
            QNetworkAccessManager manager;
            int authAttempts = 0;
            QObject::connect(&manager, &QNetworkAccessManager::authenticationRequired, [&](QNetworkReply*, QAuthenticator* auth) {
                if (++authAttempts == 1) { auth->setUser(device.username); auth->setPassword(device.password); }
            });
            QElapsedTimer download; download.start();
            QNetworkRequest request(uri);
            request.setRawHeader("Cache-Control", "no-cache");
            auto* reply = manager.get(request);
            QEventLoop loop; QTimer timeout; timeout.setSingleShot(true);
            QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
            QObject::connect(&timeout, &QTimer::timeout, &loop, [&]() { reply->abort(); loop.quit(); });
            QObject::connect(reply, &QNetworkReply::readyRead, [&]() { if (reply->bytesAvailable() > 16 * 1024 * 1024) reply->abort(); });
            timeout.start(5000); loop.exec();
            const auto bytes = reply->readAll();
            sample["download_ms"] = double(download.nsecsElapsed()) / 1e6;
            sample["http_status"] = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            sample["network_error_code"] = int(reply->error());
            cv::Mat image;
            QElapsedTimer decode; decode.start();
            if (reply->error() == QNetworkReply::NoError && bytes.size() <= 16 * 1024 * 1024) {
                try { image = cv::imdecode(std::vector<uchar>(bytes.begin(), bytes.end()), cv::IMREAD_COLOR); } catch (...) {}
            }
            sample["decode_ms"] = double(decode.nsecsElapsed()) / 1e6;
            sample["total_ms"] = double(total.nsecsElapsed()) / 1e6;
            sample["status"] = image.empty() ? "DOWNLOAD_OR_DECODE_FAILED" : "OK";
            if (!image.empty()) {
                ++successes; sample["width"] = image.cols; sample["height"] = image.rows;
                const QString suffix = bytes.startsWith(QByteArray::fromHex("ffd8")) ? "jpg"
                    : bytes.startsWith(QByteArray::fromHex("89504e47")) ? "png" : "image";
                const QString name = QString("profile_%1_sample_%2.%3").arg(profile).arg(attempt+1).arg(suffix);
                sample["image_file"] = name;
                QSaveFile file(output.filePath(name));
                sample["image_saved"] = file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
            }
            log << "Profile " << profile << " attempt " << attempt+1 << ": " << sample["status"].toString()
                << " uri=" << sample["uri_ms"].toDouble() << " download=" << sample["download_ms"].toDouble()
                << " decode=" << sample["decode_ms"].toDouble() << " total=" << sample["total_ms"].toDouble()
                << " ms size=" << image.cols << "x" << image.rows << "\n";
            results.append(sample);
        }
    }
    report["samples"] = results; report["successful_downloads"] = successes;
    report["status"] = successes ? "SNAPSHOT_VERIFIED" : "SNAPSHOT_NOT_VERIFIED";
    if (!saveReport()) return 2;
    log << "Report: " << output.absoluteFilePath("report.json") << "\n";
    return successes ? 0 : 1;
}
