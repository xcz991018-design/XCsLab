#pragma once
#include <QCoreApplication>
#include <QStandardPaths>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QMap>
#include <QSet>
#include <QCryptographicHash>
#include <QDateTime>
#include <QUrl>
#include "CameraIdentity.hpp"
#include <algorithm>

namespace DeviceCredentialStore {
inline QString defaultPath() {
    // GenericDataLocation is shared by GUI, service and Qt Creator executable names.
    return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation))
        .absoluteFilePath("GaugeReader/credentials/devices.json");
}
inline QStringList legacyPaths() {
    const auto appDir = QCoreApplication::applicationDirPath();
    QStringList paths{QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/credentials/devices.json",
        appDir + "/config/devices.json", appDir + "/devices.json",
        QDir::current().absoluteFilePath("config/devices.json"),
        QDir::current().absoluteFilePath("../config/devices.json"),
        QDir::current().absoluteFilePath("../../config/devices.json")};
    const auto base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    for (const auto* name : {"gauge_reader_gui", "gauge_reader_service", "gauge_reader_test", "GaugeReaderTest"})
        paths.append(base + "/" + name + "/credentials/devices.json");
    paths.removeDuplicates();
    return paths;
}
inline bool atomicWrite(const QString& path, const QByteArray& bytes, QString& error) {
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) { error = "CREDENTIAL_DIRECTORY_FAILED: " + path; return false; }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        error = "CREDENTIAL_WRITE_FAILED: " + path + " | " + file.errorString(); return false;
    }
    return true;
}
inline QString backupPath(const QString& source, const QString& target) {
    const auto parent = QFileInfo(source).absolutePath().toLower().toUtf8();
    const auto folder = QString::fromLatin1(QCryptographicHash::hash(parent,QCryptographicHash::Sha256).toHex().left(16));
    return QDir(QFileInfo(target).absolutePath()).absoluteFilePath("legacy-config/" + folder + "/" + QFileInfo(source).fileName());
}
inline bool archiveFile(const QString& source, const QString& target, QString& error) {
    QFile input(source);
    if (!input.open(QIODevice::ReadOnly)) { error = "LEGACY_CONFIG_READ_FAILED: " + source; return false; }
    const auto bytes = input.readAll();
    const auto destination = backupPath(source,target);
    QFile previous(destination);
    if (previous.exists()) {
        if (!previous.open(QIODevice::ReadOnly)) { error = "LEGACY_BACKUP_READ_FAILED: " + destination; return false; }
        const auto oldBytes = previous.readAll(); previous.close();
        if (oldBytes == bytes) return true;
        // Preserve earlier snapshots if an explicit migration is repeated after an edit.
        const auto suffix = QString::fromLatin1(QCryptographicHash::hash(oldBytes,QCryptographicHash::Sha256).toHex());
        if (!atomicWrite(destination + "." + suffix + ".bak", oldBytes, error)) return false;
    }
    if (!atomicWrite(destination,bytes,error)) return false;
    QFile verify(destination);
    if (!verify.open(QIODevice::ReadOnly) || verify.readAll() != bytes) { error = "LEGACY_BACKUP_VERIFY_FAILED: " + source; return false; }
    return true;
}
inline bool readRegistry(const QString& path, QJsonObject& root, QString& error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { error = "CREDENTIAL_READ_FAILED: " + path; return false; }
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(file.readAll(),&parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject() || !document.object()["devices"].isArray()) {
        error = "CREDENTIAL_JSON_INVALID: " + path; return false;
    }
    root = document.object();
    QSet<QString> ids;
    for (const auto& value : root["devices"].toArray()) {
        const auto id = value.toObject()["camera_id"].toString();
        if (id.isEmpty() || ids.contains(id)) { error = "CREDENTIAL_DEVICE_ID_INVALID: " + path; return false; }
        ids.insert(id);
    }
    return true;
}
inline QJsonObject withCredentials(QJsonObject device, const QJsonArray& records) {
    QJsonObject local;
    for (const auto& value : records) {
        const auto candidate = value.toObject();
        if (candidate["camera_id"] == device["camera_id"]) {
            const auto expected = CameraIdentity::normalize(device["hardware_id"].toString());
            const auto stored = CameraIdentity::normalize(candidate["hardware_id"].toString());
            if (!expected.isEmpty() && !stored.isEmpty() && expected != stored) continue;
            local = candidate; break;
        }
    }
    if (local.isEmpty()) {
        const auto identity = CameraIdentity::normalize(device["hardware_id"].toString());
        int matches = 0;
        if (!identity.isEmpty()) for (const auto& value : records) {
            const auto candidate = value.toObject();
            if (CameraIdentity::normalize(candidate["hardware_id"].toString()) == identity) { local = candidate; ++matches; }
        }
        if (matches != 1) local = {};
    }
    device["password"] = local["password"].toString();
    const QUrl uri(local["rtsp_url"].toString());
    if (!uri.userName().isEmpty()) device["username"] = uri.userName();
    if (!uri.password().isEmpty()) device["password"] = uri.password();
    return device;
}
inline bool migrate(QStringList sources, const QStringList& archives, QString& error,
                    const QString& target = defaultPath(), bool force = false) {
    error.clear();
    QJsonObject current;
    const bool hasTarget = QFileInfo::exists(target);
    if (hasTarget && !readRegistry(target,current,error)) return false;
    for (const auto& source : archives) if (!archiveFile(source,target,error)) return false;
    if (hasTarget && current["system"].toObject()["credential_storage_version"].toInt() == 1 && !force) return true;
    QSet<QString> seen;
    QStringList existing;
    for (const auto& source : sources) {
        const auto path = QFileInfo(source).absoluteFilePath();
        if (path.compare(QFileInfo(target).absoluteFilePath(),Qt::CaseInsensitive) == 0 || !QFileInfo::exists(path) || seen.contains(path.toLower())) continue;
        seen.insert(path.toLower()); existing.append(path);
    }
    std::stable_sort(existing.begin(),existing.end(),[](const QString& a,const QString& b) {
        return QFileInfo(a).lastModified() < QFileInfo(b).lastModified();
    });
    if (hasTarget) existing.append(target); // Existing user data remains authoritative.
    QMap<QString,QJsonObject> devices;
    QJsonObject root;
    for (const auto& path : existing) {
        QJsonObject source;
        if (!readRegistry(path,source,error)) return false;
        if (path != target && !archiveFile(path,target,error)) return false;
        root = source;
        for (const auto& value : source["devices"].toArray()) {
            auto device = value.toObject();
            const auto id = device["camera_id"].toString();
            const auto previous = devices.value(id);
            // Blank credentials from another export never erase a saved nonempty value.
            for (const auto* key : {"username", "password"})
                if (device[key].toString().isEmpty() && !previous[key].toString().isEmpty()) device[key] = previous[key];
            devices.insert(id,device);
        }
    }
    if (hasTarget && !archiveFile(target,target,error)) return false;
    QJsonArray array;
    for (const auto& device : devices) array.append(device);
    root["devices"] = array;
    auto system = root["system"].toObject(); system["credential_storage_version"] = 1; root["system"] = system;
    if (!atomicWrite(target,QJsonDocument(root).toJson(QJsonDocument::Indented),error)) return false;
    QJsonObject verified;
    if (!readRegistry(target,verified,error)) return false;
    if (verified != root) { error = "CREDENTIAL_VERIFY_FAILED: " + target; return false; }
    return true;
}
}
