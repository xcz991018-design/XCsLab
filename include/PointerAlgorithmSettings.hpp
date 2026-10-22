#pragma once
#include "GaugeTemplateStore.hpp"
#include "GaugeProfile.hpp"
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QLockFile>
#include <QJsonDocument>
#include <QCryptographicHash>
#include <stdexcept>

namespace PointerAlgorithmSettings {
inline QString path() {
    return qEnvironmentVariable("GAUGE_ALGORITHM_CONFIG_PATH",
        QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)+"/GaugeReader/pointer_algorithm_parameters.json");
}
inline void fail(const QString& text) { throw std::runtime_error(text.toUtf8().constData()); }
inline QJsonObject withoutAlgorithmFields(QJsonObject profile) {
    for (const auto& field : GaugeTemplateStore::fields()) {
        if (field.page==0) continue;
        profile.remove(field.key);
        for (const auto& alias : field.aliases) profile.remove(alias);
    }
    if (profile["pointer_detector_global_config"].isObject())
        profile["pointer_detector_global_config"]=withoutAlgorithmFields(profile["pointer_detector_global_config"].toObject());
    profile.remove("_algorithm_parameters_sha256");
    return profile;
}
inline QJsonObject resolve(const QJsonObject& definition) {
    const auto id=GaugeTemplateStore::idOf(definition);
    if (id.isEmpty()) fail(QStringLiteral(u"算法参数缺少模板编号。"));
    const auto filename=path();
    if (!QDir().mkpath(QFileInfo(filename).absolutePath())) fail(QStringLiteral(u"无法创建算法参数文件目录：%1").arg(filename));
    QLockFile lock(filename+".lock");
    if (!lock.tryLock(5000)) fail(QStringLiteral(u"算法参数文件正在使用，请稍后重试：%1").arg(filename));
    QJsonObject root{{"version",1},{"profiles",QJsonObject{}}};
    QFile source(filename);
    if (source.exists()) {
        if (!source.open(QIODevice::ReadOnly)) fail(QStringLiteral(u"无法读取算法参数文件：%1").arg(filename));
        QJsonParseError error;
        const auto document=QJsonDocument::fromJson(source.readAll(),&error); source.close();
        if (error.error!=QJsonParseError::NoError || !document.isObject() || document.object()["version"].toInt()!=1 ||
            !document.object()["profiles"].isObject()) fail(QStringLiteral(u"算法参数 JSON 格式无效：%1").arg(filename));
        root=document.object();
    }
    auto profiles=root["profiles"].toObject();
    if (!profiles.contains(id)) {
        if (definition["_algorithm_parameters_template_id"].toString()==id)
            fail(QStringLiteral(u"模板 %1 的独立算法参数已丢失，请恢复文件：%2").arg(id,filename));
        GaugeProfile effective;
        if (!effective.loadFromJson(definition)) fail(QStringLiteral(u"无法读取模板现有算法参数：%1").arg(id));
        QJsonObject parameters;
        for (const auto& field : GaugeTemplateStore::fields(effective))
            if (field.page>0) parameters[field.key]=field.defaultValue;
        profiles[id]=parameters; root["profiles"]=profiles;
        QSaveFile target(filename); const auto bytes=QJsonDocument(root).toJson(QJsonDocument::Indented);
        if (!target.open(QIODevice::WriteOnly) || target.write(bytes)!=bytes.size() || !target.commit())
            fail(QStringLiteral(u"无法保存算法参数文件：%1").arg(filename));
    }
    if (!profiles[id].isObject()) fail(QStringLiteral(u"模板 %1 的算法参数必须为 JSON 对象。").arg(id));
    const auto parameters=profiles[id].toObject();
    auto merged=withoutAlgorithmFields(definition);
    for (const auto& field : GaugeTemplateStore::fields()) {
        if (field.page==0) continue;
        if (!parameters.contains(field.key)) fail(QStringLiteral(u"模板 %1 的算法参数缺少 %2。").arg(id,field.key));
        merged[field.key]=parameters[field.key];
    }
    const auto error=GaugeTemplateStore::validate(merged);
    if (!error.isEmpty()) fail(QStringLiteral(u"算法参数文件校验失败：%1").arg(error));
    merged["_algorithm_parameters_sha256"]=QString::fromLatin1(QCryptographicHash::hash(
        QJsonDocument(parameters).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex());
    return merged;
}
}
