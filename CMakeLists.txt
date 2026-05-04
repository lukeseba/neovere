cmake_minimum_required(VERSION 3.5)
project(SimpleQtApp)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_PREFIX_PATH "C:/Qt/6.8.0/mingw_64;C:/opencv/build/x64/vc16/lib")


set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6Widgets REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Widgets Multimedia MultimediaWidgets)
find_package(Python3 REQUIRED COMPONENTS Interpreter Development)

find_package(OpenCV REQUIRED)
include_directories(${OpenCV_INCLUDE_DIRS})
include_directories(${Python3_INCLUDE_DIRS})

configure_file(neovere.py ${CMAKE_BINARY_DIR}/resources/code/neovere.py COPYONLY)

set(SOURCES main.cpp MaintainFrame.cpp MaintainFrame.h
        MediaFrame.cpp
        MediaFrame.h
        VideoSlider.cpp
        VideoSlider.h
        BoolStateButton.cpp
        BoolStateButton.h
        PythonHighlighter.h
        PythonCodeEditor.cpp
        PythonCodeEditor.h
        LineNumberArea.h
        TabsWidget.cpp
        TabsWidget.h
        TabButton.cpp
        TabButton.h
        SearchTextEdit.cpp
        SearchTextEdit.h
        ButtonTextEdit.h
        AiTextBoxWrapper.h
)
qt_add_resources(SOURCES resources.qrc)

add_executable(SimpleQtApp ${SOURCES})

target_link_libraries(SimpleQtApp PRIVATE Qt6::Widgets Qt6::Multimedia Qt6::MultimediaWidgets ${OpenCV_LIBS} ${Python3_LIBRARIES})
