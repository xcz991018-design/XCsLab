#pragma once
#include <QString>
#include <QUuid>
#include <QXmlStreamReader>

namespace CameraIdentity {
inline QString normalize(QString value) {
    value = value.trimmed();
    if (value.startsWith("urn:uuid:", Qt::CaseInsensitive)) value = value.mid(9);
    else if (value.startsWith("uuid:", Qt::CaseInsensitive)) value = value.mid(5);
    const QUuid uuid(value);
    return uuid.isNull() ? QString() : uuid.toString(QUuid::WithoutBraces).toLower();
}
inline QString archiveId(const QString& hardwareId) {
    const auto uuid = normalize(hardwareId);
    return uuid.isEmpty() ? QString() : "CAM_UUID_" + uuid;
}
// Read only the endpoint inside ProbeMatch; SOAP headers contain unrelated UUIDs.
inline QString discoveryId(const QByteArray& data) {
    QXmlStreamReader reader(data);
    bool match = false, endpoint = false;
    QString result;
    int matchCount = 0;
    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isStartElement()) {
            if (reader.name() == "ProbeMatch") { match = true; ++matchCount; }
            else if (match && reader.name() == "EndpointReference") endpoint = true;
            else if (match && endpoint && reader.name() == "Address") result = normalize(reader.readElementText());
        } else if (reader.isEndElement()) {
            if (reader.name() == "EndpointReference") endpoint = false;
            if (reader.name() == "ProbeMatch") match = false;
        }
    }
    return reader.hasError() || matchCount != 1 ? QString() : result;
}
}
