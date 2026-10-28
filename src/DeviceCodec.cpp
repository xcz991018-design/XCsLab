#include "DeviceCodec.hpp"
#include <QUrl>
#include "CameraIdentity.hpp"
#include "DeviceCalibration.hpp"
static QString protocolToString(CameraProtocolType type) {
    switch (type) {
    case CameraProtocolType::LOCAL_FILE: return "LOCAL_FILE";
    case CameraProtocolType::ONVIF_RTSP_STREAM: return "ONVIF_RTSP_STREAM";
    case CameraProtocolType::SDK_JLINK: return "SDK_JLINK";
    case CameraProtocolType::GB28181_PS_STREAM: return "GB28181_PS_STREAM";
    case CameraProtocolType::CUSTOM_SDK: return "CUSTOM_SDK";
    default: return "LOCAL_FILE";
    }
}

static CameraProtocolType stringToProtocol(const QString& str) {
    QString s = str.trimmed().toUpper();
    if (s == "ONVIF_RTSP_STREAM" || s == "ONVIF") return CameraProtocolType::ONVIF_RTSP_STREAM;
    if (s == "SDK_JLINK" || s == "JLINK" || s == "XIONGMAI") return CameraProtocolType::SDK_JLINK;
    if (s == "GB28181_PS_STREAM" || s == "GB28181") return CameraProtocolType::GB28181_PS_STREAM;
    if (s == "CUSTOM_SDK") return CameraProtocolType::CUSTOM_SDK;
    return CameraProtocolType::LOCAL_FILE;
}

DiscoveredCameraItem deviceFromJson(const QJsonObject& obj) {
    DiscoveredCameraItem item;
    item.cameraId = obj.value("camera_id").toString();
    item.hardwareId = CameraIdentity::normalize(obj.value("hardware_id").toString());


    item.customName = obj.value("custom_name").toString();
    item.ipOrSn = obj.value("ip_or_sn").toString();
    item.protocolType = stringToProtocol(obj.value("protocol_type").toString());
    item.protocolName = obj.value("protocol_name").toString("IP Camera");
    item.modelName = obj.value("model_name").toString("Network Camera");
    item.port = obj.value("port").toInt(80);
    item.rtspPort = obj.value("rtsp_port").toInt(554);
    item.rtspUrl = obj.value("rtsp_url").toString();
    item.username = obj.value("username").toString("");
    item.password = obj.value("password").toString("");
    item.defaultProfileId = obj.value("default_profile_id").toString();
    item.isOnline = obj.value("is_online").toBool(true);
    item.lastSeenTime = obj.value("last_seen_time").toString();
    item.safeWorkingRange = SafeWorkingRange::fromJson(obj.value("safe_working_range").toObject());

    // Asset & Physical Installation Metadata (no fake defaults, strictly read from configuration)
    item.substation = obj.value("substation").toString("").trimmed();
    item.area = obj.value("area").toString("").trimmed();
    item.installationLocation = obj.value("installation_location").toString(obj.value("location").toString("")).trimmed();
    item.installationPosition = obj.value("installation_position").toString().trimmed();
    item.deviceTag = obj.value("device_tag").toString("").trimmed();
    item.gaugeType = obj.value("gauge_type").toString("").trimmed();
    item.manufacturer = obj.value("manufacturer").toString("").trimmed();
    item.accuracyClass = obj.value("accuracy_class").toString("").trimmed();

    // Load Calibration Parameters (supports direct fields or calibration sub-object)
    QJsonObject calObj = obj.contains("calibration") ? obj["calibration"].toObject() : obj;
    item.hasCalibration = hasSavedDeviceCalibration(calObj);
    item.scalePoints = calObj["scale_points"].toArray();
    item.meterRange = calObj["meter_range"].toObject();
    item.perspectiveEnabled = calObj.value("perspective_correction_enabled").toBool(false);
    item.calibratedWidth = calObj.value("calibrated_width").toInt(0);
    item.calibratedHeight = calObj.value("calibrated_height").toInt(0);
    item.pt0_x = calObj.value("pt_0_x").toDouble(-1.0);
    item.pt0_y = calObj.value("pt_0_y").toDouble(-1.0);
    item.pt1_x = calObj.value("pt_1_x").toDouble(-1.0);
    item.pt1_y = calObj.value("pt_1_y").toDouble(-1.0);
    item.pt2_x = calObj.value("pt_2_x").toDouble(-1.0);
    item.pt2_y = calObj.value("pt_2_y").toDouble(-1.0);
    item.pt3_x = calObj.value("pt_3_x").toDouble(-1.0);
    item.pt3_y = calObj.value("pt_3_y").toDouble(-1.0);
    item.centerX = calObj.value("center_x").toDouble(-1.0);
    item.centerY = calObj.value("center_y").toDouble(-1.0);
    item.pointerBaseRadius = calObj.value("pointer_base_radius").toDouble(calObj.value("radius").toDouble(47.0));

    return item;
}
QJsonObject deviceToJson(const DiscoveredCameraItem& item, bool includeSecrets) {
    QJsonObject obj;
    obj["camera_id"] = item.cameraId;
    obj["hardware_id"] = CameraIdentity::normalize(item.hardwareId);
    obj["custom_name"] = item.customName;
    obj["ip_or_sn"] = item.ipOrSn;
    obj["protocol_type"] = protocolToString(item.protocolType);
    obj["protocol_name"] = item.protocolName;
    obj["model_name"] = item.modelName;
    obj["port"] = item.port;
    obj["rtsp_port"] = item.rtspPort;
    obj["rtsp_url"] = item.rtspUrl;
    obj["username"] = item.username;
    obj["password"] = item.password;
    obj["default_profile_id"] = item.defaultProfileId;
    obj["is_online"] = item.isOnline;
    obj["last_seen_time"] = item.lastSeenTime;
    obj["safe_working_range"] = item.safeWorkingRange.toJson();

    // Asset & Physical Installation Metadata
    obj["substation"] = item.substation;
    obj["area"] = item.area;
    obj["installation_location"] = item.installationLocation;
    obj["installation_position"] = item.installationPosition;
    obj["device_tag"] = item.deviceTag;
    obj["gauge_type"] = item.gaugeType;
    obj["manufacturer"] = item.manufacturer;
    obj["accuracy_class"] = item.accuracyClass;

    // Calibration Sub-Object
    QJsonObject calObj;
    calObj["perspective_correction_enabled"] = item.perspectiveEnabled;
    calObj["calibrated_width"] = item.calibratedWidth;
    calObj["calibrated_height"] = item.calibratedHeight;
    calObj["pt_0_x"] = item.pt0_x;
    calObj["pt_0_y"] = item.pt0_y;
    calObj["pt_1_x"] = item.pt1_x;
    calObj["pt_1_y"] = item.pt1_y;
    calObj["pt_2_x"] = item.pt2_x;
    calObj["pt_2_y"] = item.pt2_y;
    calObj["pt_3_x"] = item.pt3_x;
    calObj["pt_3_y"] = item.pt3_y;
    calObj["center_x"] = item.centerX;
    calObj["center_y"] = item.centerY;
    calObj["pointer_base_radius"] = item.pointerBaseRadius;
    calObj["calibration_confirmed"] = item.hasCalibration;
    if(!item.meterRange.isEmpty())calObj["meter_range"] = item.meterRange;
    if (!item.scalePoints.isEmpty()) calObj["scale_points"] = item.scalePoints;
    if (item.hasCalibration) obj["calibration"] = calObj;

    if (!includeSecrets) {
    obj.remove("password");
    QUrl uri(obj["rtsp_url"].toString());
    uri.setUserName(QString()); uri.setPassword(QString());
    obj["rtsp_url"] = uri.toString();
    }
    return obj;
}
