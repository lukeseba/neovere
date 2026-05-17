cmake_minimum_required(VERSION 3.5)
project(SimpleQtApp)

set(CMAKE_CXX_STANDARD 14)
if(APPLE)
    set(CMAKE_PREFIX_PATH "/opt/homebrew/opt/qt;/opt/homebrew/opt/opencv")
elseif(WIN32)
    set(CMAKE_PREFIX_PATH "C:/Qt/6.8.0/mingw_64;C:/opencv/build/x64/vc16/lib")
endif()

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
        FrameBufferReader.cpp
        FrameBufferReader.h
        PreviewWidget.cpp
        PreviewWidget.h
        PlaybackController.cpp
        PlaybackController.h
)
qt_add_resources(SOURCES resources.qrc OPTIONS --no-zstd)

add_executable(SimpleQtApp MACOSX_BUNDLE ${SOURCES})

target_link_libraries(SimpleQtApp PRIVATE Qt6::Widgets Qt6::Multimedia Qt6::MultimediaWidgets ${OpenCV_LIBS} ${Python3_LIBRARIES})

if(APPLE)
    set_target_properties(SimpleQtApp PROPERTIES
        MACOSX_BUNDLE TRUE
        OUTPUT_NAME "Neovere"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist"
        MACOSX_BUNDLE_BUNDLE_NAME "Neovere"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.neovere.app"
        MACOSX_BUNDLE_BUNDLE_VERSION "1.0"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0"
    )
endif()
