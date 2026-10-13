#pragma once
#include <QJsonObject>
#include <QStringList>
#include <functional>

struct GaugeTemplateRecord {
    QString id;
    QJsonObject definition;
    quint64 revision{0};
    QStringList usedBy;
};

// Optional test backend; production operations always use MySqlRepository.
struct GaugeTemplateBackend {
    std::function<QList<GaugeTemplateRecord>()> load;
    std::function<GaugeTemplateRecord(const QJsonObject&, quint64)> save;
    std::function<void(const QString&, quint64)> remove;
};
