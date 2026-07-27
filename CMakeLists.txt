cmake_minimum_required(VERSION 3.22)

project(VocalGuitarApp VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- JUCE -------------------------------------------------------------
include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.15
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(JUCE)

# --- SoundTouch (모드 2 피치 시프터, B 담당) ----------------------------
# external/soundtouch 에 SoundTouch 소스(CMakeLists.txt 포함)가 있으면
# 자동으로 감지되어 링크된다. 없으면 이 블록은 건너뛰고 HAVE_SOUNDTOUCH=0 으로 빌드된다.
set(SOUNDTOUCH_DIR ${CMAKE_SOURCE_DIR}/external/soundtouch)
if(EXISTS ${SOUNDTOUCH_DIR}/CMakeLists.txt)
    set(SOUNDSTRETCH OFF CACHE BOOL "Build soundstretch command line utility." FORCE)
    add_subdirectory(${SOUNDTOUCH_DIR})
    set(HAVE_SOUNDTOUCH TRUE)
else()
    message(STATUS "SoundTouch not found under external/soundtouch — Mode 2 pitch shifting will build without it for now.")
    set(HAVE_SOUNDTOUCH FALSE)
endif()

# --- App target ---------------------------------------------------------
juce_add_gui_app(VocalGuitarApp
    PRODUCT_NAME "Vocal Guitar App"
    COMPANY_NAME "MadCamp"
)

target_sources(VocalGuitarApp PRIVATE
    src/main/Main.cpp
    src/main/MainComponent.cpp
    src/core/dsp/OnsetDetector.cpp
    src/core/dsp/PitchDetector.cpp
    src/core/dsp/PitchStabilizer.cpp
    src/core/dsp/NoiseGate.cpp
    src/core/audio/FeedbackCalibrator.cpp
    src/core/audio/FeedbackNotchSuppressor.cpp
    src/mode2_guitar_vocoder/CorrectionCalculator.cpp
    src/mode2_guitar_vocoder/GuitarTargetTracker.cpp
    src/mode2_guitar_vocoder/PitchShifterEngine.cpp
    src/mode2_guitar_vocoder/Mode2Controller.cpp
    src/ui/mode2/Mode2Screen.cpp
)

target_include_directories(VocalGuitarApp PRIVATE
    src
)

target_compile_definitions(VocalGuitarApp
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:VocalGuitarApp,JUCE_PRODUCT_NAME>"
        JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:VocalGuitarApp,JUCE_VERSION>"
)

target_link_libraries(VocalGuitarApp
    PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_devices
        juce::juce_dsp
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

if(HAVE_SOUNDTOUCH)
    target_link_libraries(VocalGuitarApp PRIVATE SoundTouch)
    target_compile_definitions(VocalGuitarApp PRIVATE HAVE_SOUNDTOUCH=1)
endif()

# --- Mode 2 unit tests ---------------------------------------------------
juce_add_console_app(Mode2Tests
    PRODUCT_NAME "Mode2Tests"
)

target_sources(Mode2Tests PRIVATE
    tests/mode2/Mode2Tests.cpp
    src/core/dsp/OnsetDetector.cpp
    src/core/dsp/PitchDetector.cpp
    src/core/dsp/PitchStabilizer.cpp
    src/core/dsp/NoiseGate.cpp
    src/mode2_guitar_vocoder/CorrectionCalculator.cpp
    src/mode2_guitar_vocoder/GuitarTargetTracker.cpp
    src/mode2_guitar_vocoder/PitchShifterEngine.cpp
)

target_include_directories(Mode2Tests PRIVATE src)

target_link_libraries(Mode2Tests
    PRIVATE
        juce::juce_core
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)
