#include "MySqlRepository.hpp"
#include "DeviceCodec.hpp"
#include "GaugeReaderEngine.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QTextStream>
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    if(argc<3) return 2;
    try {
        const QString source=QString::fromLocal8Bit(argv[1]), output=QFileInfo(QString::fromLocal8Bit(argv[2])).absoluteFilePath();
        QDir().mkpath(output);
        QJsonObject definition; DiscoveredCameraItem device; cv::Mat frame;
        if(source.endsWith(".json")) {
            QFile file(source); if(!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Record read failed");
            const auto root=QJsonDocument::fromJson(file.readAll()).object();
            definition=root["profile_snapshot"].toObject(); device=deviceFromJson(root["device"].toObject());
            frame=cv::imread((QFileInfo(source).dir().filePath("original.png")).toStdString());
        } else {
            const auto stored=MySqlRepository().original(source);
            definition=stored.inspection.profile; device=stored.inspection.device;
            std::vector<uchar> bytes(stored.bytes.begin(),stored.bytes.end()); frame=cv::imdecode(bytes,cv::IMREAD_COLOR);
            QFile original(output+"/original.png"); if(!original.open(QIODevice::WriteOnly)) throw std::runtime_error("Original export failed");
            original.write(stored.bytes); original.close();
            QFile record(output+"/source.json"); record.open(QIODevice::WriteOnly);
            record.write(QJsonDocument(QJsonObject{{"task_id",source},{"device",deviceToJson(device)},
                {"profile_snapshot",definition},{"stored_value",stored.inspection.value}}).toJson());
        }
        if(frame.empty()) throw std::runtime_error("Image decode failed");
        GaugeProfile profile; if(!profile.loadFromJson(definition,device.defaultProfileId.toStdString())) throw std::runtime_error("Profile invalid");
        profile.gauge_id=device.cameraId.toStdString(); profile.perspective_correction_enabled=device.perspectiveEnabled;
        profile.perspective_pts={{float(device.pt0_x),float(device.pt0_y)},{float(device.pt1_x),float(device.pt1_y)},
            {float(device.pt2_x),float(device.pt2_y)},{float(device.pt3_x),float(device.pt3_y)}};
        profile.center={float(device.centerX),float(device.centerY)}; profile.scalePoints=device.scalePoints;
        profile.pointerBaseRadius=device.pointerBaseRadius; profile.calibrated_width=device.calibratedWidth;
        profile.calibrated_height=device.calibratedHeight; profile.auto_detect_roi=false; profile.save_debug_images=false;
        GaugeReaderEngine engine; const auto result=engine.process(frame,profile,GaugeRunMode::Debug);
        if(!result.step2_rectified.empty()) cv::imwrite((output+"/rectified.png").toStdString(),result.step2_rectified);
        if(!result.step4_recognition.empty()) cv::imwrite((output+"/recognition.png").toStdString(),result.step4_recognition);
        if(!result.grayImage.empty()) cv::imwrite((output+"/gray.png").toStdString(),result.grayImage);
        if(!result.diagnosticHudImage.empty()) cv::imwrite((output+"/hud.png").toStdString(),result.diagnosticHudImage);
        QFile diagnostics(output+"/diagnostics.txt"); diagnostics.open(QIODevice::WriteOnly);
        diagnostics.write(QByteArray::fromStdString(result.pointerDetectorResult.detailedDebugText));
        QJsonObject summary{{"success",result.success},{"value",result.success ? QJsonValue(result.value) : QJsonValue(QJsonValue::Null)},
            {"angle",result.pointer_angle_deg},
            {"confidence",result.confidence},{"center_x",result.center.x},{"center_y",result.center.y},
            {"warning",QString::fromStdString(result.warningMessage)}};
        QFile report(output+"/replay.json"); report.open(QIODevice::WriteOnly); report.write(QJsonDocument(summary).toJson());
        QTextStream(stdout)<<QJsonDocument(summary).toJson(QJsonDocument::Compact)<<"\n";
        return 0;
    } catch(const std::exception& error) {QTextStream(stderr)<<error.what()<<"\n";return 1;}
}
