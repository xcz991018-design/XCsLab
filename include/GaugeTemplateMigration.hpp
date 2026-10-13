#pragma once
#include <QJsonObject>
#include <QString>
#include <QList>

// Explicit one-time import only. Normal application reads never call this.
QList<QJsonObject> materializeLegacyGaugeTemplates(const QString& path);
