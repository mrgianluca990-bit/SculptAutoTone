cmake_minimum_required(VERSION 3.22)
project(SculptAutoTone VERSION 1.3.5 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT APPLE)
  message(FATAL_ERROR "Sculpt Guitar Rig is currently macOS-only.")
endif()
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)
FetchContent_Declare(JUCE GIT_REPOSITORY https://github.com/juce-framework/JUCE.git GIT_TAG 8.0.10 GIT_SHALLOW TRUE)
FetchContent_Declare(NeuralAudioSource GIT_REPOSITORY https://github.com/mikeoliphant/NeuralAudio.git GIT_TAG main GIT_SUBMODULES_RECURSE TRUE)
FetchContent_MakeAvailable(JUCE NeuralAudioSource)

juce_add_gui_app(SculptAutoTone
    PRODUCT_NAME "Sculpt Guitar Rig"
    COMPANY_NAME "Sculpt Audio Lab"
    BUNDLE_ID "com.sculptaudiolab.autotone.beta"
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/Assets/AppIcon.png"
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "Sculpt Guitar Rig needs your audio interface input to analyze and process your guitar live. No guitar audio is saved."
)

juce_add_binary_data(SculptRigArtwork SOURCES Assets/RigArtwork.png Assets/AppIcon.png)

target_sources(SculptAutoTone PRIVATE Source/Main.cpp Source/VstChain.h Source/GuitarTuner.h)
target_include_directories(SculptAutoTone PRIVATE ${neuralaudiosource_SOURCE_DIR})
target_link_libraries(SculptAutoTone PRIVATE juce::juce_audio_utils juce::juce_audio_processors juce::juce_dsp NeuralAudio SculptRigArtwork
  juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
target_compile_definitions(SculptAutoTone PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_PLUGINHOST_VST3=1 JUCE_PLUGINHOST_VST=0)
if(CMAKE_BUILD_TYPE STREQUAL "Release")
  target_compile_options(SculptAutoTone PRIVATE -O3)
endif()
