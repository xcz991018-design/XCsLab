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
    "${CMAKE_CURRENT_SOURCE_DIR}/include/MainWindow.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/CardinalSelectorDialog.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/CameraInfoEditDialog.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/PointerDetector.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/GB28181StreamManager.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/ONVIFDeviceManager.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/AutoCameraDiscoveryService.hpp"
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
)

add_executable(gauge_reader_test ${CLI_SOURCES})
target_link_libraries(gauge_reader_test PRIVATE ${OpenCV_LIBS} ${QT_LIBS})

add_executable(gauge_reader_gui WIN32 ${GUI_SOURCES})
target_link_libraries(gauge_reader_gui PRIVATE ${OpenCV_LIBS} ${QT_LIBS})

# Copy DLLs to target binary directory on Windows
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

