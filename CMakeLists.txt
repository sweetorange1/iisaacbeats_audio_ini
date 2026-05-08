cmake_minimum_required(VERSION 3.22)
project(Puponvst VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
FetchContent_Declare(
        juce
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.12
)
FetchContent_MakeAvailable(juce)

juce_add_plugin(
        Puponvst
        PRODUCT_NAME "Pupon"
        VERSION 0.1.0
        COMPANY_NAME "iisaacbeas.cn"
        BUNDLE_ID "cn.iisaacbeas.Pupon"
        PLUGIN_MANUFACTURER_CODE Pupo
        PLUGIN_CODE Pupn
        # 注意：AU 仅能在 macOS 上构建，其它平台会被 JUCE 自动忽略
        FORMATS VST3 AU Standalone
        IS_SYNTH FALSE
        NEEDS_MIDI_INPUT FALSE
        NEEDS_MIDI_OUTPUT FALSE
        VST3_AUTO_MANIFEST FALSE
        # AU 必需的插件分类（效果器）
        AU_MAIN_TYPE kAudioUnitType_Effect
        COPY_PLUGIN_AFTER_BUILD TRUE
)

juce_generate_juce_header(Puponvst)

target_compile_definitions(Puponvst
  PUBLIC
    JUCE_VST3_CAN_REPLACE_VST2=0
)

target_sources(Puponvst PRIVATE
  PluginProcessor.cpp
  PluginProcessor.h
  PluginEditor.cpp
  PluginEditor.h
)

target_link_libraries(Puponvst PRIVATE juce::juce_audio_utils)
