cmake_minimum_required(VERSION 3.22)

# Для macOS: универсальный бинарник (Intel + Apple Silicon). Должно стоять до project().
if(APPLE)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "" FORCE)
endif()

project(WebcamVST VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.13
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(JUCE)

juce_add_plugin(WebcamVST
    # Имя в DAW и имя файла (CAM.vst3), производитель в списке плагинов
    COMPANY_NAME "COTERIA"
    PRODUCT_NAME "CAM"
    PLUGIN_NAME "CAM"
    BUNDLE_ID "com.coteria.cam"
    PLUGIN_MANUFACTURER_CODE Cotr
    PLUGIN_CODE Cam1
    FORMATS VST3 Standalone
    VST3_CATEGORIES Fx
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    # Иконка Standalone-приложения (.ico / .icns JUCE делает из PNG сам)
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon.png"
    CAMERA_PERMISSION_ENABLED TRUE
    CAMERA_PERMISSION_TEXT "CAM needs access to the camera to show and record video."
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "CAM needs access to the microphone to record audio with the video."
)

target_sources(WebcamVST PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginProcessor.h
    Source/PluginEditor.cpp
    Source/PluginEditor.h
    Source/Recorder.cpp
    Source/Recorder.h
    Source/MediaWriters.h
    Source/Strings.h
    Source/HostAudio.h
    Source/HostProject.cpp
    Source/HostProject.h
    Source/FileNaming.h
    Source/AudioAligner.h
)

target_compile_definitions(WebcamVST PUBLIC
    JUCE_USE_CAMERA=1
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
)

# Исходники в UTF-8 (в них есть русские надписи): для MSVC это нужно указать явно
target_compile_options(WebcamVST PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
)

target_link_libraries(WebcamVST
    PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_devices
        juce::juce_video
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)
