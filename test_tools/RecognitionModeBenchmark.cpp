#include "GaugeReaderEngine.hpp"
#include "DeviceCodec.hpp"
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QSaveFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QTextStream>
#include <chrono>
#include <cmath>
#include <algorithm>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCommandLineParser cli;
    cli.addHelpOption();
    cli.addOptions({QCommandLineOption("record", "Existing result.json containing frozen device/template", "path"),
        QCommandLineOption("iterations", "Measured runs per mode after one warm-up per mode", "n", "7"),
        QCommandLineOption("output", "Benchmark report path", "path", "test_outputs/recognition_mode_benchmark.json")});
    cli.process(app);
    bool ok;
    int count = cli.value("iterations").toInt(&ok);
    if (!ok || count < 1 || count > 50 || !cli.isSet("record")) return 2;
    QFile file(cli.value("record"));
    if (!file.open(QIODevice::ReadOnly)) return 2;
    const auto record = QJsonDocument::fromJson(file.readAll()).object();
    const auto device = deviceFromJson(record["device"].toObject());
    const auto frozen = record["profile_snapshot"].toObject();
    QString imagePath = QFileInfo(cli.value("record")).dir().filePath("original.png");
    const auto frame = cv::imread(imagePath.toStdString());
    if (frame.empty() || frozen.isEmpty()) return 2;
    GaugeProfile base;
    if (!base.loadFromJson(frozen, device.defaultProfileId.toStdString())) return 2;
    base.gauge_id = device.cameraId.toStdString();
    base.perspective_correction_enabled = device.perspectiveEnabled;
    base.perspective_pts = {{float(device.pt0_x),float(device.pt0_y)}, {float(device.pt1_x),float(device.pt1_y)},
                           {float(device.pt2_x),float(device.pt2_y)}, {float(device.pt3_x),float(device.pt3_y)}};
    base.center = {float(device.centerX),float(device.centerY)};
    base.scalePoints = device.scalePoints;
    base.pointerBaseRadius = device.pointerBaseRadius;
    base.calibrated_width = device.calibratedWidth; base.calibrated_height = device.calibratedHeight;
    base.auto_detect_roi = false; base.save_debug_images = false;
    const cv::Mat original = frame.clone();
    GaugeReaderEngine engine;
    QJsonArray runs;
    GaugeResult reference;
    bool haveReference = false, equivalent = true;
    for (int round = -1; round < count; ++round) {
        // Alternate which mode runs first to reduce ordering/thermal bias.
        for (int slot = 0; slot < 2; ++slot) {
            const bool production = ((round + 1) % 2 == 0) ? slot == 0 : slot == 1;
            const auto mode = production ? GaugeRunMode::Production : GaugeRunMode::Evidence;
            auto profile = base;
            const auto started = std::chrono::steady_clock::now();
            const auto result = engine.process(frame, profile, mode);
            const double wall = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
            if (!haveReference) { reference = result; haveReference = true; }
            equivalent &= result.success == reference.success && std::abs(result.value-reference.value)<1e-9
                && std::abs(result.confidence-reference.confidence)<1e-9
                && std::abs(result.pointer_angle_deg-reference.pointer_angle_deg)<1e-9
                && result.recognitionQuality == reference.recognitionQuality
                && result.centerInconsistent == reference.centerInconsistent
                && std::abs(result.centerOffsetDist-reference.centerOffsetDist)<1e-9;
            runs.append(QJsonObject{{"mode",gaugeRunModeName(mode)}, {"warmup",round<0}, {"iteration",round+1},
                {"wall_ms",wall}, {"steps_ms",result.stageTimings}, {"success",result.success},
                {"value",result.value}, {"confidence",result.confidence}});
        }
    }
    QJsonObject medians;
    for (const QString mode : {QString("production"), QString("evidence")}) {
        QJsonObject values;
        QStringList keys{"wall_ms"};
        for (const auto& run : runs) {
            auto steps = run.toObject()["steps_ms"].toObject();
            for (auto it=steps.begin(); it!=steps.end(); ++it) if(it.value().isDouble() && !keys.contains(it.key())) keys.append(it.key());
        }
        for (const auto& key : keys) {
            std::vector<double> samples;
            for (const auto& run : runs) {
                const auto obj=run.toObject();
                if(obj["warmup"].toBool() || obj["mode"].toString()!=mode) continue;
                const auto value=key=="wall_ms" ? obj[key] : obj["steps_ms"].toObject()[key];
                if(value.isDouble()) samples.push_back(value.toDouble());
            }
            if(samples.empty()) continue;
            std::sort(samples.begin(),samples.end());
            const auto n=samples.size(); values[key]=(samples[(n-1)/2]+samples[n/2])/2;
        }
        medians[mode]=values;
    }
#ifdef NDEBUG
    const QString build="Release";
#else
    const QString build="Debug";
#endif
    QJsonObject report{{"build",build},{"opencv_threads",cv::getNumThreads()}, {"opencv_version",CV_VERSION},
        {"source_task_id",record["task_id"]},{"source_mode",record["mode"]}, {"image",QFileInfo(imagePath).absoluteFilePath()},
        {"width",frame.cols},{"height",frame.rows}, {"template_id",frozen["_database_template_id"]},
        {"template_revision",frozen["_database_template_revision"]}, {"equivalent_results",equivalent},
        {"input_unchanged",cv::norm(frame,original,cv::NORM_INF)==0}, {"median_ms",medians}, {"runs",runs}};
    const QString path=cli.value("output");
    if(!QDir().mkpath(QFileInfo(path).absolutePath())) return 2;
    QSaveFile output(path); const auto bytes=QJsonDocument(report).toJson();
    if(!output.open(QIODevice::WriteOnly) || output.write(bytes)!=bytes.size() || !output.commit()) return 2;
    QTextStream(stdout) << "BENCHMARK " << build << " " << QJsonDocument(medians).toJson(QJsonDocument::Compact)
        << " equivalent=" << equivalent << " report=" << QFileInfo(path).absoluteFilePath() << "\n";
    return equivalent ? 0 : 1;
}
