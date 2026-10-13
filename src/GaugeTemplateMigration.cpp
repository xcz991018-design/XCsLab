#include "GaugeTemplateMigration.hpp"
#include "GaugeTemplateStore.hpp"
#include "GaugeProfile.hpp"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <stdexcept>

QList<QJsonObject> materializeLegacyGaugeTemplates(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("LEGACY_TEMPLATE_READ_FAILED");
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject()) throw std::runtime_error("LEGACY_TEMPLATE_JSON_INVALID");
    const auto root = document.object();
    const auto array = root.contains("profiles") ? root["profiles"] : root["cameras"];
    if (!array.isArray()) throw std::runtime_error("LEGACY_TEMPLATE_ARRAY_REQUIRED");
    QList<QJsonObject> definitions;
    QSet<QString> ids;
    for (const auto& value : array.toArray()) {
        auto definition = value.toObject();
        const auto id = GaugeTemplateStore::idOf(definition);
        if (id.isEmpty() || ids.contains(id)) throw std::runtime_error("LEGACY_TEMPLATE_ID_INVALID");
        ids.insert(id);
        GaugeProfile resolved;
        if (!resolved.loadFromFile(path.toStdString(), id.toStdString())) throw std::runtime_error("LEGACY_TEMPLATE_RESOLVE_FAILED");
        for (const auto& field : GaugeTemplateStore::fields())
            for (const auto& alias : field.aliases) definition.remove(alias);
        for (const auto& key : {"use_model", "gauge_profile", "profile_model", "parent_model_path", "default_profile_id", "profile_id"}) definition.remove(key);
        const auto effective = resolved.toJson();
        for (auto it = effective.begin(); it != effective.end(); ++it) definition[it.key()] = it.value();
        definition["gauge_id"] = id;
        const auto error = GaugeTemplateStore::validate(definition);
        if (!error.isEmpty()) throw std::runtime_error(error.toUtf8().constData());
        definitions.append(definition);
    }
    return definitions;
}
