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

# Automatically deploy config folder (devices.json, gauge_profile.json) to output directories on build
add_custom_command(TARGET gauge_reader_gui POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:gauge_reader_gui>/config"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/config"
        "$<TARGET_FILE_DIR:gauge_reader_gui>/config"
    COMMENT "Deploying config directory to gauge_reader_gui output directory"
)

add_custom_command(TARGET gauge_reader_test POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:gauge_reader_test>/config"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/config"
        "$<TARGET_FILE_DIR:gauge_reader_test>/config"
    COMMENT "Deploying config directory to gauge_reader_test output directory"
)


# Headless runtime: same inspection pipeline, no MainWindow dependency.
add_library(gauge_inspection_runtime STATIC
    src/MySqlErrorText.cpp
    src/DatabaseSettingsStore.cpp
    src/MySqlRepository.cpp src/DeviceCodec.cpp include/DeviceConfigManager.hpp
    src/CameraPreviewService.cpp include/CameraPreviewService.hpp
    src/InspectionService.cpp include/InspectionService.hpp
    src/DeviceConfigManager.cpp src/FileCameraSource.cpp src/GaugeProfile.cpp
    src/GaugeReaderEngine.cpp src/PointerDetector.cpp src/ONVIFDeviceManager.cpp)
target_link_libraries(gauge_inspection_runtime PUBLIC ${OpenCV_LIBS} ${QT_LIBS} gauge_mysql_client ws2_32 crypt32)
add_executable(gauge_reader_service src/main_service.cpp)
target_link_libraries(gauge_reader_service PRIVATE gauge_inspection_runtime)
include(CTest)
if(BUILD_TESTING)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/tests-disabled-mysql.json" "{\"version\":1,\"enabled\":false}")
    add_executable(inspection_service_tests tests/InspectionServiceTest.cpp)
    target_link_libraries(inspection_service_tests PRIVATE gauge_inspection_runtime)
    add_test(NAME inspection_service_pipeline COMMAND inspection_service_tests)
    add_executable(gauge_run_mode_tests tests/GaugeRunModeTest.cpp)
    target_link_libraries(gauge_run_mode_tests PRIVATE gauge_inspection_runtime)
    add_test(NAME gauge_run_modes COMMAND gauge_run_mode_tests)
    add_executable(input_acquisition_tests tests/InputAcquisitionTest.cpp)
    target_link_libraries(input_acquisition_tests PRIVATE gauge_inspection_runtime)
    add_test(NAME explicit_input_acquisition COMMAND input_acquisition_tests)
    add_executable(mysql_integration_tests tests/MySqlIntegrationTest.cpp src/DatabaseHistoryDialog.cpp src/DatabaseConnectionDialog.cpp)
    target_link_libraries(mysql_integration_tests PRIVATE gauge_inspection_runtime)
    target_compile_definitions(mysql_integration_tests PRIVATE REPO_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME mysql_storage_and_review COMMAND mysql_integration_tests)
    set_tests_properties(mysql_storage_and_review PROPERTIES SKIP_RETURN_CODE 77 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(database_configuration_tests tests/DatabaseConfigurationTest.cpp src/DatabaseConnectionDialog.cpp)
    target_link_libraries(database_configuration_tests PRIVATE gauge_inspection_runtime)
    add_test(NAME mysql_connection_configuration COMMAND database_configuration_tests)
    add_executable(mysql_error_text_tests tests/MySqlErrorTextTest.cpp src/MySqlErrorText.cpp)
    target_link_libraries(mysql_error_text_tests PRIVATE ${QT_LIBS})
    add_test(NAME mysql_error_message_encoding COMMAND mysql_error_text_tests)
    set_tests_properties(mysql_connection_configuration PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    set_tests_properties(inspection_service_pipeline gauge_run_modes explicit_input_acquisition mysql_storage_and_review
        PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;GAUGE_MYSQL_DATABASE=;GAUGE_MYSQL_CONFIG_PATH=${CMAKE_CURRENT_BINARY_DIR}/tests-disabled-mysql.json")
endif()

if(WIN32)
    foreach(runtime_target gauge_reader_gui gauge_reader_service inspection_service_tests gauge_run_mode_tests input_acquisition_tests mysql_integration_tests database_configuration_tests)
        if(TARGET ${runtime_target})
            foreach(client_dll "${MYSQL_ROOT}/lib/libmysql.dll" "${MYSQL_ROOT}/bin/libssl-3-x64.dll" "${MYSQL_ROOT}/bin/libcrypto-3-x64.dll")
                if(EXISTS "${client_dll}")
                    add_custom_command(TARGET ${runtime_target} POST_BUILD
                        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${client_dll}" "$<TARGET_FILE_DIR:${runtime_target}>")
                endif()
            endforeach()
        endif()
    endforeach()
endif()
