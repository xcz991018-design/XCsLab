#pragma once
#include "InspectionCalibration.hpp"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>

// One atomic record binds the last successful calibration to its unmodified
// PNG original. No credentials, live frames or inspection images are archived.
namespace CalibrationArchive {
inline QString path(const QString& cameraId,const QString& root={}) {
    const auto directory=root.isEmpty()?QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)+"/GaugeReader/calibrations":root;
    const auto key=QCryptographicHash::hash(cameraId.toUtf8(),QCryptographicHash::Sha256).toHex();
    return QDir(directory).filePath(QString::fromLatin1(key)+".json");
}
inline bool matches(const DiscoveredCameraItem& a,const DiscoveredCameraItem& b) {
    return a.cameraId==b.cameraId&&deviceToJson(a)["calibration"]==deviceToJson(b)["calibration"];
}
inline QString save(const DiscoveredCameraItem& device,const cv::Mat& raw,const QString& root={}) {
    try {
        if(!canRecognizeDevice(device)||raw.empty()||raw.cols!=device.calibratedWidth||raw.rows!=device.calibratedHeight)
            return QStringLiteral(u"标定原图尺寸或标定参数无效");
        std::vector<uchar> png;
        if(!cv::imencode(".png",raw,png))return QStringLiteral(u"标定原图编码失败");
        const QByteArray bytes(reinterpret_cast<const char*>(png.data()),int(png.size()));
        const QJsonObject record{{"schema_version",1},{"saved_at",QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
            {"device",deviceToJson(device)},{"original_png",QString::fromLatin1(bytes.toBase64())},
            {"sha256",QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex())}};
        const auto destination=path(device.cameraId,root);
        if(!QDir().mkpath(QFileInfo(destination).absolutePath()))return QStringLiteral(u"无法创建标定原图归档目录");
        QSaveFile file(destination);const auto data=QJsonDocument(record).toJson(QJsonDocument::Compact);
        if(!file.open(QIODevice::WriteOnly)||file.write(data)!=data.size()||!file.commit())return file.errorString();
        return {};
    }catch(const std::exception& e){return QString::fromUtf8(e.what());}
}
struct Record {DiscoveredCameraItem device;cv::Mat original;QString error;bool valid() const{return error.isEmpty()&&!original.empty();}};
inline Record load(const QString& cameraId,const QString& root={}) {
    Record out;QFile file(path(cameraId,root));
    if(!file.open(QIODevice::ReadOnly)){out.error=QStringLiteral(u"未找到上次标定原图归档（旧版标定未自动保存原图）");return out;}
    try {
        const auto record=QJsonDocument::fromJson(file.readAll()).object();
        out.device=deviceFromJson(record["device"].toObject());
        const auto bytes=QByteArray::fromBase64(record["original_png"].toString().toLatin1());
        if(record["schema_version"].toInt()!=1||out.device.cameraId!=cameraId||!canRecognizeDevice(out.device)||bytes.isEmpty()||
           QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex()!=record["sha256"].toString().toLatin1())
            throw std::runtime_error("Calibration archive validation failed");
        out.original=cv::imdecode(std::vector<uchar>(bytes.begin(),bytes.end()),cv::IMREAD_COLOR);
        if(out.original.empty()||out.original.cols!=out.device.calibratedWidth||out.original.rows!=out.device.calibratedHeight)
            throw std::runtime_error("Calibration original dimensions do not match");
    }catch(const std::exception& e){out.original.release();out.error=QString::fromUtf8(e.what());}
    return out;
}
}
