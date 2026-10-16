#include "MySqlRepository.hpp"
#include "DeviceCodec.hpp"
#include "DatabaseSettingsStore.hpp"
#include "MySqlErrorText.hpp"
#include "MySqlBootstrapSchema.hpp"
#include "MySqlTemplateSchema.hpp"
#include "GaugeTemplateStore.hpp"
#include "GaugeProfile.hpp"
#include "DeviceCalibration.hpp"
#include "EvidenceArchive.hpp"
#include <mysql.h>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonArray>
#include <QFile>
#include <QSaveFile>
#include <QDir>
#include <QStandardPaths>
#include <QUuid>
#include <QUrl>
#include <QSet>
#include <QFileInfo>
#include <QBuffer>
#include <QImageReader>
#include <QLockFile>
#include <optional>
#include <vector>
#include <memory>
#include <stdexcept>
#include <cmath>

namespace {
using Cell = std::optional<QByteArray>;
using Row = std::vector<Cell>;
using Rows = std::vector<Row>;
[[noreturn]] void fail(const QString& message) { throw std::runtime_error(message.toUtf8().constData()); }
QByteArray json(const QJsonObject& value) { return QJsonDocument(value).toJson(QJsonDocument::Compact); }
QByteArray number(qulonglong n) { return QByteArray::number(n); }
QByteArray hash(const QByteArray& data) { return QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex(); }
QByteArray time(const QDateTime& t) { return t.toUTC().toString("yyyy-MM-dd HH:mm:ss.zzz").toUtf8(); }
Cell nullable(const QString& s) { return s.isEmpty() ? Cell{} : Cell{s.toUtf8()}; }
QString str(const Cell& c) { return c ? QString::fromUtf8(*c) : QString(); }
QDateTime date(const Cell& c) {
    auto result = QDateTime::fromString(str(c), "yyyy-MM-dd HH:mm:ss.zzz");
    if (!result.isValid()) result = QDateTime::fromString(str(c), "yyyy-MM-dd HH:mm:ss");
    result.setTimeSpec(Qt::UTC); return result;
}
struct Library {
    Library() { if (mysql_library_init(0, nullptr, nullptr)) fail("MYSQL_LIBRARY_INIT_FAILED"); }
    ~Library() { mysql_library_end(); }
};
class Connection {
public:
    MYSQL* handle{nullptr};
    int maxBytes;
    explicit Connection(const DatabaseSettings& settings) : maxBytes(settings.maxImageBytes) {
        static Library library;
        if (!settings.enabled || settings.database.isEmpty() || settings.user.isEmpty() || settings.port == 0) fail("MYSQL_NOT_CONFIGURED");
        if (mysql_thread_init()) fail("MYSQL_THREAD_INIT_FAILED");
        handle = mysql_init(nullptr);
        if (!handle) { mysql_thread_end(); fail("MYSQL_INIT_FAILED"); }
        unsigned int timeout = 3, readTimeout = 15;
        mysql_options(handle, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
        mysql_options(handle, MYSQL_OPT_READ_TIMEOUT, &readTimeout);
        mysql_options(handle, MYSQL_OPT_WRITE_TIMEOUT, &readTimeout);
        mysql_options(handle, MYSQL_SET_CHARSET_NAME, "utf8mb4");
        mysql_ssl_mode mode = SSL_MODE_REQUIRED;
        if (settings.tlsMode == "disabled") mode = SSL_MODE_DISABLED;
        else if (settings.tlsMode == "verify_identity") mode = SSL_MODE_VERIFY_IDENTITY;
        else if (settings.tlsMode != "required") {
            mysql_close(handle); handle = nullptr; mysql_thread_end(); fail("INVALID_MYSQL_TLS_MODE");
        }
        mysql_options(handle, MYSQL_OPT_SSL_MODE, &mode);
        if (!settings.caFile.isEmpty()) mysql_options(handle, MYSQL_OPT_SSL_CA, settings.caFile.toUtf8().constData());
        const auto connect = [&](const char* database) {
            return mysql_real_connect(handle, settings.host.toUtf8().constData(), settings.user.toUtf8().constData(),
                settings.password.toUtf8().constData(), database, settings.port, nullptr, 0);
        };
        try {
            bool initialize = false;
            if (!connect(settings.database.toUtf8().constData())) {
                if (mysql_errno(handle) != 1049 || !connect(nullptr))
                    fail(QString("MYSQL_CONNECT_FAILED (%1): %2").arg(mysql_errno(handle))
                        .arg(decodeMySqlError(QByteArray(mysql_error(handle)))));
                auto name = settings.database.toUtf8(); name.replace("`", "``");
                const auto sql = "CREATE DATABASE IF NOT EXISTS `" + name + "` CHARACTER SET utf8mb4";
                if (mysql_real_query(handle, sql.constData(), static_cast<unsigned long>(sql.size())) ||
                    mysql_select_db(handle, settings.database.toUtf8().constData()))
                    fail(QString("MYSQL_DATABASE_CREATE_FAILED (%1): %2").arg(mysql_errno(handle))
                        .arg(decodeMySqlError(QByteArray(mysql_error(handle)))));
                initialize = true;
            }
            // Resume interrupted initialization only in databases with no runtime marker.
            if (!initialize) {
                if (mysql_query(handle, "SELECT 1 FROM runtime_settings LIMIT 0")) {
                    if (mysql_errno(handle) == 1146) initialize = true;
                    else fail(QString("MYSQL_SCHEMA_CHECK_FAILED: %1").arg(decodeMySqlError(QByteArray(mysql_error(handle)))));
                } else if (auto* result = mysql_store_result(handle)) mysql_free_result(result);
            }
            if (initialize) {
                for (const auto& statement : QByteArray(MySqlBootstrapSchema).split(';')) {
                    const auto sql = statement.trimmed();
                    if (!sql.isEmpty() && mysql_real_query(handle, sql.constData(), static_cast<unsigned long>(sql.size())))
                        fail(QString("MYSQL_SCHEMA_INIT_FAILED (%1): %2").arg(mysql_errno(handle))
                            .arg(decodeMySqlError(QByteArray(mysql_error(handle)))));
                }
            }
            if (mysql_query(handle, "SELECT 1 FROM gauge_templates LIMIT 0")) {
                if (mysql_errno(handle) != 1146)
                    fail(QString("MYSQL_TEMPLATE_SCHEMA_CHECK_FAILED: %1").arg(decodeMySqlError(QByteArray(mysql_error(handle)))));
                for (const auto& statement : QByteArray(MySqlTemplateSchema).split(';')) {
                    const auto sql = statement.trimmed();
                    if (!sql.isEmpty() && mysql_real_query(handle, sql.constData(), static_cast<unsigned long>(sql.size())))
                        fail(QString("MYSQL_TEMPLATE_SCHEMA_CREATE_FAILED: %1").arg(decodeMySqlError(QByteArray(mysql_error(handle)))));
                }
            } else if (auto* result = mysql_store_result(handle)) mysql_free_result(result);
            if (mysql_query(handle, "SELECT result_snapshot FROM inspection_history LIMIT 0")) {
                if (mysql_errno(handle)!=1054) fail("MYSQL_EVIDENCE_SCHEMA_CHECK_FAILED");
                if (mysql_query(handle, "ALTER TABLE inspection_history ADD COLUMN result_snapshot JSON NULL") && mysql_errno(handle)!=1060)
                    fail("MYSQL_EVIDENCE_SCHEMA_UPGRADE_FAILED: ALTER permission required");
            } else if (auto* result=mysql_store_result(handle)) mysql_free_result(result);
            if (mysql_query(handle, "SET time_zone = '+00:00'")) fail("MYSQL_SESSION_SETUP_FAILED");
        }
        catch (...) { mysql_close(handle); handle = nullptr; mysql_thread_end(); throw; }
    }
    ~Connection() { if (handle) { mysql_close(handle); mysql_thread_end(); } }
    Rows exec(const QByteArray& sql, const Row& values = {}, int binaryIndex = -1) {
        std::unique_ptr<MYSQL_STMT, decltype(&mysql_stmt_close)> stmt(mysql_stmt_init(handle), mysql_stmt_close);
        if (!stmt) fail("MYSQL_STATEMENT_INIT_FAILED");
        auto check = [&](int code) {
            if (code) fail(QString("MYSQL_QUERY_FAILED (%1): %2").arg(mysql_stmt_errno(stmt.get()))
                .arg(decodeMySqlError(QByteArray(mysql_stmt_error(stmt.get())))));
        };
        check(mysql_stmt_prepare(stmt.get(), sql.constData(), static_cast<unsigned long>(sql.size())));
        std::vector<MYSQL_BIND> params(values.size());
        std::vector<unsigned long> lengths(values.size());
        std::unique_ptr<bool[]> nulls(new bool[values.size()]{});
        for (size_t i = 0; i < values.size(); ++i) {
            nulls[i] = !values[i];
            lengths[i] = values[i] ? static_cast<unsigned long>(values[i]->size()) : 0;
            params[i].buffer_type = static_cast<int>(i) == binaryIndex ? MYSQL_TYPE_BLOB : MYSQL_TYPE_STRING;
            params[i].buffer = values[i] ? const_cast<char*>(values[i]->constData()) : nullptr;
            params[i].buffer_length = lengths[i]; params[i].length = &lengths[i]; params[i].is_null = &nulls[i];
        }
        if (!values.empty()) check(mysql_stmt_bind_param(stmt.get(), params.data()));
        bool updateLengths = true;
        check(mysql_stmt_attr_set(stmt.get(), STMT_ATTR_UPDATE_MAX_LENGTH, &updateLengths));
        check(mysql_stmt_execute(stmt.get()));
        if (!mysql_stmt_field_count(stmt.get())) return {};
        check(mysql_stmt_store_result(stmt.get()));
        std::unique_ptr<MYSQL_RES, decltype(&mysql_free_result)> meta(mysql_stmt_result_metadata(stmt.get()), mysql_free_result);
        const unsigned count = mysql_num_fields(meta.get());
        MYSQL_FIELD* fields = mysql_fetch_fields(meta.get());
        std::vector<QByteArray> buffers(count);
        std::vector<MYSQL_BIND> outputs(count);
        std::vector<unsigned long> sizes(count);
        std::unique_ptr<bool[]> outputNulls(new bool[count]{}), truncated(new bool[count]{});
        for (unsigned i = 0; i < count; ++i) {
            if (fields[i].max_length > static_cast<unsigned long>(maxBytes)) fail("MYSQL_RESULT_TOO_LARGE");
            buffers[i].resize(static_cast<int>(fields[i].max_length) + 1);
            outputs[i].buffer_type = MYSQL_TYPE_BLOB; outputs[i].buffer = buffers[i].data();
            outputs[i].buffer_length = static_cast<unsigned long>(buffers[i].size());
            outputs[i].length = &sizes[i]; outputs[i].is_null = &outputNulls[i]; outputs[i].error = &truncated[i];
        }
        check(mysql_stmt_bind_result(stmt.get(), outputs.data()));
        Rows rows;
        for (;;) {
            int status = mysql_stmt_fetch(stmt.get());
            if (status == MYSQL_NO_DATA) break;
            check(status);
            Row row;
            for (unsigned i = 0; i < count; ++i)
                row.push_back(outputNulls[i] ? Cell{} : Cell{buffers[i].left(static_cast<int>(sizes[i]))});
            rows.push_back(std::move(row));
        }
        return rows;
    }
};
class Transaction {
    Connection& c;
    bool committed{false};
public:
    explicit Transaction(Connection& connection) : c(connection) {
        if (mysql_autocommit(c.handle, false)) fail("MYSQL_TRANSACTION_BEGIN_FAILED");
    }
    ~Transaction() { if (!committed) mysql_rollback(c.handle); }
    void commit() {
        if (mysql_commit(c.handle)) fail("MYSQL_TRANSACTION_COMMIT_FAILED");
        committed = true;
    }
};
QJsonObject cleanDevice(QJsonObject d) {
    d.remove("password");
    d.remove("is_auto_connect");
    QUrl url(d["rtsp_url"].toString()); url.setUserName(QString()); url.setPassword(QString());
    d["rtsp_url"] = url.toString(); return d;
}
void saveCamera(Connection& c, const QJsonObject& raw, bool update) {
    auto d = cleanDevice(raw);
    if (d["camera_id"].toString().isEmpty()) fail("CAMERA_ID_REQUIRED");
    QByteArray statement = "INSERT INTO camera_archives (camera_id, display_name, protocol, endpoint, credential_ref,"
        " substation, area, installation_location, device_tag, gauge_type, manufacturer, accuracy_class,"
        " enabled, created_at, updated_at, archive_json) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,1,?,?,?)";
    if (update) statement += " ON DUPLICATE KEY UPDATE display_name=VALUES(display_name), protocol=VALUES(protocol),"
        " endpoint=VALUES(endpoint), credential_ref=VALUES(credential_ref), substation=VALUES(substation),"
        " area=VALUES(area), installation_location=VALUES(installation_location), device_tag=VALUES(device_tag),"
        " gauge_type=VALUES(gauge_type), manufacturer=VALUES(manufacturer), accuracy_class=VALUES(accuracy_class),"
        " archive_json=VALUES(archive_json), enabled=1, deleted_at=NULL, updated_at=VALUES(updated_at)";
    else statement += " ON DUPLICATE KEY UPDATE camera_id=camera_id";
    auto now = time(QDateTime::currentDateTimeUtc());
    c.exec(statement, {d["camera_id"].toString().toUtf8(), d["custom_name"].toString().toUtf8(),
        d["protocol_type"].toString().toUtf8(), (d["rtsp_url"].toString().isEmpty() ? d["ip_or_sn"].toString() : d["rtsp_url"].toString()).toUtf8(),
        ("local-camera:" + d["camera_id"].toString()).toUtf8(), nullable(d["substation"].toString()),
        nullable(d["area"].toString()), nullable(d["installation_location"].toString()),
        nullable(d["device_tag"].toString()), nullable(d["gauge_type"].toString()),
        nullable(d["manufacturer"].toString()), nullable(d["accuracy_class"].toString()), now, now, json(d)});
}
StoredInspection toInspection(const Row& r) {
    StoredInspection out;
    out.taskId = str(r[0]); out.cameraId = str(r[1]); out.parentTaskId = str(r[2]);
    out.capturedAt = date(r[3]); out.processedAt = date(r[4]); out.value = str(r[5]).toDouble();
    out.unit = str(r[6]); out.confidence = str(r[7]).toDouble(); out.success = str(r[8]) == "1";
    out.recognitionPerformed = str(r[9]) == "1"; out.errorCode = str(r[10]); out.errorMessage = str(r[11]);
    out.inputKind = str(r[12]); out.qualityStatus = str(r[13]); out.processStatus = str(r[14]);
    out.device = deviceFromJson(QJsonDocument::fromJson(r[15].value_or(QByteArray())).object());
    out.profile = QJsonDocument::fromJson(r[16].value_or(QByteArray())).object();
    out.hasOriginal = str(r[17]) == "1";
    out.latestReview = QJsonDocument::fromJson(r[18].value_or(QByteArray())).object();
    out.resultSnapshot = QJsonDocument::fromJson(r[19].value_or(QByteArray())).object();
    return out;
}
const QByteArray historySelect = "SELECT h.task_id,h.camera_id,h.parent_task_id,h.captured_at,h.processed_at,"
    "h.reading_value,h.unit,h.confidence,h.recognition_success,h.recognition_performed,h.error_code,h.error_message,"
    "h.input_kind,h.quality_status,h.process_status,v.archive_snapshot,v.recognition_profile,"
    "EXISTS(SELECT 1 FROM inspection_evidence e WHERE e.task_id=h.task_id AND e.evidence_kind='original'"
    " AND e.sequence_number=0 AND e.storage_backend='mysql' AND e.availability='available'),"
    "(SELECT JSON_OBJECT('decision',r.decision,'reviewer',r.reviewer_id,'value',r.corrected_value,'comment',r.comment,'time',r.reviewed_at)"
    " FROM reading_reviews r WHERE r.task_id=h.task_id ORDER BY r.reviewed_at DESC,r.review_id DESC LIMIT 1),h.result_snapshot"
    " FROM inspection_history h JOIN camera_config_versions v ON v.config_id=h.config_id";
void writeAtomic(const QString& path, const QJsonObject& object) {
    QSaveFile file(path); const auto bytes = json(object);
    const auto describe = [&](const QString& operation) {
        fail(QString("OUTBOX_WRITE_FAILED | %1 | 文件: %2 | 原因: %3")
            .arg(operation, QFileInfo(path).absoluteFilePath(), file.errorString()));
    };
    if (!file.open(QIODevice::WriteOnly)) describe(QStringLiteral(u"\u6253\u5f00"));
    if (file.write(bytes) != bytes.size()) { describe(QStringLiteral(u"\u5199\u5165")); }
    if (!file.commit()) describe(QStringLiteral(u"\u539f\u5b50\u63d0\u4ea4"));
}
}

DatabaseSettings DatabaseSettings::fromEnvironment() {
    // Saved settings are frozen for this process. Saving in the GUI takes effect on restart.
    if (qEnvironmentVariable("GAUGE_MYSQL_DATABASE").isEmpty()) {
        static const auto saved = DatabaseSettingsStore::load();
        return saved;
    }
    DatabaseSettings s;
    s.database = qEnvironmentVariable("GAUGE_MYSQL_DATABASE"); s.enabled = !s.database.isEmpty();
    s.host = qEnvironmentVariable("GAUGE_MYSQL_HOST", "127.0.0.1");
    s.user = qEnvironmentVariable("GAUGE_MYSQL_USER"); s.password = qEnvironmentVariable("GAUGE_MYSQL_PASSWORD");
    bool ok = false; auto port = qEnvironmentVariable("GAUGE_MYSQL_PORT", "3306").toUInt(&ok);
    s.port = ok && port > 0 && port <= 65535 ? port : 0;
    s.tlsMode = qEnvironmentVariable("GAUGE_MYSQL_TLS_MODE", "required");
    s.caFile = qEnvironmentVariable("GAUGE_MYSQL_CA_FILE"); return s;
}
MySqlRepository::MySqlRepository(DatabaseSettings settings) : m_settings(std::move(settings)) {}
void MySqlRepository::checkConnection() const { Connection c(m_settings); c.exec("SELECT 1 FROM runtime_settings LIMIT 1"); }
QString MySqlRepository::testConnection() const {
    Connection c(m_settings);
    const auto result=c.exec("SELECT COUNT(*) FROM information_schema.tables WHERE table_schema=?"
        " AND table_name IN ('camera_archives','camera_config_versions','inspection_history','inspection_evidence',"
        "'reading_reviews','image_blobs','runtime_settings','gauge_templates')", {m_settings.database.toUtf8()});
    return str(result[0][0]).toInt()==8 ? QStringLiteral(u"\u8fde\u63a5\u6210\u529f\uff0c\u6570\u636e\u5e93\u6240\u9700\u8868\u5df2\u5b58\u5728\u3002")
        : QStringLiteral(u"\u8fde\u63a5\u6210\u529f\uff0c\u4f46\u6570\u636e\u5e93\u8868\u7ed3\u6784\u5c1a\u672a\u51c6\u5907\u9f50\u5168\u3002\u8bf7\u5148\u6267\u884c database/mysql \u4e2d\u7684\u8fc1\u79fb\u811a\u672c\uff08001\u2013004\uff09\u3002");
}
QJsonObject MySqlRepository::sanitizedRegistry(QJsonObject registry) {
    QJsonArray devices;
    for (const auto& value : registry["devices"].toArray()) devices.append(cleanDevice(value.toObject()));
    registry["devices"] = devices;
    auto system=registry["system"].toObject(); system.remove("auto_connect_on_startup"); system.remove("last_active_device_id");
    registry["system"]=system; return registry;
}
QJsonObject MySqlRepository::loadRegistry() const {
    Connection c(m_settings); QJsonArray devices;
    for (const auto& row : c.exec("SELECT archive_json FROM camera_archives WHERE deleted_at IS NULL ORDER BY camera_id"))
        devices.append(QJsonDocument::fromJson(row[0].value_or(QByteArray())).object());
    QJsonObject result; result["devices"] = devices;
    auto sys = c.exec("SELECT setting_json FROM runtime_settings WHERE setting_key='system'");
    if (!sys.empty()) result["system"] = QJsonDocument::fromJson(sys[0][0].value_or(QByteArray())).object();
    return result;
}

DiscoveredCameraItem MySqlRepository::loadDevice(const QString& cameraId) const {
    Connection c(m_settings);
    const auto rows = c.exec("SELECT archive_json FROM camera_archives WHERE camera_id=? AND deleted_at IS NULL", {cameraId.toUtf8()});
    if (rows.empty()) fail("CAMERA_NOT_FOUND");
    return deviceFromJson(QJsonDocument::fromJson(rows.front()[0].value_or(QByteArray())).object());
}

DiscoveredCameraItem MySqlRepository::saveCalibration(const QString& cameraId, const QJsonObject& calibration) const {
    if (!validDeviceCalibration(calibration)) fail("INVALID_CAMERA_CALIBRATION");
    Connection c(m_settings); Transaction tx(c);
    const auto rows = c.exec("SELECT archive_json FROM camera_archives WHERE camera_id=? AND deleted_at IS NULL FOR UPDATE", {cameraId.toUtf8()});
    if (rows.empty()) fail("CAMERA_NOT_FOUND");
    auto archive = QJsonDocument::fromJson(rows.front()[0].value_or(QByteArray())).object();
    auto confirmed = calibration; confirmed["calibration_confirmed"] = true;
    archive["calibration"] = confirmed;
    c.exec("UPDATE camera_archives SET archive_json=?,updated_at=? WHERE camera_id=?",
           {json(archive), time(QDateTime::currentDateTimeUtc()), cameraId.toUtf8()});
    tx.commit();
    return deviceFromJson(archive);
}
void MySqlRepository::saveRegistry(const QJsonObject& registry) const {
    Connection c(m_settings); Transaction tx(c); QSet<QString> ids;
    // Always lock templates before archives, matching deleteTemplate's order.
    QStringList templates;
    for (const auto& value : registry["devices"].toArray()) {
        auto id = value.toObject()["default_profile_id"].toString();
        if (id.isEmpty()) id = "PRESSURE_GAUGE_1.6MPA";
        if (!templates.contains(id)) templates.append(id);
    }
    templates.sort();
    for (const auto& id : templates)
        if (c.exec("SELECT template_id FROM gauge_templates WHERE template_id=? AND deleted_at IS NULL FOR UPDATE", {id.toUtf8()}).empty())
            fail(QStringLiteral(u"\u8bbe\u5907\u9009\u62e9\u7684\u6570\u636e\u5e93\u6a21\u677f\u4e0d\u5b58\u5728\uff1a%1").arg(id));
    // This desktop registry is a complete snapshot; only its serialized writer submits it.
    c.exec("SELECT camera_id FROM camera_archives FOR UPDATE");
    for (const auto& value : registry["devices"].toArray()) {
        auto device = value.toObject(); ids.insert(device["camera_id"].toString()); saveCamera(c, device, true);
    }
    for (const auto& row : c.exec("SELECT camera_id FROM camera_archives WHERE deleted_at IS NULL"))
        if (!ids.contains(str(row[0]))) c.exec("UPDATE camera_archives SET enabled=0,deleted_at=?,updated_at=? WHERE camera_id=?",
            {time(QDateTime::currentDateTimeUtc()), time(QDateTime::currentDateTimeUtc()), row[0]});
    c.exec("INSERT INTO runtime_settings(setting_key,setting_json) VALUES('system',?)"
        " ON DUPLICATE KEY UPDATE setting_json=VALUES(setting_json)", {json(sanitizedRegistry(registry)["system"].toObject())});
    tx.commit();
}

namespace {
GaugeTemplateRecord templateRecord(const Row& row) {
    GaugeTemplateRecord record;
    record.id = str(row[0]);
    record.definition = QJsonDocument::fromJson(row[1].value_or(QByteArray())).object();
    record.definition.remove("min_angle_deg"); record.definition.remove("max_angle_deg");
    record.revision = str(row[2]).toULongLong();
    if (record.definition.isEmpty() || record.definition["gauge_id"].toString() != record.id || !record.revision)
        fail("DATABASE_TEMPLATE_INVALID");
    return record;
}
void validateTemplate(const QJsonObject& definition) {
    const auto error = GaugeTemplateStore::validate(definition);
    if (!error.isEmpty()) fail(error);
    GaugeProfile profile;
    if (!profile.loadFromJson(definition)) fail("DATABASE_TEMPLATE_INVALID");
}
const QByteArray templateReferences = "SELECT camera_id FROM camera_archives WHERE deleted_at IS NULL AND "
    "CASE WHEN JSON_TYPE(JSON_EXTRACT(archive_json,'$.default_profile_id'))='STRING' AND "
    "JSON_UNQUOTE(JSON_EXTRACT(archive_json,'$.default_profile_id'))<>'' "
    "THEN JSON_UNQUOTE(JSON_EXTRACT(archive_json,'$.default_profile_id')) ELSE 'PRESSURE_GAUGE_1.6MPA' END=?";
}

QList<GaugeTemplateRecord> MySqlRepository::loadTemplates() const {
    Connection c(m_settings);
    QList<GaugeTemplateRecord> records;
    for (const auto& row : c.exec("SELECT template_id,definition,revision FROM gauge_templates WHERE deleted_at IS NULL ORDER BY template_id")) {
        auto record = templateRecord(row);
        validateTemplate(record.definition);
        records.append(record);
    }
    // One registry read provides usage labels; deletion rechecks under locks.
    QMap<QString, QStringList> usage;
    for (const auto& row : c.exec("SELECT camera_id,archive_json FROM camera_archives WHERE deleted_at IS NULL")) {
        auto id = QJsonDocument::fromJson(row[1].value_or(QByteArray())).object()["default_profile_id"].toString();
        if (id.isEmpty()) id = "PRESSURE_GAUGE_1.6MPA";
        usage[id].append(str(row[0]));
    }
    for (auto& record : records) record.usedBy = usage.value(record.id);
    return records;
}

GaugeTemplateRecord MySqlRepository::loadTemplate(const QString& id) const {
    Connection c(m_settings);
    const auto rows = c.exec("SELECT template_id,definition,revision FROM gauge_templates WHERE template_id=? AND deleted_at IS NULL", {id.toUtf8()});
    if (rows.empty()) fail(QStringLiteral(u"\u6570\u636e\u5e93\u6a21\u677f\u4e0d\u5b58\u5728\u6216\u5df2\u5220\u9664\uff1a%1").arg(id));
    auto record = templateRecord(rows.front());
    validateTemplate(record.definition);
    return record;
}

GaugeTemplateRecord MySqlRepository::saveTemplate(const QJsonObject& input, quint64 expectedRevision) const {
    auto definition=input; definition.remove("min_angle_deg"); definition.remove("max_angle_deg");
    validateTemplate(definition);
    const auto id = definition["gauge_id"].toString();
    Connection c(m_settings); Transaction tx(c);
    const auto rows = c.exec("SELECT revision,deleted_at FROM gauge_templates WHERE template_id=? FOR UPDATE", {id.toUtf8()});
    if (!expectedRevision && !rows.empty()) fail(QStringLiteral(u"\u6a21\u677f\u7f16\u53f7\u5df2\u5b58\u5728\uff08\u542b\u5df2\u5220\u9664\u6a21\u677f\uff09\uff0c\u8bf7\u4f7f\u7528\u4e0d\u540c\u7f16\u53f7"));
    if (expectedRevision && (rows.empty() || rows.front()[1] || str(rows.front()[0]).toULongLong() != expectedRevision))
        fail(QStringLiteral(u"\u6a21\u677f\u5df2\u88ab\u5176\u4ed6\u64cd\u4f5c\u4fee\u6539\u6216\u5220\u9664\uff0c\u8bf7\u5237\u65b0\u5217\u8868\u540e\u91cd\u8bd5"));
    const auto now = time(QDateTime::currentDateTimeUtc());
    const auto revision = expectedRevision + 1;
    if (!expectedRevision) c.exec("INSERT INTO gauge_templates(template_id,definition,revision,created_at,updated_at) VALUES(?,?,1,?,?)",
                                 {id.toUtf8(), json(definition), now, now});
    else c.exec("UPDATE gauge_templates SET definition=?,revision=?,updated_at=? WHERE template_id=?",
                {json(definition), number(revision), now, id.toUtf8()});
    tx.commit();
    return {id, definition, revision, {}};
}

void MySqlRepository::deleteTemplate(const QString& id, quint64 expectedRevision) const {
    Connection c(m_settings); Transaction tx(c);
    const auto rows = c.exec("SELECT revision,deleted_at FROM gauge_templates WHERE template_id=? FOR UPDATE", {id.toUtf8()});
    if (!expectedRevision || rows.empty() || rows.front()[1] || str(rows.front()[0]).toULongLong() != expectedRevision)
        fail(QStringLiteral(u"\u6a21\u677f\u5df2\u88ab\u5176\u4ed6\u64cd\u4f5c\u4fee\u6539\u6216\u5220\u9664\uff0c\u8bf7\u5237\u65b0\u5217\u8868\u540e\u91cd\u8bd5"));
    const auto references = c.exec(templateReferences + " FOR UPDATE", {id.toUtf8()});
    if (!references.empty()) {
        QStringList devices;
        for (const auto& row : references) devices.append(str(row[0]));
        fail(QStringLiteral(u"\u6a21\u677f\u6b63\u5728\u88ab\u8bbe\u5907\u4f7f\u7528\uff0c\u8bf7\u5148\u66f4\u6362\u8bbe\u5907\u6a21\u677f\uff1a%1").arg(devices.join("、")));
    }
    const auto now = time(QDateTime::currentDateTimeUtc());
    c.exec("UPDATE gauge_templates SET deleted_at=?,updated_at=?,revision=revision+1 WHERE template_id=?", {now, now, id.toUtf8()});
    tx.commit();
}

int MySqlRepository::importTemplates(const QList<QJsonObject>& definitions) const {
    QMap<QString, QJsonObject> unique;
    for (const auto& definition : definitions) {
        validateTemplate(definition);
        const auto id = definition["gauge_id"].toString();
        if (unique.contains(id)) fail(QStringLiteral(u"\u8fc1\u79fb\u6587\u4ef6\u4e2d\u7684\u6a21\u677f\u7f16\u53f7\u91cd\u590d\uff1a%1").arg(id));
        auto cleaned=definition; cleaned.remove("min_angle_deg"); cleaned.remove("max_angle_deg");
        unique[id] = cleaned;
    }
    Connection c(m_settings); Transaction tx(c);
    int imported = 0;
    const auto now = time(QDateTime::currentDateTimeUtc());
    for (auto it = unique.begin(); it != unique.end(); ++it) {
        // Repeated migration never overwrites database edits or resurrects deletes.
        if (!c.exec("SELECT template_id FROM gauge_templates WHERE template_id=? FOR UPDATE", {it.key().toUtf8()}).empty()) continue;
        c.exec("INSERT INTO gauge_templates(template_id,definition,revision,created_at,updated_at) VALUES(?,?,1,?,?)",
               {it.key().toUtf8(), json(it.value()), now, now});
        ++imported;
    }
    tx.commit(); return imported;
}
QJsonObject MySqlRepository::inspectionEnvelope(const InspectionTask& task, const InspectionOutcome& out) {
    QJsonObject record;
    record["task_id"] = out.taskId; record["camera_id"] = out.cameraId;
    record["device"] = out.deviceSnapshot.isEmpty() ? deviceToJson(task.device) : out.deviceSnapshot;
    record["profile"] = out.profileSnapshot;
    // A failed historical identity check must not create a false cross-camera link.
    record["parent_task_id"] = out.capturedFrame.empty() ? QString() : task.parentTaskId;
    record["input_kind"] = inspectionInputName(task.inputKind); record["mode"] = gaugeRunModeName(task.runMode);
    record["captured_at"] = out.capturedAt.isValid() ? out.capturedAt.toUTC().toString(Qt::ISODateWithMs) : QString();
    record["processed_at"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    record["recognition_performed"] = out.recognitionPerformed;
    record["success"] = out.errorCode.isEmpty() && out.result.success;
    record["value"] = out.result.success ? QJsonValue(out.result.value) : QJsonValue(QJsonValue::Null);
    record["unit"] = QString::fromStdString(out.result.unit); record["confidence"] = out.result.confidence;
    record["result"] = QJsonObject{{"archive",out.archiveMetadata},{"pointer_angle_deg",out.result.pointer_angle_deg},
        {"recognition_quality",out.result.recognitionQuality},{"recognition_timings_ms",out.result.stageTimings},
        {"capture_method",out.captureMethod},{"center_inconsistent",out.result.centerInconsistent},
        {"center_offset_px",out.result.centerOffsetDist},{"warning",QString::fromStdString(out.result.warningMessage)},
        {"diagnostics",QString::fromStdString(out.result.detailedDebugInfo)}};
    record["error_code"] = out.errorCode;
    record["recognition_quality"] = out.result.recognitionQuality;
    auto resultSnapshot = record["result"].toObject();
    resultSnapshot["safety_assessment"] = out.safetyAssessment;
    record["result"] = resultSnapshot;
    record["error_message"] = out.errorMessage;
    record["original_path"] = QFileInfo(out.originalPath).absoluteFilePath();
    if (out.originalPath.isEmpty()) record["original_path"] = QString();
    record["rectified_path"] = out.rectifiedPath.isEmpty() ? QString() : QFileInfo(out.rectifiedPath).absoluteFilePath();
    record["recognition_path"] = out.recognitionPath.isEmpty() ? QString() : QFileInfo(out.recognitionPath).absoluteFilePath();
    record["reuse_original_task_id"] = out.capturedFrame.empty() ? QString() : task.databaseOriginalTaskId;
    return record;
}
void MySqlRepository::saveInspection(const QJsonObject& r) const {
    Connection c(m_settings); Transaction tx(c);
    const auto id = r["task_id"].toString().toUtf8(); auto device = r["device"].toObject();
    const auto camera = r["camera_id"].toString().toUtf8();
    if (device["camera_id"].toString().toUtf8() != camera || QUuid(QString::fromUtf8(id)).isNull()) fail("INVALID_HISTORY_IDENTITY");
    if (!c.exec("SELECT task_id FROM inspection_history WHERE task_id=?", {id}).empty()) { tx.commit(); return; }
    saveCamera(c, device, false);
    c.exec("SELECT camera_id FROM camera_archives WHERE camera_id=? FOR UPDATE", {camera});
    // Another storage worker may have committed this task while we waited on the camera row.
    if (!c.exec("SELECT task_id FROM inspection_history WHERE task_id=? FOR UPDATE", {id}).empty()) { tx.commit(); return; }
    auto profile = r["profile"].toObject(); auto cleaned = cleanDevice(device);
    auto versionedDevice = cleaned;
    for (const auto& key : {"is_online", "last_seen_time", "is_auto_connect"}) versionedDevice.remove(key);
    auto snapshotHash = hash(json(versionedDevice) + '\n' + json(profile));
    auto versions = c.exec("SELECT config_id FROM camera_config_versions WHERE camera_id=? AND snapshot_sha256=?", {camera,snapshotHash});
    Cell config;
    if (versions.empty()) {
        auto next = c.exec("SELECT COALESCE(MAX(version_number),0)+1 FROM camera_config_versions WHERE camera_id=?", {camera});
        c.exec("INSERT INTO camera_config_versions(camera_id,version_number,calibration,recognition_profile,archive_snapshot,created_at,snapshot_sha256)"
            " VALUES(?,?,?,?,?,?,?)", {camera,next[0][0],json(cleaned["calibration"].toObject()),json(profile),json(cleaned),
             time(QDateTime::currentDateTimeUtc()),snapshotHash});
        config = number(mysql_insert_id(c.handle));
    } else config = versions[0][0];
    const auto acquired = QDateTime::fromString(r["captured_at"].toString(), Qt::ISODateWithMs);
    const auto processed = QDateTime::fromString(r["processed_at"].toString(), Qt::ISODateWithMs);
    if (!processed.isValid()) fail("INVALID_PROCESSING_TIME");
    const bool successful = r["success"].toBool();
    const QString quality = !r["recognition_performed"].toBool() ? "not_evaluated" :
        successful ? (r["confidence"].toDouble() < 0.4 ? "review_required" : "accepted") : "failed";
    c.exec("INSERT INTO inspection_history(task_id,camera_id,config_id,parent_task_id,input_kind,run_mode,captured_at,processed_at,"
        "recognition_performed,recognition_success,reading_value,unit,confidence,quality_status,process_status,error_code,error_message,result_snapshot)"
        " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)", {id,camera,config,nullable(r["parent_task_id"].toString()),
        r["input_kind"].toString().toUtf8(),r["mode"].toString().toUtf8(),acquired.isValid()?Cell{time(acquired)}:Cell{},time(processed),
        r["recognition_performed"].toBool()?QByteArray("1"):QByteArray("0"),successful?QByteArray("1"):QByteArray("0"),
        r["value"].isDouble()?Cell{QByteArray::number(r["value"].toDouble(),'g',17)}:Cell{},nullable(r["unit"].toString()),
        QByteArray::number(r["confidence"].toDouble(),'g',17),quality.toUtf8(),QByteArray("not_evaluated"),
        nullable(r["error_code"].toString()),nullable(r["error_message"].toString()),json(r["result"].toObject())});
    for (const auto& kind : {QString("original"),QString("rectified"),QString("recognition")}) {
        Cell imageId; QByteArray digest, byteSize;
        if (kind == "original" && !r["reuse_original_task_id"].toString().isEmpty()) {
            const auto parent = r["reuse_original_task_id"].toString();
            if (parent != r["parent_task_id"].toString()) fail("HISTORICAL_IDENTITY_MISMATCH");
            auto original = c.exec("SELECT e.image_id,e.sha256,e.byte_size,h.captured_at FROM inspection_evidence e"
                " JOIN inspection_history h ON h.task_id=e.task_id WHERE e.task_id=? AND h.camera_id=?"
                " AND e.evidence_kind='original' AND e.sequence_number=0 AND e.storage_backend='mysql' AND e.availability='available'",
                {parent.toUtf8(),camera});
            if (original.empty() || date(original[0][3]) != acquired) fail("HISTORICAL_IDENTITY_MISMATCH");
            imageId=original[0][0]; digest=original[0][1].value(); byteSize=original[0][2].value();
        } else {
            const auto path = r[kind+"_path"].toString(); if (path.isEmpty()) continue;
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly) || file.size() > m_settings.maxImageBytes) fail("EVIDENCE_UNAVAILABLE_OR_TOO_LARGE");
            auto bytes = file.readAll(); if (bytes.isEmpty()) fail("EMPTY_EVIDENCE");
            std::vector<uchar> buffer(bytes.begin(),bytes.end()); auto image = cv::imdecode(buffer,cv::IMREAD_UNCHANGED);
            if (image.empty()) fail("INVALID_EVIDENCE_IMAGE");
            digest=hash(bytes); byteSize=number(bytes.size());
            const auto mime=bytes.startsWith(QByteArray::fromHex("ffd8ff")) ? QByteArray("image/jpeg") : QByteArray("image/png");
            c.exec("INSERT INTO image_blobs(mime_type,pixel_width,pixel_height,sha256,byte_size,image_bytes,created_at)"
                " VALUES(?,?,?,?,?,?,?) ON DUPLICATE KEY UPDATE image_id=LAST_INSERT_ID(image_id)",
                {mime,number(image.cols),number(image.rows),digest,byteSize,bytes,time(QDateTime::currentDateTimeUtc())},5);
            imageId=number(mysql_insert_id(c.handle));
        }
        c.exec("INSERT INTO inspection_evidence(task_id,evidence_kind,sequence_number,storage_key,sha256,byte_size,availability,created_at,storage_backend,image_id)"
            " VALUES(?,?,0,NULL,?,?,'available',?,'mysql',?)", {id,kind.toUtf8(),digest,byteSize,time(processed),imageId});
    }
    tx.commit();
}
QList<StoredInspection> MySqlRepository::history(const QString& camera, const QDateTime& from,
        const QDateTime& until, int page, int size) const {
    if (page < 0 || page > 1000000 || size < 1 || size > 100 || !from.isValid() || !until.isValid() || from >= until)
        fail("INVALID_HISTORY_QUERY");
    Connection c(m_settings); Row params{time(from),time(until)};
    auto sql = historySelect + " WHERE h.processed_at>=? AND h.processed_at<?";
    if (!camera.trimmed().isEmpty()) { sql += " AND h.camera_id=?"; params.push_back(camera.trimmed().toUtf8()); }
    sql += " ORDER BY h.processed_at DESC,h.task_id DESC LIMIT " + QByteArray::number(size)
        + " OFFSET " + QByteArray::number(page * size);
    QList<StoredInspection> result;
    for (const auto& row : c.exec(sql,params)) result.append(toInspection(row));
    return result;
}
StoredImage MySqlRepository::original(const QString& taskId) const {
    Connection c(m_settings); Transaction tx(c);
    auto record = c.exec(historySelect + " WHERE h.task_id=?",{taskId.toUtf8()});
    if (record.empty()) fail("HISTORY_NOT_FOUND");
    StoredImage result; result.inspection=toInspection(record[0]);
    auto image = c.exec("SELECT b.image_bytes,b.sha256,b.pixel_width,b.pixel_height,b.byte_size,e.sha256,e.byte_size"
        " FROM inspection_evidence e JOIN image_blobs b ON b.image_id=e.image_id WHERE e.task_id=?"
        " AND e.evidence_kind='original' AND e.sequence_number=0 AND e.storage_backend='mysql' AND e.availability='available'",{taskId.toUtf8()});
    if (image.empty()) fail("ORIGINAL_NOT_AVAILABLE");
    result.bytes=image[0][0].value(); result.sha256=str(image[0][1]);
    result.width=str(image[0][2]).toInt(); result.height=str(image[0][3]).toInt();
    if (result.bytes.size()>m_settings.maxImageBytes || hash(result.bytes)!=image[0][1].value() ||
        str(image[0][4]).toLongLong()!=result.bytes.size() || image[0][1]!=image[0][5] || image[0][4]!=image[0][6])
        fail("ORIGINAL_INTEGRITY_FAILED");
    QBuffer buffer(&result.bytes); buffer.open(QIODevice::ReadOnly); QImageReader reader(&buffer);
    const auto dimensions = reader.size();
    if (dimensions.width()!=result.width || dimensions.height()!=result.height || result.width<=0 || result.height<=0 ||
        qint64(result.width)*result.height>50000000) fail("ORIGINAL_DIMENSIONS_INVALID");
    tx.commit(); return result;
}
void MySqlRepository::addReview(const QString& task, const QString& reviewer, const QString& decision,
        const QString& corrected, const QString& comment) const {
    bool valid=false; double value=corrected.toDouble(&valid);
    if (reviewer.trimmed().isEmpty() || reviewer.size()>128 || comment.size()>4096 ||
        (decision!="confirmed" && decision!="corrected" && decision!="unreadable") ||
        (decision=="corrected" && (!valid || !std::isfinite(value)))) fail("INVALID_REVIEW");
    Connection c(m_settings);
    c.exec("INSERT INTO reading_reviews(task_id,reviewer_id,decision,corrected_value,comment,reviewed_at) VALUES(?,?,?,?,?,?)",
        {task.toUtf8(),reviewer.trimmed().toUtf8(),decision.toUtf8(),decision=="corrected"?Cell{QByteArray::number(value,'g',17)}:Cell{},
         nullable(comment),time(QDateTime::currentDateTimeUtc())});
}
QString MySqlRepository::queueInspection(const QJsonObject& envelope,const QString& directory) {
    const auto folder=QDir(directory).filePath(".mysql_pending");
    if (!QDir().mkpath(folder)) fail("OUTBOX_DIRECTORY_FAILED");
    const auto id=envelope["task_id"].toString(); if (QUuid(id).isNull()) fail("INVALID_TASK_ID");
    const auto path=QDir(folder).filePath(envelope["processed_at"].toString().remove(':')+"_"+id+".json");
    writeAtomic(path,envelope); return path;
}
QString MySqlRepository::registryQueueDirectory() {
    return qEnvironmentVariable("GAUGE_MYSQL_OUTBOX",QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)+"/mysql_registry_outbox");
}
QString MySqlRepository::queueRegistry(const QJsonObject& registry) {
    const auto folder=QFileInfo(registryQueueDirectory()).absoluteFilePath();
    if (!QDir().mkpath(folder)) {
        QSaveFile probe(QDir(folder).filePath("latest.json")); probe.open(QIODevice::WriteOnly);
        fail(QString("OUTBOX_DIRECTORY_FAILED | 目录: %1 | 原因: %2").arg(folder, probe.errorString()));
    }
    const auto path=QDir(folder).filePath("latest.json");
    QLockFile lock(folder+"/registry.lock");
    if (!lock.tryLock(1000)) {
        const auto reason = lock.error() == QLockFile::LockFailedError ? QStringLiteral(u"\u540c\u6b65\u961f\u5217\u5df2\u88ab\u5176\u4ed6\u8fdb\u7a0b\u9501\u5b9a")
            : lock.error() == QLockFile::PermissionError ? QStringLiteral(u"\u6ca1\u6709\u521b\u5efa\u540c\u6b65\u961f\u5217\u9501\u6587\u4ef6\u7684\u6743\u9650")
            : QStringLiteral(u"\u540c\u6b65\u961f\u5217\u9501\u6587\u4ef6\u53d1\u751f I/O \u9519\u8bef");
        fail(QString("REGISTRY_OUTBOX_BUSY | 锁文件: %1 | 原因: %2 | 错误码: %3")
            .arg(folder+"/registry.lock", reason).arg(int(lock.error())));
    }
    writeAtomic(path,sanitizedRegistry(registry)); return path;
}
QString MySqlRepository::retryPending(const QString& directory,int limit) {
    try {
        if (!DatabaseSettings::fromEnvironment().enabled) return {};
        MySqlRepository repository;
        QString registryStatus;
        auto registryFolder=registryQueueDirectory(); QDir().mkpath(registryFolder);
        QLockFile registrySyncLock(registryFolder+"/sync.lock");
        if (registrySyncLock.tryLock(0)) {
            QFile file(QDir(registryFolder).filePath("latest.json"));
            QByteArray bytes;
            {
                QLockFile lock(registryFolder+"/registry.lock");
                if (!lock.tryLock(1000)) fail("REGISTRY_OUTBOX_BUSY");
                if (file.exists()) {
                    if (!file.open(QIODevice::ReadOnly)) fail("REGISTRY_OUTBOX_READ_FAILED");
                    bytes=file.readAll(); file.close();
                }
            }
            if (!bytes.isEmpty()) {
                auto doc=QJsonDocument::fromJson(bytes);
                if (!doc.isObject()) fail("INVALID_REGISTRY_OUTBOX");
                repository.saveRegistry(doc.object());
                QLockFile lock(registryFolder+"/registry.lock");
                if (!lock.tryLock(1000)) fail("REGISTRY_OUTBOX_BUSY");
                if (!file.open(QIODevice::ReadOnly)) fail("REGISTRY_OUTBOX_READ_FAILED");
                const auto latest=file.readAll(); file.close();
                if (latest==bytes) {
                    if (!file.remove()) fail("OUTBOX_REMOVE_FAILED");
                } else registryStatus="REGISTRY_NEWER_SNAPSHOT_PENDING";
            }
        } else if (QFile::exists(QDir(registryFolder).filePath("latest.json"))) registryStatus="REGISTRY_SYNC_IN_PROGRESS";
        if (directory.isEmpty()) return registryStatus;
        QDir folder(QDir(directory).filePath(".mysql_pending"));
        if (!folder.exists()) return registryStatus;
        QLockFile lock(folder.filePath("sync.lock")); if (!lock.tryLock(0)) return registryStatus;
        int count=0; QString firstError;
        for (const auto& name:folder.entryList({"*.json"},QDir::Files,QDir::Name)) {
            if (++count>limit) break;
            QFile file(folder.filePath(name)); if (!file.open(QIODevice::ReadOnly)) fail("OUTBOX_READ_FAILED");
            auto doc=QJsonDocument::fromJson(file.readAll()); file.close(); if (!doc.isObject()) fail("INVALID_OUTBOX");
            try {
                repository.saveInspection(doc.object()); if (!file.remove()) fail("OUTBOX_REMOVE_FAILED");
            } catch (const std::exception& e) {
                if (firstError.isEmpty()) firstError=QString::fromUtf8(e.what());
            }
        }
        return firstError.isEmpty() ? registryStatus : firstError;
    } catch (const std::exception& e) { return QString::fromUtf8(e.what()); }
}


QString MySqlRepository::maintainEvidence(const QString& directory) const {
    if (directory.isEmpty()) return {};
    const auto now=QDateTime::currentDateTimeUtc();
    const auto policy=EvidenceArchive::Policy::fromEnvironment();
    const auto normalCut=time(now.addDays(-policy.normalDays));
    const auto abnormalCut=time(now.addDays(-policy.abnormalDays));
    QSet<QString> protectedTasks;
    const QDir pending(QDir(directory).filePath(".mysql_pending"));
    for (const auto& name:pending.entryList({"*.json"},QDir::Files)) {
        QFile file(pending.filePath(name));
        if (!file.open(QIODevice::ReadOnly)) fail("RETENTION_PENDING_UNREADABLE");
        const auto doc=QJsonDocument::fromJson(file.readAll());
        if (!doc.isObject()) fail("RETENTION_PENDING_INVALID");
        for (const auto& key:{"task_id","parent_task_id","reuse_original_task_id"}) {
            const auto id=doc.object()[key].toString(); if(!id.isEmpty()) protectedTasks.insert(id);
        }
    }
    Connection c(m_settings);
    // Unknown legacy results are conservatively retained for the abnormal window.
    const QByteArray eligible="((h.processed_at<? AND NOT EXISTS(SELECT 1 FROM reading_reviews r WHERE r.task_id=h.task_id AND r.decision IN('corrected','unreadable') AND r.reviewed_at>=?)) OR "
        "(h.processed_at<? AND h.recognition_success=1 AND h.confidence>=0.4 AND "
        "JSON_UNQUOTE(JSON_EXTRACT(h.result_snapshot,'$.archive.anomaly'))='false' AND "
        "NOT EXISTS(SELECT 1 FROM reading_reviews r WHERE r.task_id=h.task_id AND r.decision IN('corrected','unreadable'))))";
    const Row cutoffs{abnormalCut,abnormalCut,normalCut};
    int expiredTasks=0,removedBlobs=0,expiredLocal=0,localFailures=0;
    {
        Transaction tx(c);
        const auto tasks=c.exec("SELECT h.task_id FROM inspection_history h WHERE "+eligible+
            " AND EXISTS(SELECT 1 FROM inspection_evidence e WHERE e.task_id=h.task_id AND e.availability='available')"
            " ORDER BY h.processed_at LIMIT 500 FOR UPDATE",cutoffs);
        for(const auto& row:tasks) {
            const auto id=str(row[0]); if(protectedTasks.contains(id)) continue;
            c.exec("UPDATE inspection_evidence SET availability='expired',storage_backend='file',storage_key=CONCAT('expired:',task_id,':',evidence_kind),image_id=NULL WHERE task_id=? AND availability='available'",{id.toUtf8()});
            ++expiredTasks;
        }
        // Shared BLOBs are removed only when no task references them, including reanalysis.
        const auto blobs=c.exec("SELECT b.image_id FROM image_blobs b WHERE NOT EXISTS(SELECT 1 FROM inspection_evidence e WHERE e.image_id=b.image_id) LIMIT 1500 FOR UPDATE");
        for(const auto& row:blobs) {
            c.exec("DELETE FROM image_blobs WHERE image_id=? AND NOT EXISTS(SELECT 1 FROM inspection_evidence e WHERE e.image_id=image_blobs.image_id)",{row[0]});
            ++removedBlobs;
        }
        tx.commit();
    }
    // Re-evaluate local tasks against the authoritative DB, including manual reviews.
    // Missing DB records or pending uploads are never eligible for local deletion.
    const QDir root(directory);
    Transaction localTx(c);
    Row localArgs=cutoffs; localArgs.push_back(QFileInfo(directory).absoluteFilePath().toUtf8());
    const auto localTasks=c.exec("SELECT h.task_id FROM inspection_history h WHERE "+eligible+
        " AND JSON_EXTRACT(h.result_snapshot,'$.local_images_expired_at') IS NULL"
        " AND (JSON_EXTRACT(h.result_snapshot,'$.archive.local_root') IS NULL OR JSON_UNQUOTE(JSON_EXTRACT(h.result_snapshot,'$.archive.local_root'))=?)"
        " AND EXISTS(SELECT 1 FROM inspection_evidence e WHERE e.task_id=h.task_id AND e.availability='expired')"
        " ORDER BY h.processed_at LIMIT 500",localArgs);
    for(const auto& row:localTasks) {
        const auto id=str(row[0]); if(protectedTasks.contains(id)) continue;
        if(EvidenceArchive::expireLocalImages(directory,id,now)) {
            ++expiredLocal;
            c.exec("UPDATE inspection_history SET result_snapshot=JSON_SET(COALESCE(result_snapshot,JSON_OBJECT()),'$.local_images_expired_at',?) WHERE task_id=?",{time(now),id.toUtf8()});
        } else ++localFailures;
    }
    localTx.commit();
    qint64 localBytes=0;
    for(const auto& entry:root.entryInfoList(QDir::Dirs|QDir::NoDotAndDotDot)) {
        if(QUuid(entry.fileName()).isNull() || entry.isSymLink()) continue;
        for(const auto& image:QDir(entry.absoluteFilePath()).entryInfoList({"*.jpg","*.png"},QDir::Files))
            if(!image.isSymLink()) localBytes+=image.size();
    }
    const auto stored=c.exec("SELECT COALESCE(SUM(byte_size),0) FROM image_blobs");
    const QStorageInfo disk(QFileInfo(directory).absoluteFilePath());
    return QString::fromUtf8(QJsonDocument(QJsonObject{{"normal_days",policy.normalDays},{"abnormal_days",policy.abnormalDays},
        {"expired_database_tasks",expiredTasks},{"removed_unreferenced_blobs",removedBlobs},{"expired_local_tasks",expiredLocal},
        {"local_cleanup_failures",localFailures},{"database_image_bytes",str(stored[0][0]).toDouble()},
        {"local_image_bytes",double(localBytes)},{"disk_available_bytes",double(disk.bytesAvailable())},
        {"low_disk_space",disk.isValid() && disk.bytesAvailable()<2LL*1024*1024*1024}}).toJson(QJsonDocument::Compact));
}
