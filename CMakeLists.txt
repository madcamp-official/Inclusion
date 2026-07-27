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

# --- WORLD (실시간 하이브리드 + 오프라인 음색 보존 기준선) ----------------
set(WORLD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WORLD_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(external/world)

# --- 피치 시프터 백엔드 --------------------------------------------------
# WORLD는 vendored 소스로 항상 빌드한다. Rubber Band와 SoundTouch는 A/B 및 폴백용이다.
# Rubber Band 시스템 패키지(Homebrew의 `brew install rubberband`,
# apt의 `librubberband-dev` 등)가 있으면 함께 활성화한다.
find_path(RUBBERBAND_INCLUDE_DIR
    NAMES rubberband/RubberBandStretcher.h
)
find_library(RUBBERBAND_LIBRARY
    NAMES rubberband
)

if(RUBBERBAND_INCLUDE_DIR AND RUBBERBAND_LIBRARY)
    add_library(RubberBand::RubberBand UNKNOWN IMPORTED)
    set_target_properties(RubberBand::RubberBand PROPERTIES
        IMPORTED_LOCATION "${RUBBERBAND_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${RUBBERBAND_INCLUDE_DIR}"
    )
    set(HAVE_RUBBERBAND TRUE)
    message(STATUS "Rubber Band found: ${RUBBERBAND_LIBRARY}")
else()
    set(HAVE_RUBBERBAND FALSE)
    message(STATUS "Rubber Band not found — falling back to SoundTouch when available.")
endif()

# SoundTouch는 Rubber Band와 녹음 A/B 비교를 위해 그대로 유지한다.
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

function(configure_pitch_shifter target)
    target_link_libraries(${target} PRIVATE world::core)
    target_compile_definitions(${target} PRIVATE HAVE_WORLD=1)
    if(HAVE_RUBBERBAND)
        target_link_libraries(${target} PRIVATE RubberBand::RubberBand)
        target_compile_definitions(${target} PRIVATE HAVE_RUBBERBAND=1)
    endif()
    if(HAVE_SOUNDTOUCH)
        target_link_libraries(${target} PRIVATE SoundTouch)
        target_compile_definitions(${target} PRIVATE HAVE_SOUNDTOUCH=1)
    endif()
endfunction()

# --- App target ---------------------------------------------------------
juce_add_gui_app(VocalGuitarApp
    PRODUCT_NAME "Vocal Guitar App"
    COMPANY_NAME "MadCamp"
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "기타/보컬 입력을 처리하려면 마이크(오디오 인터페이스) 접근 권한이 필요합니다."
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
    src/mode2_guitar_vocoder/GuitarBleedCanceller.cpp
    src/mode2_guitar_vocoder/GuitarTargetTracker.cpp
    src/mode2_guitar_vocoder/PitchShifterEngine.cpp
    src/mode2_guitar_vocoder/WorldRealtimePitchShifter.cpp
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

configure_pitch_shifter(VocalGuitarApp)

# --- Mode 2 오프라인 재현 도구 -------------------------------------------
# 앱이 녹음한 입력 WAV를 앱과 같은 DSP에 통과시켜, 파라미터만 바꿔가며 결과를 비교할 수 있게 한다.
juce_add_console_app(Mode2Offline
    PRODUCT_NAME "Mode2Offline"
)

target_sources(Mode2Offline PRIVATE
    tools/Mode2Offline.cpp
    tools/PitchAccuracyMetrics.cpp
    src/core/dsp/OnsetDetector.cpp
    src/core/dsp/PitchDetector.cpp
    src/core/dsp/PitchStabilizer.cpp
    src/core/dsp/NoiseGate.cpp
    src/core/audio/FeedbackCalibrator.cpp
    src/core/audio/FeedbackNotchSuppressor.cpp
    src/mode2_guitar_vocoder/CorrectionCalculator.cpp
    src/mode2_guitar_vocoder/GuitarBleedCanceller.cpp
    src/mode2_guitar_vocoder/GuitarTargetTracker.cpp
    src/mode2_guitar_vocoder/PitchShifterEngine.cpp
    src/mode2_guitar_vocoder/WorldRealtimePitchShifter.cpp
    src/mode2_guitar_vocoder/WorldVoiceTransformer.cpp
    src/mode2_guitar_vocoder/Mode2Controller.cpp
)

target_include_directories(Mode2Offline PRIVATE src tools)

target_compile_definitions(Mode2Offline PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
)

target_link_libraries(Mode2Offline
    PRIVATE
        juce::juce_audio_formats
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)

configure_pitch_shifter(Mode2Offline)

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
    src/mode2_guitar_vocoder/GuitarBleedCanceller.cpp
    src/mode2_guitar_vocoder/GuitarTargetTracker.cpp
    src/mode2_guitar_vocoder/PitchShifterEngine.cpp
    src/mode2_guitar_vocoder/WorldRealtimePitchShifter.cpp
)

target_include_directories(Mode2Tests PRIVATE src)

target_link_libraries(Mode2Tests
    PRIVATE
        juce::juce_core
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)

configure_pitch_shifter(Mode2Tests)
