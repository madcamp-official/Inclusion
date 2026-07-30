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
    # SoundTouch 2.3.3 still declares CMake 3.1 compatibility. Newer CMake
    # releases require an explicit policy floor for such dependencies.
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
    set(SOUNDSTRETCH OFF CACHE BOOL "Build soundstretch command line utility." FORCE)
    set(SOUNDTOUCH_DLL OFF CACHE BOOL "" FORCE)
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
    else()
        target_compile_definitions(${target} PRIVATE HAVE_RUBBERBAND=0)
    endif()
    # 모드 1 쪽 코드는 `#if HAVE_SOUNDTOUCH`로 분기하므로 없을 때도 0으로 정의해 둔다.
    if(HAVE_SOUNDTOUCH)
        target_link_libraries(${target} PRIVATE SoundTouch)
        target_compile_definitions(${target} PRIVATE HAVE_SOUNDTOUCH=1)
    else()
        target_compile_definitions(${target} PRIVATE HAVE_SOUNDTOUCH=0)
    endif()
endfunction()

# --- App target ---------------------------------------------------------
juce_add_gui_app(VocalGuitarApp
    PRODUCT_NAME "Vocal Guitar App"
    COMPANY_NAME "MadCamp"
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "기타/보컬 입력을 처리하려면 마이크(오디오 인터페이스) 접근 권한이 필요합니다."
)

juce_add_binary_data(GuitaruAssets
    SOURCES
        assets/branding/guitaru-logo-centered.png
        assets/branding/guitaru-mascot.png
        assets/fonts/Gaegu-Bold.ttf
)

target_sources(VocalGuitarApp PRIVATE
    src/main/Main.cpp
    src/main/MainComponent.cpp
    src/ui/GuitaruLookAndFeel.cpp
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
    src/main/AudioLatencyCalibrator.cpp
    src/mode1_vocal_follower/GuitarChordTracker.cpp
    src/mode1_vocal_follower/GuitarOnsetTracker.cpp
    src/mode1_vocal_follower/BeatClock.cpp
    src/mode1_vocal_follower/PredictiveTransport.cpp
    src/mode1_vocal_follower/IntroChromaAligner.cpp
    src/mode1_vocal_follower/ContinuousChromaExtractor.cpp
    src/mode1_vocal_follower/PredictivePhraseScheduler.cpp
    src/mode1_vocal_follower/Mode1Controller.cpp
    src/mode1_vocal_follower/PhrasePlayer.cpp
    src/mode1_vocal_follower/PhraseScheduler.cpp
    src/mode1_vocal_follower/SongPackage.cpp
    src/voice_capture/RecordingQualityChecker.cpp
    src/voice_capture/InputLevelCalibrator.cpp
    src/voice_capture/NoiseReducer.cpp
    src/voice_capture/VoiceRecorder.cpp
    src/voice_capture/VoiceActivityTracker.cpp
    src/voice_capture/GuidedRecordingSession.cpp
    src/voice_capture/VoiceModelTrainer.cpp
    src/main/RecordingSessionScreen.cpp
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

if(WIN32)
    target_compile_definitions(VocalGuitarApp PRIVATE JUCE_ASIO=1)
endif()

target_link_libraries(VocalGuitarApp
    PRIVATE
        GuitaruAssets
        juce::juce_audio_utils
        juce::juce_audio_devices
        juce::juce_audio_formats
        juce::juce_dsp
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

configure_pitch_shifter(VocalGuitarApp)

if(MSVC)
    # 소스에 한글 주석이 섞여 있으면 MSVC가 기본 코드페이지(949)로 오해석해
    # 파싱이 깨지는 문제가 있어 UTF-8로 강제 해석하도록 지정
    target_compile_options(VocalGuitarApp PRIVATE /utf-8)
endif()

# --- Mode 1 core integration test --------------------------------------
enable_testing()

juce_add_console_app(Mode1CoreTests
    PRODUCT_NAME "Mode 1 Core Tests"
)

target_sources(Mode1CoreTests PRIVATE
    tests/mode1/Mode1CoreTests.cpp
    src/mode1_vocal_follower/GuitarChordTracker.cpp
    src/mode1_vocal_follower/GuitarOnsetTracker.cpp
    src/mode1_vocal_follower/BeatClock.cpp
    src/mode1_vocal_follower/PredictiveTransport.cpp
    src/mode1_vocal_follower/IntroChromaAligner.cpp
    src/mode1_vocal_follower/ContinuousChromaExtractor.cpp
    src/mode1_vocal_follower/PredictivePhraseScheduler.cpp
    src/mode1_vocal_follower/Mode1Controller.cpp
    src/mode1_vocal_follower/PhrasePlayer.cpp
    src/mode1_vocal_follower/PhraseScheduler.cpp
    src/mode1_vocal_follower/SongPackage.cpp
)

target_include_directories(Mode1CoreTests PRIVATE src)

target_compile_definitions(Mode1CoreTests
    PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
)

target_link_libraries(Mode1CoreTests
    PRIVATE
        juce::juce_audio_formats
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)

if(HAVE_SOUNDTOUCH)
    target_link_libraries(Mode1CoreTests PRIVATE SoundTouch)
    target_compile_definitions(Mode1CoreTests PRIVATE HAVE_SOUNDTOUCH=1)
else()
    target_compile_definitions(Mode1CoreTests PRIVATE HAVE_SOUNDTOUCH=0)
endif()

if(MSVC)
    target_compile_options(Mode1CoreTests PRIVATE /utf-8)
endif()

add_test(
    NAME Mode1CoreTests
    COMMAND Mode1CoreTests
)

# --- Voice capture quality/noise reduction test ------------------------
juce_add_console_app(VoiceCaptureTests
    PRODUCT_NAME "Voice Capture Tests"
)

target_sources(VoiceCaptureTests PRIVATE
    tests/voice_capture/VoiceCaptureTests.cpp
    src/voice_capture/NoiseReducer.cpp
    src/voice_capture/RecordingQualityChecker.cpp
    src/voice_capture/InputLevelCalibrator.cpp
    src/voice_capture/VoiceRecorder.cpp
    src/voice_capture/VoiceActivityTracker.cpp
    src/voice_capture/GuidedRecordingSession.cpp
)

target_include_directories(VoiceCaptureTests PRIVATE src)
target_link_libraries(VoiceCaptureTests
    PRIVATE
        juce::juce_audio_basics
        juce::juce_audio_formats
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)

if(MSVC)
    target_compile_options(VoiceCaptureTests PRIVATE /utf-8)
endif()

add_test(
    NAME VoiceCaptureTests
    COMMAND VoiceCaptureTests
)

# --- MIDI Splitter tool (곡 데이터 제작용 — SynthV 등의 트랙당 노트 수 제한 우회) ---
juce_add_console_app(MidiSplitter
    PRODUCT_NAME "MIDI Splitter"
)

target_sources(MidiSplitter PRIVATE
    tools/midi_splitter/Main.cpp
)

target_link_libraries(MidiSplitter
    PRIVATE
        juce::juce_core
        juce::juce_audio_basics
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)

if(MSVC)
    target_compile_options(MidiSplitter PRIVATE /utf-8)
endif()

# --- WAV Splitter tool (MidiSplitter가 남긴 boundaries.csv로 원본 보컬 WAV를 같은 지점에서 자름) ---
juce_add_console_app(WavSplitter
    PRODUCT_NAME "WAV Splitter"
)

target_sources(WavSplitter PRIVATE
    tools/wav_splitter/Main.cpp
)

target_link_libraries(WavSplitter
    PRIVATE
        juce::juce_core
        juce::juce_audio_basics
        juce::juce_audio_formats
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)

if(MSVC)
    target_compile_options(WavSplitter PRIVATE /utf-8)
endif()

# --- Mode 1 offline guitar/replay renderer -----------------------------
juce_add_console_app(Mode1OfflineRenderer
    PRODUCT_NAME "Mode 1 Offline Renderer"
)

target_sources(Mode1OfflineRenderer PRIVATE
    tools/mode1_offline_renderer/Main.cpp
    src/mode1_vocal_follower/GuitarChordTracker.cpp
    src/mode1_vocal_follower/GuitarOnsetTracker.cpp
    src/mode1_vocal_follower/BeatClock.cpp
    src/mode1_vocal_follower/PredictiveTransport.cpp
    src/mode1_vocal_follower/IntroChromaAligner.cpp
    src/mode1_vocal_follower/ContinuousChromaExtractor.cpp
    src/mode1_vocal_follower/PredictivePhraseScheduler.cpp
    src/mode1_vocal_follower/Mode1Controller.cpp
    src/mode1_vocal_follower/PhrasePlayer.cpp
    src/mode1_vocal_follower/PhraseScheduler.cpp
    src/mode1_vocal_follower/SongPackage.cpp
)

target_include_directories(Mode1OfflineRenderer PRIVATE src)

target_compile_definitions(Mode1OfflineRenderer
    PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
)

target_link_libraries(Mode1OfflineRenderer
    PRIVATE
        juce::juce_audio_formats
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)

if(HAVE_SOUNDTOUCH)
    target_link_libraries(Mode1OfflineRenderer PRIVATE SoundTouch)
    target_compile_definitions(Mode1OfflineRenderer PRIVATE HAVE_SOUNDTOUCH=1)
else()
    target_compile_definitions(Mode1OfflineRenderer PRIVATE HAVE_SOUNDTOUCH=0)
endif()

if(MSVC)
    target_compile_options(Mode1OfflineRenderer PRIVATE /utf-8)
endif()

# --- Generic audio decoder for timing-analysis fixtures ----------------
juce_add_console_app(AudioToWav
    PRODUCT_NAME "Audio To WAV"
)

target_sources(AudioToWav PRIVATE
    tools/audio_to_wav/Main.cpp
)

target_compile_definitions(AudioToWav
    PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
)

target_link_libraries(AudioToWav
    PRIVATE
        juce::juce_audio_formats
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)

if(MSVC)
    target_compile_options(AudioToWav PRIVATE /utf-8)
endif()

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

if(MSVC)
    target_compile_options(Mode2Offline PRIVATE /utf-8)
endif()

# --- macOS 통합 오디오 기기 도구 -----------------------------------------
# 기타(오디오 인터페이스)와 목소리(다른 마이크)를 동시에 받으려면 통합 기기가 필요하다.
# 손으로 만들면 구성이 사람마다 달라져 채널 번호가 어긋나므로 도구로 재현한다.
# CoreAudio만 쓰는 순수 C 타깃이라 JUCE를 링크하지 않는다.
if(APPLE)
    add_executable(Mode2AudioDevice tools/AggregateDevice.c)
    target_link_libraries(Mode2AudioDevice PRIVATE "-framework CoreAudio" "-framework CoreFoundation")
    # AudioHardwareCreate/DestroyAggregateDevice는 deprecated로 표시돼 있지만 대체 API가 없다.
    target_compile_options(Mode2AudioDevice PRIVATE -Wno-deprecated-declarations)
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

if(MSVC)
    target_compile_options(Mode2Tests PRIVATE /utf-8)
endif()

add_test(
    NAME Mode2Tests
    COMMAND Mode2Tests
)

# --- 기타 목표 음정 프로브 --------------------------------------------------
# 같은 WAV에 대해 앱이 실제로 보는 목표 음정을 찍어, 다른 분석 결과와 대조한다.
juce_add_console_app(GuitarTargetProbe
    PRODUCT_NAME "GuitarTargetProbe"
)

target_sources(GuitarTargetProbe PRIVATE
    tools/guitar_target_probe/Main.cpp
    src/core/dsp/OnsetDetector.cpp
    src/core/dsp/PitchDetector.cpp
    src/core/dsp/PitchStabilizer.cpp
    src/mode2_guitar_vocoder/GuitarTargetTracker.cpp
)

target_include_directories(GuitarTargetProbe PRIVATE src)

target_compile_definitions(GuitarTargetProbe PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
)

target_link_libraries(GuitarTargetProbe
    PRIVATE
        juce::juce_audio_formats
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)

configure_pitch_shifter(GuitarTargetProbe)
