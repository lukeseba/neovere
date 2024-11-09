cmake_minimum_required(VERSION 3.5)
project(SimpleQtApp)

set(CMAKE_CXX_STANDARD 14)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6Widgets REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Widgets Multimedia MultimediaWidgets)

set(SOURCES main.cpp MaintainFrame.cpp MaintainFrame.h
        MediaFrame.cpp
        MediaFrame.h
        VideoSlider.cpp
        VideoSlider.h
        BoolStateButton.cpp
        BoolStateButton.h)
qt_add_resources(SOURCES resources.qrc)

add_executable(SimpleQtApp ${SOURCES})

target_link_libraries(SimpleQtApp PRIVATE Qt6::Widgets Qt6::Multimedia Qt6::MultimediaWidgets)