# ===================================================================
# GaugeReaderTest - Qt Creator Project File (.pro)
# ===================================================================

QT       += core gui widgets concurrent network

CONFIG   += c++17
CONFIG   += utf8_source

TARGET    = GaugeReaderTest
TEMPLATE  = app

# Project Include Directories
INCLUDEPATH += $$PWD/include

# OpenCV Configuration (Auto switch Debug / Release)
win32 {
    INCLUDEPATH += "C:/openCV/opencv/build/include"
    
    CONFIG(debug, debug|release) {
        LIBS += -L"C:/openCV/opencv/build/x64/vc16/lib" -lopencv_world4120d
    } else {
        LIBS += -L"C:/openCV/opencv/build/x64/vc16/lib" -lopencv_world4120
    }
}

# Header Files
HEADERS += \
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
    $$PWD/include/AutoCameraDiscoveryService.hpp

# Source Files
SOURCES += \
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
    $$PWD/src/AutoCameraDiscoveryService.cpp

# Copy OpenCV DLL to output folder after build
win32:MSVC {
    CONFIG(debug, debug|release) {
        QMAKE_POST_LINK += $(COPY_FILE) "C:\\openCV\\opencv\\build\\x64\\vc16\\bin\\opencv_world4120d.dll" "$$OUT_PWD"
    } else {
        QMAKE_POST_LINK += $(COPY_FILE) "C:\\openCV\\opencv\\build\\x64\\vc16\\bin\\opencv_world4120.dll" "$$OUT_PWD"
    }
}
