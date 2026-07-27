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
# external/soundtouch 에 SoundTouch 소스(CMakeLists.txt 포함)를 받아 넣으면
# 자동으로 감지되어 링크된다. 아직 없으면 이 블록은 건너뛰고
# HAVE_SOUNDTOUCH=0 으로 빌드된다.
set(SOUNDTOUCH_DIR ${CMAKE_SOURCE_DIR}/external/soundtouch)
if(EXISTS ${SOUNDTOUCH_DIR}/CMakeLists.txt)
    # SoundTouch 2.3.3 still declares CMake 3.1 compatibility. Newer CMake
    # releases require an explicit policy floor for such dependencies.
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
    set(SOUNDSTRETCH OFF CACHE BOOL "" FORCE)
    set(SOUNDTOUCH_DLL OFF CACHE BOOL "" FORCE)
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
    src/mode1_vocal_follower/GuitarChordTracker.cpp
    src/mode1_vocal_follower/GuitarOnsetTracker.cpp
    src/mode1_vocal_follower/Mode1Controller.cpp
    src/mode1_vocal_follower/PhrasePlayer.cpp
    src/mode1_vocal_follower/PhraseScheduler.cpp
    src/mode1_vocal_follower/SongPackage.cpp
    src/voice_capture/RecordingQualityChecker.cpp
    src/voice_capture/VoiceRecorder.cpp
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
        juce::juce_audio_formats
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
else()
    target_compile_definitions(VocalGuitarApp PRIVATE HAVE_SOUNDTOUCH=0)
endif()

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
