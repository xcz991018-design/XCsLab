#pragma once
#include "InspectionService.hpp"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QDir>
#include <QSaveFile>
#include <QStorageInfo>
#include <QUuid>
#include <algorithm>

namespace EvidenceArchive {
struct Policy {
    int normalDays{7};
    int abnormalDays{90};
    static Policy fromEnvironment() {
        Policy p;
        bool ok = false;
        int n = qEnvironmentVariableIntValue("GAUGE_NORMAL_IMAGE_DAYS", &ok);
        if (ok && n >= 1 && n <= 3650) p.normalDays = n;
        n = qEnvironmentVariableIntValue("GAUGE_ABNORMAL_IMAGE_DAYS", &ok);
        if (ok && n >= p.normalDays && n <= 36500) p.abnormalDays = n;
        p.abnormalDays = std::max(p.normalDays, p.abnormalDays);
        return p;
    }
};
inline bool abnormal(const InspectionOutcome& out) {
    return out.safetyAssessment["status"].toString() == "out_of_range" || !out.errorCode.isEmpty() || !out.recognitionPerformed || !out.result.success ||
        out.result.confidence < 0.4 || out.result.centerInconsistent || !out.result.warningMessage.empty() ||
        out.result.recognitionQuality["status"].toString() != "trusted";
}
inline QJsonObject software() {
    static const QJsonObject info = []() {
        QFile executable(QCoreApplication::applicationFilePath());
        QCryptographicHash digest(QCryptographicHash::Sha256);
        const bool hashed = executable.open(QIODevice::ReadOnly) && digest.addData(&executable);
#ifdef NDEBUG
        const QString build = "Release";
#else
        const QString build = "Debug";
#endif
        return QJsonObject{{"application_version", QCoreApplication::applicationVersion()},
            {"build",build}, {"opencv",CV_VERSION},
            {"qt",qVersion()}, {"executable_sha256",hashed ? QString::fromLatin1(digest.result().toHex()) : QString()}};
    }();
    return info;
}
inline QString imageSuffix(const std::vector<uchar>& bytes) {
    if (bytes.size() >= 3 && bytes[0]==0xff && bytes[1]==0xd8 && bytes[2]==0xff) return ".jpg";
    if (bytes.size() >= 8 && bytes[0]==0x89 && bytes[1]=='P' && bytes[2]=='N' && bytes[3]=='G') return ".png";
    return {};
}
// Only fixed evidence filenames inside a non-symlink UUID directory may be removed.
// Metadata, timing files, pending outbox records and unknown files are retained.
inline bool expireLocalImages(const QString& root, const QString& id, const QDateTime& now) {
    if (QUuid(id).isNull()) return false;
    const QFileInfo directory(QDir(root).filePath(id));
    if (!directory.exists()) return true;
    if (directory.isSymLink() || !directory.isDir()) return false;
    QDir folder(directory.absoluteFilePath());
    QFile source(folder.filePath("result.json"));
    if (!source.open(QIODevice::ReadOnly)) return false;
    auto record=QJsonDocument::fromJson(source.readAll()).object(); source.close();
    if (record["task_id"].toString()!=id) return false;
    for (const auto& name : {"original.png","original.jpg","rectified.png","rectified.jpg","recognition.png",
                              "recognition.jpg","gray.png","diagnostic_hud.png"}) {
        QFileInfo file(folder.filePath(name));
        if (file.isSymLink()) return false;
        if (file.exists() && !QFile::remove(file.absoluteFilePath())) return false;
    }
    record["images_expired_at"]=now.toUTC().toString(Qt::ISODateWithMs);
    record["review_image_available"]=false;
    QSaveFile target(source.fileName()); const auto bytes=QJsonDocument(record).toJson();
    return target.open(QIODevice::WriteOnly) && target.write(bytes)==bytes.size() && target.commit();
}
}
