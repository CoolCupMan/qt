cmake_minimum_required(VERSION 3.22)

project(WavRemixer VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(REMIX_BUILD_CLI "Also build the headless command-line renderer (RemixCli)" OFF)

# ---------------------------------------------------------------------------
# JUCE: use a local checkout if JUCE_DIR is given, otherwise fetch it.
# ---------------------------------------------------------------------------
set(JUCE_DIR "" CACHE PATH "Path to a local JUCE checkout (optional)")

if(JUCE_DIR)
    add_subdirectory(${JUCE_DIR} ${CMAKE_BINARY_DIR}/JUCE EXCLUDE_FROM_ALL)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        8.0.9
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

# Static MSVC runtime so the .exe and .vst3 run without a VC++ redistributable.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# ---------------------------------------------------------------------------
# Remix engine (shared by plugin, standalone and CLI)
# ---------------------------------------------------------------------------
set(REMIX_ENGINE_SOURCES
    Source/Engine/AudioLoader.cpp
    Source/Engine/Lexicon.cpp
    Source/Engine/RemixEngine.cpp
    Source/Engine/WavWriter.cpp)

# ---------------------------------------------------------------------------
# Plugin: one codebase -> VST3 + Standalone (.exe on Windows)
# ---------------------------------------------------------------------------
juce_add_plugin(WavRemixer
    COMPANY_NAME                "CoolCupMan"
    PRODUCT_NAME                "WAV Remixer"
    BUNDLE_ID                   "com.coolcupman.wavremixer"
    PLUGIN_MANUFACTURER_CODE    Ccmn
    PLUGIN_CODE                 Wrmx
    FORMATS                     VST3 Standalone
    IS_SYNTH                    FALSE
    NEEDS_MIDI_INPUT            FALSE
    NEEDS_MIDI_OUTPUT           FALSE
    IS_MIDI_EFFECT              FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD     FALSE
    VST3_CATEGORIES             Fx Generator Tools)

target_sources(WavRemixer PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    ${REMIX_ENGINE_SOURCES})

target_include_directories(WavRemixer PRIVATE Source)

target_compile_definitions(WavRemixer PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_USE_MP3AUDIOFORMAT=1
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(WavRemixer
    PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_formats
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# ---------------------------------------------------------------------------
# Optional headless renderer (handy for batch jobs and for testing the engine)
# ---------------------------------------------------------------------------
if(REMIX_BUILD_CLI)
    juce_add_console_app(RemixCli PRODUCT_NAME "RemixCli")
    target_sources(RemixCli PRIVATE Source/Cli/Main.cpp ${REMIX_ENGINE_SOURCES})
    target_include_directories(RemixCli PRIVATE Source)
    target_compile_definitions(RemixCli PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_USE_MP3AUDIOFORMAT=1)
    target_link_libraries(RemixCli
        PRIVATE
            juce::juce_audio_formats
            juce::juce_audio_basics
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)
endif()
