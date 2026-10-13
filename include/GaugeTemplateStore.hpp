#pragma once
#include <QJsonObject>
#include <QStringList>
#include <QVector>
#include <QMap>
#include "GaugeTemplateRecord.hpp"
struct GaugeProfile;

struct GaugeTemplateField {
    QString key;
    QString label;
    int page;
    QJsonValue defaultValue;
    double minimum;
    double maximum;
    int decimals;
    QStringList aliases;
};

// Blocking database facade. GUI callers execute operations on workers.
class GaugeTemplateStore {
public:
    explicit GaugeTemplateStore(GaugeTemplateBackend backend = {});
    bool load(QString& error);
    QStringList ids() const;
    QJsonObject profile(const QString& id) const;
    QJsonObject effectiveProfile(const QString& id) const;
    QStringList usedBy(const QString& id) const;
    bool saveProfile(const QJsonObject& profile, bool replacing, QString& error);
    bool removeProfile(const QString& id, const QStringList& usedBy, QString& error);
    static QString idOf(const QJsonObject& profile);
    static QVector<GaugeTemplateField> fields();
    static QVector<GaugeTemplateField> fields(const GaugeProfile& profile);
    static QJsonObject defaults();
    static QString validate(const QJsonObject& profile);
private:
    GaugeTemplateBackend m_backend;
    QMap<QString, GaugeTemplateRecord> m_records;
    bool m_loaded{false};
};
