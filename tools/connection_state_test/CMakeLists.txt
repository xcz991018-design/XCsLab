cmake_minimum_required(VERSION 3.16)
project(connection_state_test LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
find_package(Qt5 REQUIRED COMPONENTS Widgets Core Network Concurrent)
find_package(OpenCV REQUIRED)
set(MYSQL_ROOT "C:/Program Files/MySQL/MySQL Server 8.0")
set(CMAKE_AUTOMOC ON)
add_executable(connection_state_test main.cpp ../../src/AutoCameraDiscoveryService.cpp ../../include/AutoCameraDiscoveryService.hpp)
target_compile_options(connection_state_test PRIVATE /utf-8)
target_include_directories(connection_state_test PRIVATE ../../include ${OpenCV_INCLUDE_DIRS} "${MYSQL_ROOT}/include")
target_link_libraries(connection_state_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../build/Release/gauge_inspection_runtime.lib"
    Qt5::Widgets Qt5::Core Qt5::Network Qt5::Concurrent ${OpenCV_LIBS}
    "${MYSQL_ROOT}/lib/libmysql.lib" ws2_32 crypt32)

add_executable(capture_failure_health_test capture_failure.cpp ../../src/AutoCameraDiscoveryService.cpp ../../include/AutoCameraDiscoveryService.hpp)
target_compile_options(capture_failure_health_test PRIVATE /utf-8)
target_include_directories(capture_failure_health_test PRIVATE ../../include ${OpenCV_INCLUDE_DIRS} "${MYSQL_ROOT}/include")
target_link_libraries(capture_failure_health_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../build/Release/gauge_inspection_runtime.lib"
    Qt5::Widgets Qt5::Core Qt5::Network Qt5::Concurrent ${OpenCV_LIBS}
    "${MYSQL_ROOT}/lib/libmysql.lib" ws2_32 crypt32)
