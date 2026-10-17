cmake_minimum_required(VERSION 3.22)
project(CloudsPlugin VERSION 0.3.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(APPLE)
  # Native Apple silicon by default; uncomment for a universal binary.
  # set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "")
  set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")
endif()

include(FetchContent)
FetchContent_Declare(JUCE
  GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
  GIT_TAG 8.0.6
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)

# Plugin formats: AU + VST3 (+ Standalone for quick testing).
set(FORMATS VST3 Standalone)
if(APPLE)
  list(APPEND FORMATS AU)
endif()

juce_add_plugin(Clouds
  COMPANY_NAME "particledots"
  PLUGIN_MANUFACTURER_CODE Ptdt
  PLUGIN_CODE Clou
  FORMATS ${FORMATS}
  PRODUCT_NAME "Unofficial Clouds"
  IS_SYNTH FALSE
  NEEDS_MIDI_INPUT TRUE
  NEEDS_MIDI_OUTPUT FALSE
  AU_MAIN_TYPE kAudioUnitType_MusicEffect   # lets Logic route MIDI to it as a "MIDI-controlled effect"
  COPY_PLUGIN_AFTER_BUILD TRUE)

# --- DSP core: vendored Clouds/stmlib code + our JUCE-free wrapper, built with plain flags ---
add_library(clouds_core STATIC
  src/CloudsEngine.cpp
  third_party/clouds/dsp/correlator.cc
  third_party/clouds/dsp/granular_processor.cc
  third_party/clouds/dsp/mu_law.cc
  third_party/clouds/dsp/pvoc/frame_transformation.cc
  third_party/clouds/dsp/pvoc/phase_vocoder.cc
  third_party/clouds/dsp/pvoc/stft.cc
  third_party/clouds/resources.cc
  third_party/stmlib/utils/random.cc
  third_party/stmlib/dsp/units.cc
  third_party/stmlib/dsp/atan.cc)
target_include_directories(clouds_core PUBLIC src third_party)
target_compile_definitions(clouds_core PRIVATE TEST=1)
set_target_properties(clouds_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
# Some upstream headers rely on <cstdio>/<cstdlib>/<cmath> being pulled in transitively.
if(MSVC)
  target_compile_options(clouds_core PRIVATE /FIcstdio /FIcstdlib /FIcmath /FIcstring)
else()
  target_compile_options(clouds_core PRIVATE "SHELL:-include cstdio" "SHELL:-include cstdlib" "SHELL:-include cmath" "SHELL:-include cstring" -Wno-unused-local-typedef)
endif()

# --- The plugin ---
target_sources(Clouds PRIVATE src/PluginProcessor.cpp src/PluginEditor.cpp)
target_compile_definitions(Clouds PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)
target_link_libraries(Clouds PRIVATE
  clouds_core
  juce::juce_audio_utils
  juce::juce_recommended_config_flags
  juce::juce_recommended_warning_flags)

# --- Headless test of the DSP wrapper:  cmake --build build --target engine_test && build/engine_test ---
add_executable(engine_test EXCLUDE_FROM_ALL src/engine_test.cpp)
target_link_libraries(engine_test PRIVATE clouds_core)

# --- Level tests of the output stage (Output Level + soft limiter) ---
#   cmake --build build --target level_test && build/level_test            (stage and engine, no JUCE)
#   cmake --build build --target proc_level_test && build/proc_level_test  (the real plug-in processor class, headless)
add_executable(level_test EXCLUDE_FROM_ALL tests/level_test.cpp)
target_link_libraries(level_test PRIVATE clouds_core)

add_executable(proc_level_test EXCLUDE_FROM_ALL tests/proc_level_test.cpp src/PluginProcessor.cpp src/PluginEditor.cpp)
target_compile_definitions(proc_level_test PRIVATE JucePlugin_Name="Unofficial Clouds" JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(proc_level_test PRIVATE
  clouds_core
  juce::juce_audio_processors
  juce::juce_recommended_config_flags)
