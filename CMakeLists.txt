cmake_minimum_required(VERSION 3.5)
project(SimpleQtApp)

set(CMAKE_CXX_STANDARD 14)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6Widgets REQUIRED)

set(SOURCES main.cpp MaintainFrame.cpp MaintainFrame.h)
qt_add_resources(SOURCES resources.qrc)

add_executable(SimpleQtApp ${SOURCES})

target_link_libraries(SimpleQtApp Qt6::Widgets)
