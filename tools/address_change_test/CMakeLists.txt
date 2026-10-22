cmake_minimum_required(VERSION 3.16)
project(address_change_test LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
find_package(Qt5 REQUIRED COMPONENTS Widgets Core Network Concurrent)
find_package(OpenCV REQUIRED)
set(MYSQL_ROOT "C:/Program Files/MySQL/MySQL Server 8.0")
add_executable(address_change_test main.cpp)
target_compile_options(address_change_test PRIVATE /utf-8)
target_include_directories(address_change_test PRIVATE ../../include ${OpenCV_INCLUDE_DIRS} "${MYSQL_ROOT}/include")
target_link_libraries(address_change_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../build/Release/gauge_inspection_runtime.lib"
    Qt5::Widgets Qt5::Core Qt5::Network Qt5::Concurrent ${OpenCV_LIBS}
    "${MYSQL_ROOT}/lib/libmysql.lib" ws2_32 crypt32)
