cmake_minimum_required(VERSION 3.21)

project(OnyxStudioOta VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS
    Core
    Gui
    Qml
    Quick
    QuickControls2
    Bluetooth
)

qt_standard_project_setup(REQUIRES 6.5)

qt_add_executable(onyx_ota_app
    src/main.cpp
    src/OtaProtocol.h
    src/OtaProtocol.cpp
    src/SpeakerDevice.h
    src/SpeakerScanner.h
    src/SpeakerScanner.cpp
    src/OtaTransport.h
    src/BleOtaTransport.h
    src/BleOtaTransport.cpp
    src/ClassicOtaTransport.h
    src/ClassicOtaTransport.cpp
    src/OtaController.h
    src/OtaController.cpp
)

qt_add_qml_module(onyx_ota_app
    URI OnyxOta
    VERSION 1.0
    QML_FILES
        qml/Main.qml
    RESOURCES
        assets/app_icon.png
)

target_link_libraries(onyx_ota_app PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Bluetooth
)

set_target_properties(onyx_ota_app PROPERTIES
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
)

if(ANDROID)
    set_target_properties(onyx_ota_app PROPERTIES
        QT_ANDROID_PACKAGE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/android"
        QT_ANDROID_PACKAGE_NAME "com.onyxstudio.ota"
        QT_ANDROID_VERSION_NAME "1.0.0"
        QT_ANDROID_VERSION_CODE 1
        QT_ANDROID_MIN_SDK_VERSION 31
        QT_ANDROID_TARGET_SDK_VERSION 35
    )
endif()
