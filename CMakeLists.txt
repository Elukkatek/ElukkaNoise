cmake_minimum_required(VERSION 3.22)
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "")  # universal: Live 10 is Intel-only
project(ElukkaNoise VERSION 0.9.26)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Windows: link the C++ runtime statically so the plugin does not depend on a VC++ redistributable.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if(APPLE)
    set(ELUKKA_COPY_PLUGIN_DEFAULT ON)
else()
    set(ELUKKA_COPY_PLUGIN_DEFAULT OFF)
endif()
option(ELUKKA_COPY_PLUGIN_AFTER_BUILD "Copy the VST3 into the user's plugin folder after building" ${ELUKKA_COPY_PLUGIN_DEFAULT})

add_subdirectory(JUCE)

juce_add_plugin(ElukkaNoise
    COMPANY_NAME "Elukka"
    PRODUCT_NAME "ElukkaNoise"
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD ${ELUKKA_COPY_PLUGIN_AFTER_BUILD}
    PLUGIN_MANUFACTURER_CODE Elkk
    PLUGIN_CODE Elkn
    VST3_CATEGORIES Instrument Synth
    FORMATS VST3)

juce_generate_juce_header(ElukkaNoise)

juce_add_binary_data(ElukkaNoiseData SOURCES Resources/logo.png)

set(ELUKKA_SOURCES
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/Presets.cpp
    Source/FxPresets.cpp
    Source/DSP/Engine.cpp
    Source/UI/Pages.cpp)
if(APPLE)
    list(APPEND ELUKKA_SOURCES Source/UI/HostWindow.mm)
endif()

target_sources(ElukkaNoise PRIVATE ${ELUKKA_SOURCES})

set(ELUKKA_DEFS
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)
target_compile_definitions(ElukkaNoise PUBLIC ${ELUKKA_DEFS})

target_link_libraries(ElukkaNoise
    PRIVATE
        ElukkaNoiseData
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# Developer-only test harness: offline renders, click detection, state round trips, CPU benchmark, host-load check.
option(ELUKKA_BUILD_TESTS "Build the offline test program" ON)
if(ELUKKA_BUILD_TESTS)
    juce_add_console_app(ElukkaNoiseTest PRODUCT_NAME "ElukkaNoiseTest")
    juce_generate_juce_header(ElukkaNoiseTest)
    target_sources(ElukkaNoiseTest PRIVATE Tests/RenderTest.cpp ${ELUKKA_SOURCES})
    target_compile_definitions(ElukkaNoiseTest PRIVATE
        ${ELUKKA_DEFS} JUCE_PLUGINHOST_VST3=1 JUCE_MODAL_LOOPS_PERMITTED=1 JucePlugin_Name="ElukkaNoise"
        JucePlugin_IsSynth=1 JucePlugin_WantsMidiInput=1 JucePlugin_ProducesMidiOutput=0
        JucePlugin_IsMidiEffect=0 JucePlugin_EditorRequiresKeyboardFocus=0)
    target_link_libraries(ElukkaNoiseTest PRIVATE
        ElukkaNoiseData
        juce::juce_audio_utils
        juce::juce_dsp
        juce::juce_recommended_config_flags)
endif()
