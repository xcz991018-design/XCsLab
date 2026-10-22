#pragma once
#include "InspectionService.hpp"
#include "AccessControl.hpp"
#include <QJsonObject>
#include <QByteArray>
#include "GaugeTemplateRecord.hpp"

struct DatabaseSettings {
    bool enabled{true};
    QString host{QStringLiteral(u"127.0.0.1")};
    QString database{QStringLiteral(u"BYS_GaugeReaderDB")};
    QString user{QStringLiteral(u"root")};
    QString password;
    unsigned int port{3306};
    QString tlsMode{QStringLiteral(u"required")};
    QString caFile;
    QString configurationError;
    int maxImageBytes{32 * 1024 * 1024};
    static DatabaseSettings fromEnvironment();
};
struct StoredInspection {
    QString taskId, cameraId, parentTaskId, unit, errorCode, errorMessage;
    QString inputKind, qualityStatus, processStatus;
    QDateTime capturedAt, processedAt;
    double value{0}, confidence{0};
    bool success{false}, recognitionPerformed{false}, hasOriginal{false};
    DiscoveredCameraItem device;
    QJsonObject profile;
    QJsonObject latestReview;
    QJsonObject resultSnapshot;
};
struct HistoryDeletion {
    int tasks{0}, images{0};
    qint64 imageBytes{0};
};
struct StoredImage {
    QByteArray bytes;
    QString sha256;
    int width{0}, height{0};
    StoredInspection inspection;
};
// Blocking repository: callers must run on a worker. Connections never cross threads.
class MySqlRepository {
public:
    explicit MySqlRepository(DatabaseSettings settings = DatabaseSettings::fromEnvironment());
    void changeApplicationPassword(const QString& oldPassword, const QString& newPassword) const;
    bool hasApplicationAccounts() const;
    bool hasAdministratorAccount() const;
    QString authenticateAccount(const QString& username, const QString& password) const;
    void createApplicationAccount(const QString& username, const QString& password, const QString& role, bool bootstrap = false) const;
    QList<ApplicationAccount> applicationAccounts() const;
    void deleteApplicationAccount(const QString& username) const;
    void checkConnection() const;
    QString testConnection() const;
    QJsonObject loadRegistry() const;
    void saveRegistry(const QJsonObject& registry) const;
    DiscoveredCameraItem loadDevice(const QString& cameraId) const;
    DiscoveredCameraItem saveCalibration(const QString& cameraId, const QJsonObject& calibration) const;
    QList<GaugeTemplateRecord> loadTemplates() const;
    GaugeTemplateRecord loadTemplate(const QString& id) const;
    GaugeTemplateRecord saveTemplate(const QJsonObject& definition, quint64 expectedRevision = 0) const;
    void deleteTemplate(const QString& id, quint64 expectedRevision) const;
    int importTemplates(const QList<QJsonObject>& definitions) const;
    void saveInspection(const QJsonObject& envelope) const;
    QList<StoredInspection> history(const QString& cameraId, const QDateTime& from,
                                  const QDateTime& until, int page, int pageSize = 50) const;
    StoredImage original(const QString& taskId) const;
    HistoryDeletion deleteHistory(const QStringList& taskIds) const;
    QStringList historyTaskIds(const QString& cameraId, const QDateTime& from, const QDateTime& until) const;
    void addReview(const QString& taskId, const QString& reviewer, const QString& decision,
                   const QString& correctedValue, const QString& comment) const;
    static QJsonObject inspectionEnvelope(const InspectionTask&, const InspectionOutcome&);
    static QString queueInspection(const QJsonObject& envelope, const QString& directory);
    static QString queueRegistry(const QJsonObject& registry);
    static QJsonObject sanitizedRegistry(QJsonObject registry);
    static QString retryPending(const QString& evidenceDirectory, int limit = 20);
    static QString registryQueueDirectory();
    QString maintainEvidence(const QString& directory) const;
private:
    DatabaseSettings m_settings;
};
