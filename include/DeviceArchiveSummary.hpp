#pragma once
#include "AutoCameraDiscoveryService.hpp"
#include "DeviceCalibration.hpp"
#include "DeviceCodec.hpp"
#include <QUrl>
#include <QObject>

struct DeviceArchiveSummary {
    QString identity, hardware, network, location, specification, safety, calibration;
    static DeviceArchiveSummary fromDevice(const DiscoveredCameraItem& device) {
        const auto text = [](const QString& value) { return value.trimmed().isEmpty() ? QObject::tr("未填写") : value; };
        DeviceArchiveSummary result;
        result.identity = QObject::tr("设备编号：%1\n设备名称：%2").arg(text(device.cameraId), text(device.customName));
        result.hardware = QObject::tr("硬件 UUID：%1").arg(text(device.hardwareId));
        QUrl uri(device.rtspUrl);
        QString stream = device.rtspUrl;
        if (!uri.host().isEmpty()) {
            uri.setUserName(QString()); uri.setPassword(QString()); stream = uri.toString();
        }
        result.network = QObject::tr("通信协议：%1\n地址/SN：%2\n服务端口：%3；RTSP 端口：%4\n视频地址：%5\n连接账号：%6\n设备状态：%7\n最近在线时间：%8")
            .arg(text(device.protocolName), text(device.ipOrSn)).arg(device.port).arg(device.rtspPort)
            .arg(stream.isEmpty() ? QObject::tr("由 ONVIF 查询") : stream, text(device.username),
                device.isOnline ? QObject::tr("在线") : QObject::tr("离线"), text(device.lastSeenTime));
        result.location = QObject::tr("变电站/厂区：%1\n安装区域：%2\n安装点位：%3\n设备标签：%4")
            .arg(text(device.substation), text(device.area), text(device.installationLocation), text(device.deviceTag));
        result.specification = QObject::tr("表计模板：%1\n表计类型：%2\n生产厂家：%3\n规格型号：%4\n精度等级：%5")
            .arg(text(device.defaultProfileId), text(device.gaugeType), text(device.manufacturer), text(device.modelName), text(device.accuracyClass));
        const auto& range = device.safeWorkingRange;
        if (!range.valid()) result.safety = QObject::tr("安全范围检查：%1\n安全工作范围：待配置或配置无效")
            .arg(range.enabled ? QObject::tr("已启用") : QObject::tr("未启用"));
        else result.safety = QObject::tr("安全范围检查：%1\n正常工作范围：[%2，%3]\n允许偏差：−%4 / ＋%5\n安全工作范围：[%6，%7]（单位与表计模板一致）")
            .arg(range.enabled ? QObject::tr("已启用") : QObject::tr("未启用"))
            .arg(range.normalMin, 0, 'f', 6).arg(range.normalMax, 0, 'f', 6)
            .arg(range.allowanceMinus, 0, 'f', 6).arg(range.allowancePlus, 0, 'f', 6)
            .arg(range.lower(), 0, 'f', 6).arg(range.upper(), 0, 'f', 6);
        result.calibration = hasSavedDeviceCalibration(deviceToJson(device).value("calibration").toObject()) ?
            QObject::tr("表计标定：已标定；刻度点：%1 个").arg(device.scalePoints.size()) : QObject::tr("表计标定：未标定");
        return result;
    }
};
