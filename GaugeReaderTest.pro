# ===================================================================
# GaugeReaderTest - Qt Creator Project File (.pro)
# ===================================================================

QT       += core gui widgets concurrent network

CONFIG   += c++17
CONFIG   += utf8_source

# MSVC2017: QStringLiteral arguments must use u"..."; mixing u"" and
# narrow UTF-8 literals corrupts Chinese even with /utf-8.
# Force MSVC compiler to use UTF-8 source and execution charset
win32-msvc {
    QMAKE_CXXFLAGS += /utf-8
    QMAKE_CFLAGS += /utf-8
}

TARGET    = GaugeReaderTest
TEMPLATE  = app

# Project Include Directories
INCLUDEPATH += $$PWD/include
isEmpty(MYSQL_ROOT): MYSQL_ROOT = "C:/Program Files/MySQL/MySQL Server 8.0"
INCLUDEPATH += "$$MYSQL_ROOT/include"
LIBS += -L"$$MYSQL_ROOT/lib" -llibmysql

# OpenCV Configuration (Auto switch Debug / Release)
win32 {
    INCLUDEPATH += "C:/openCV/opencv/build/include"
    
    CONFIG(debug, debug|release) {
        LIBS += -L"C:/openCV/opencv/build/x64/vc16/lib" -lopencv_world4120d
    } else {
        LIBS += -L"C:/openCV/opencv/build/x64/vc16/lib" -lopencv_world4120
    }

    LIBS += -lws2_32 -lcrypt32
}

# Header Files
HEADERS += \
    $$PWD/include/CameraHealthSchedule.hpp \
    $$PWD/include/DeviceCalibration.hpp \
    $$PWD/include/InspectionCalibration.hpp \
    $$PWD/include/CameraDiscoveryTargets.hpp \
    $$PWD/include/GaugeTemplateRecord.hpp \
    $$PWD/include/MySqlTemplateSchema.hpp \
    $$PWD/include/SnapshotFrameReader.hpp \
    $$PWD/include/GaugeTemplateStore.hpp \
    $$PWD/include/GaugeTemplateDialog.hpp \
    $$PWD/include/MySqlBootstrapSchema.hpp \
    $$PWD/include/MySqlErrorText.hpp \
    $$PWD/include/DatabaseSettingsStore.hpp \
    $$PWD/include/DatabaseConnectionDialog.hpp \
    $$PWD/include/DeviceCodec.hpp \
    $$PWD/include/MySqlRepository.hpp \
    $$PWD/include/DatabaseHistoryDialog.hpp \
    $$PWD/include/CameraPreviewService.hpp \
    $$PWD/include/InspectionService.hpp \
    $$PWD/include/FileCameraSource.hpp \
    $$PWD/include/GaugeProfile.hpp \
    $$PWD/include/GaugeReaderEngine.hpp \
    $$PWD/include/ICameraSource.hpp \
    $$PWD/include/SDKCameraSourceStub.hpp \
    $$PWD/include/CardinalSelectorDialog.hpp \
    $$PWD/include/CameraInfoEditDialog.hpp \
    $$PWD/include/MainWindow.hpp \
    $$PWD/include/PointerDetector.hpp \
    $$PWD/include/GB28181StreamManager.hpp \
    $$PWD/include/ONVIFDeviceManager.hpp \
    $$PWD/include/AutoCameraDiscoveryService.hpp \
    $$PWD/include/DeviceConfigManager.hpp

# Source Files
SOURCES += \
    $$PWD/src/GaugeTemplateStore.cpp \
    $$PWD/src/GaugeTemplateDialog.cpp \
    $$PWD/src/IsolatedCapture.cpp \
    $$PWD/src/MySqlErrorText.cpp \
    $$PWD/src/DatabaseSettingsStore.cpp \
    $$PWD/src/DatabaseConnectionDialog.cpp \
    $$PWD/src/MySqlRepository.cpp \
    $$PWD/src/DeviceCodec.cpp \
    $$PWD/src/DatabaseHistoryDialog.cpp \
    $$PWD/src/CameraPreviewService.cpp \
    $$PWD/src/InspectionService.cpp \
    $$PWD/src/main_gui.cpp \
    $$PWD/src/MainWindow.cpp \
    $$PWD/src/CardinalSelectorDialog.cpp \
    $$PWD/src/CameraInfoEditDialog.cpp \
    $$PWD/src/GaugeProfile.cpp \
    $$PWD/src/GaugeReaderEngine.cpp \
    $$PWD/src/PointerDetector.cpp \
    $$PWD/src/FileCameraSource.cpp \
    $$PWD/src/GB28181StreamManager.cpp \
    $$PWD/src/ONVIFDeviceManager.cpp \
    $$PWD/src/AutoCameraDiscoveryService.cpp \
    $$PWD/src/DeviceConfigManager.cpp

# Use native, quoted paths: cmd.exe copy rejects forward-slash destinations.
win32-msvc {
    CONFIG(debug, debug|release) {
        OPENCV_RUNTIME = C:/openCV/opencv/build/x64/vc16/bin/opencv_world4120d.dll
    } else {
        OPENCV_RUNTIME = C:/openCV/opencv/build/x64/vc16/bin/opencv_world4120.dll
    }
    OPENCV_RUNTIME_PATH = $$shell_path($$OPENCV_RUNTIME)
    RUNTIME_OUT_PATH = $$shell_path($$OUT_PWD)
    QMAKE_POST_LINK += $(COPY_FILE) $$shell_quote($$OPENCV_RUNTIME_PATH) $$shell_quote($$RUNTIME_OUT_PATH)
}

HEADERS += $$PWD/include/ScaleCalibration.hpp

HEADERS += $$PWD/include/RecognitionQuality.hpp

HEADERS += $$PWD/include/DeviceCredentialStore.hpp


HEADERS += $$PWD/include/LiquidSurface.hpp

HEADERS += $$PWD/include/AutoDialCalibration.hpp

SOURCES += $$PWD/src/AccessControl.cpp
HEADERS += $$PWD/include/AccessControl.hpp

HEADERS += $$PWD/include/UiTheme.hpp

HEADERS += $$PWD/include/DeviceConnection.hpp
