#pragma once
#include "AutoCameraDiscoveryService.hpp"

inline bool sameDeviceConnection(const DiscoveredCameraItem& a,const DiscoveredCameraItem& b) {
    return a.ipOrSn==b.ipOrSn && a.port==b.port && a.rtspPort==b.rtspPort &&
        a.rtspUrl==b.rtspUrl && a.protocolType==b.protocolType && a.username==b.username && a.password==b.password;
}
// Preserve the task's frozen calibration, template and safety configuration.
inline void updateDeviceConnection(DiscoveredCameraItem& target,const DiscoveredCameraItem& current) {
    target.ipOrSn=current.ipOrSn; target.port=current.port; target.rtspPort=current.rtspPort;
    target.rtspUrl=current.rtspUrl; target.protocolType=current.protocolType;
    target.username=current.username; target.password=current.password;
}
