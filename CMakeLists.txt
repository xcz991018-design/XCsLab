cmake_minimum_required(VERSION 3.15)
project(GaugeReaderTest VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# Hint OpenCV Location
if(MSVC)
    add_compile_options(/utf-8)
    set(OpenCV_DIR "C:/openCV/opencv/build")
else()
    set(OpenCV_DIR "C:/openCV/OpenCV-MinGW-Build-OpenCV-3.4.8-x64/OpenCV-MinGW-Build-OpenCV-3.4.8-x64")
endif()

find_package(OpenCV REQUIRED HINTS ${OpenCV_DIR})

message(STATUS "OpenCV found version: ${OpenCV_VERSION}")
message(STATUS "OpenCV include dirs: ${OpenCV_INCLUDE_DIRS}")
message(STATUS "OpenCV libs: ${OpenCV_LIBS}")

# Hint Qt Location
if(NOT CMAKE_PREFIX_PATH)
    if(EXISTS "C:/Qt/Qt5.14.2/5.14.2/msvc2017_64")
        set(CMAKE_PREFIX_PATH "C:/Qt/Qt5.14.2/5.14.2/msvc2017_64")
    elseif(EXISTS "C:/Qt/qt6.5/6.5.3/msvc2019_64")
        set(CMAKE_PREFIX_PATH "C:/Qt/qt6.5/6.5.3/msvc2019_64")
    endif()
endif()

find_package(Qt5 COMPONENTS Widgets Core Gui Concurrent Network QUIET)
if(Qt5_FOUND)
    message(STATUS "Found Qt5: ${Qt5_VERSION}")
    set(QT_LIBS Qt5::Widgets Qt5::Core Qt5::Gui Qt5::Concurrent Qt5::Network)
else()
    find_package(Qt6 COMPONENTS Widgets Core Gui Concurrent Network REQUIRED)
    message(STATUS "Found Qt6: ${Qt6_VERSION}")
    set(QT_LIBS Qt6::Widgets Qt6::Core Qt6::Gui Qt6::Concurrent Qt6::Network)
endif()

set(MYSQL_ROOT "C:/Program Files/MySQL/MySQL Server 8.0" CACHE PATH "MySQL client development installation")
find_path(MYSQL_INCLUDE_DIR mysql.h HINTS "${MYSQL_ROOT}/include" REQUIRED)
find_library(MYSQL_CLIENT_LIBRARY NAMES libmysql mysqlclient HINTS "${MYSQL_ROOT}/lib" REQUIRED)
add_library(gauge_mysql_client INTERFACE)
target_include_directories(gauge_mysql_client INTERFACE "${MYSQL_INCLUDE_DIR}")
target_link_libraries(gauge_mysql_client INTERFACE "${MYSQL_CLIENT_LIBRARY}")

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${OpenCV_INCLUDE_DIRS}
    "C:/Users/95254/code/jlink-restful-cpp-demo-main/jlink-restful-cpp-demo-main/include"
    "C:/Users/95254/code/jlink-restful-cpp-demo-main/jlink-restful-cpp-demo-main/third"
)

# CLI Target Sources
set(CLI_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/FileCameraSource.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/GaugeProfile.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/GaugeReaderEngine.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/PointerDetector.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/GB28181StreamManager.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ONVIFDeviceManager.cpp"
)

# GUI Target Sources
set(GUI_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/AccessControl.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/GaugeTemplateStore.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/GaugeTemplateDialog.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/GaugeTemplateDialog.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/MySqlErrorText.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/DatabaseSettingsStore.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/DatabaseConnectionDialog.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/MySqlRepository.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/DeviceCodec.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/DatabaseHistoryDialog.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/CameraPreviewService.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/CameraPreviewService.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/InspectionService.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/InspectionService.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/MainWindow.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/CardinalSelectorDialog.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/LiquidSurface.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/AutoDialCalibration.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/CameraInfoEditDialog.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/PointerDetector.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/GB28181StreamManager.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/ONVIFDeviceManager.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/AutoCameraDiscoveryService.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/DeviceConfigManager.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/main_gui.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/MainWindow.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/CardinalSelectorDialog.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/CameraInfoEditDialog.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/FileCameraSource.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/GaugeProfile.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/GaugeReaderEngine.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/PointerDetector.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/GB28181StreamManager.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ONVIFDeviceManager.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/AutoCameraDiscoveryService.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/DeviceConfigManager.cpp"
)

add_executable(gauge_reader_test ${CLI_SOURCES})
target_link_libraries(gauge_reader_test PRIVATE ${OpenCV_LIBS} ${QT_LIBS} ws2_32)

add_executable(gauge_reader_gui WIN32 ${GUI_SOURCES})
target_sources(gauge_reader_gui PRIVATE src/IsolatedCapture.cpp)
target_link_libraries(gauge_reader_gui PRIVATE ${OpenCV_LIBS} ${QT_LIBS} gauge_mysql_client ws2_32 crypt32)

# Copy DLLs and config files to target binary directory
if(WIN32)
    if(MSVC)
        add_custom_command(TARGET gauge_reader_gui POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "C:/openCV/opencv/build/x64/vc16/bin/opencv_world4120.dll"
            "$<TARGET_FILE_DIR:gauge_reader_gui>"
        )
        add_custom_command(TARGET gauge_reader_test POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "C:/openCV/opencv/build/x64/vc16/bin/opencv_world4120.dll"
            "$<TARGET_FILE_DIR:gauge_reader_test>"
        )
    endif()
endif()

# Headless runtime: same inspection pipeline, no MainWindow dependency.
add_library(gauge_inspection_runtime STATIC
    src/GaugeTemplateStore.cpp src/GaugeTemplateMigration.cpp
    src/MySqlErrorText.cpp
    src/DatabaseSettingsStore.cpp
    src/MySqlRepository.cpp src/DeviceCodec.cpp include/DeviceConfigManager.hpp
    src/CameraPreviewService.cpp include/CameraPreviewService.hpp
    src/InspectionService.cpp include/InspectionService.hpp
    src/DeviceConfigManager.cpp src/FileCameraSource.cpp src/GaugeProfile.cpp
    src/GaugeReaderEngine.cpp src/PointerDetector.cpp src/ONVIFDeviceManager.cpp)
target_link_libraries(gauge_inspection_runtime PUBLIC ${OpenCV_LIBS} ${QT_LIBS} gauge_mysql_client ws2_32 crypt32)
add_executable(gauge_reader_service src/main_service.cpp)
target_sources(gauge_inspection_runtime PRIVATE src/IsolatedCapture.cpp)
target_link_libraries(gauge_reader_service PRIVATE gauge_inspection_runtime)
target_link_libraries(gauge_reader_test PRIVATE gauge_inspection_runtime)

if(WIN32)
    foreach(runtime_target gauge_reader_gui gauge_reader_service gauge_reader_test)
        if(TARGET ${runtime_target})
            foreach(qt_module Core Network Concurrent)
                if(TARGET Qt5::${qt_module})
                    add_custom_command(TARGET ${runtime_target} POST_BUILD
                        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:Qt5::${qt_module}>" "$<TARGET_FILE_DIR:${runtime_target}>")
                elseif(TARGET Qt6::${qt_module})
                    add_custom_command(TARGET ${runtime_target} POST_BUILD
                        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:Qt6::${qt_module}>" "$<TARGET_FILE_DIR:${runtime_target}>")
                endif()
            endforeach()
            foreach(client_dll "${MYSQL_ROOT}/lib/libmysql.dll" "${MYSQL_ROOT}/bin/libssl-3-x64.dll" "${MYSQL_ROOT}/bin/libcrypto-3-x64.dll")
                if(EXISTS "${client_dll}")
                    add_custom_command(TARGET ${runtime_target} POST_BUILD
                        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${client_dll}" "$<TARGET_FILE_DIR:${runtime_target}>")
                endif()
            endforeach()
        endif()
    endforeach()
endif()
