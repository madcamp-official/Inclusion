#pragma once

#include "AudioLatencyCalibrator.h"
#include "RecordingSessionScreen.h"
#include "mode1_vocal_follower/Mode1Controller.h"
#include "mode2_guitar_vocoder/Mode2Controller.h"
#include "ui/GuitaruLookAndFeel.h"
#include "ui/mode2/Mode2Screen.h"
#include "voice_capture/GuidedRecordingSession.h"
#include "voice_capture/InputLevelCalibrator.h"
#include "voice_capture/VoiceModelTrainer.h"
#include "voice_capture/VoiceRecorder.h"

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include <atomic>
#include <array>
#include <memory>
#include <vector>

// 앱 진입점 — 오디오 디바이스를 열고, 랜딩 화면에서 모드 1 / 모드 2 화면을 전환한다.
// 랜딩 화면은 MainComponent가 직접 그리고, 모드 1(보컬 팔로워) UI는 mode1Panel 안에,
// 모드 2(기타 보코더) UI는 Mode2Screen 안에 들어 있다.
// 기타/목소리가 어느 입력 채널로 들어오는지는 장치 구성에 따라 다르므로(예: Scarlett Solo는
// INPUT 1=XLR 마이크, INPUT 2=기타 잭. 통합 기기를 쓰면 채널이 3개 이상) 화면에서 직접 고르고
// XML로 저장한다. 두 모드가 같은 매핑을 공유하므로 한쪽에서 고른 배선이 다른 쪽에도 적용된다.
class MainComponent : public juce::AudioAppComponent,
                      private juce::ChangeListener,
                      private juce::Timer
{
public:
    MainComponent();
    ~MainComponent() override;

    void prepareToPlay(int samplesPerBlockExpected, double sampleRate) override;
    void getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill) override;
    void releaseResources() override;

    void paint(juce::Graphics& graphics) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress& key) override;

    // 진단용 3채널 녹음(기타 입력 / 목소리 입력 / 최종 출력). 입력까지 함께 남기면 같은 연주로
    // 파라미터를 바꿔가며 오프라인 재현할 수 있어, 매번 다시 연주하지 않아도 된다.
    // 오디오 스레드에서 디스크를 만지면 안 되므로 JUCE의 ThreadedWriter를 쓴다.
    bool toggleRecording();
    bool isRecording() const;
    juce::File getLastRecordingFile() const { return recordingFile; }

private:
    enum class ActiveMode
    {
        landing,
        mode1,
        mode2,
    };

    void timerCallback() override;
    void changeListenerCallback(juce::ChangeBroadcaster* source) override;

    // --- 화면 전환 -------------------------------------------------------
    void showLanding();
    void showMode1();
    void showMode2();
    void layoutMode1Panel();
    void applyChalkStyleToMode1Controls();
    // 모드 1은 조정 손잡이가 많아 기본 화면이 금방 조잡해진다. 연주에 꼭 필요한
    // 것만 남기고 나머지는 접어 둔다.
    void collectAdvancedMode1Controls();
    void setAdvancedControlsVisible(bool shouldBeVisible);

    // --- 공통 오디오 장치 ------------------------------------------------
    void selectPreferredLowLatencyDevice();
    void configureLowLatencyAudio();
    void showAudioSettings();
    void updateAudioDeviceStatus();
    // 오디오 장치 선택은 기본적으로 이번 실행에만 적용되므로(macOS는 매번 시스템
    // 기본 장치로 되돌아감), 다음 실행에도 이어지도록 XML로 저장/복원한다.
    static juce::File getAudioSettingsFile();
    static juce::File getChannelMapFile();
    void saveAudioSettings();
    void saveChannelMap();
    void loadChannelMap();
    void refreshChannelChoices();

    // --- 모드 1 ----------------------------------------------------------
    void requestLatencyMeasurement();
    void beginLatencyMeasurement();
    void applyLatencyMeasurement();
    void resetLatencyCompensation();
    void refreshLatencyDisplay();
    bool latencyResultMatchesCurrentDevice() const;
    void setVirtualControlsEnabled(bool enabled);
    void refreshTransportControls();
    void startMode1Performance(bool restart);
    void stopMode1Performance();
    void startGuitarTestRecording();
    void stopGuitarTestRecording();
    void startGuitarTestReplay();
    void stopGuitarTestReplay();
    void refreshGuitarTestControls();
    bool loadGuitarTestReplay(const juce::File& file);
    juce::String getPerformanceRecordingSongSlug() const;
    juce::File getGuitarTestRecordingFile() const;
    juce::File getPerformanceOutputRecordingFile() const;
    juce::File getPerformanceMixRecordingFile() const;
    juce::File getPerformanceTraceFile() const;
    juce::File getPerformanceSessionFile() const;
    juce::Result savePerformanceDiagnostics();
    juce::Result createPerformanceMix(
        const juce::File& guitarFile,
        const juce::File& vocalFile,
        const juce::File& destination) const;
    void chooseSongPackage();
    bool loadSongPackage(const juce::File& file);
    juce::File findBundledSongPackage(const juce::String& songSlug) const;
    juce::File findDevelopmentSongPackage() const;
    void triggerNextPhrase();

    void startGuidedRecordingSession();
    void cancelGuidedRecordingSession();
    void finalizeGuidedItem(const voice_capture::GuidedRecordingState& state);
    juce::StringArray loadSongLyricLines() const;
    juce::File ensureProfileDirectory();
    juce::File findRepositoryRoot() const;
    void startVoiceModelTraining();
    juce::String getRecordingCategorySlug() const;
    void writeManifestEntry(const juce::File& rawFile,
                            const juce::File& acceptedFile,
                            const voice_capture::RecordingQuality& quality,
                            bool noiseReduced);

    // --- 모드 2 ----------------------------------------------------------
    void processMode2Block(const juce::AudioSourceChannelInfo& bufferToFill);
    void updateLatencyInfo();
    void selectPitchShifterBackend(PitchShifterEngine::Backend backend);
    static juce::File getRecordingsDirectory();
    void startRecording();
    void stopRecording();

    ActiveMode activeMode = ActiveMode::landing;
    double currentSampleRate = 48'000.0;

    GuitaruLookAndFeel guitaruLookAndFeel;
    juce::Image logoImage;
    juce::Image mascotImage;

    // 랜딩 화면(모드 선택).
    juce::TextButton mode1Button;
    juce::TextButton mode2Button;
    juce::TextButton audioSettingsButton;
    juce::Label statusLabel;

    // 채널 배선은 모드마다 다르다. 모드 1은 보통 오인페 하나만 쓰므로
    // 기타=악기잭, 마이크=XLR로 같은 장치의 두 입력을 쓰고, 모드 2는 기타와
    // 목소리를 서로 다른 하드웨어에서 받아야 해서 통합 기기의 세 번째 채널을
    // 쓰는 식이다. 한때 둘을 공유하게 했더니 모드 1에서 고른 배선이 모드 2의
    // 목소리 채널을 죽은 입력으로 덮어써서 보코더가 무음이 됐다. 따로 둔다.
    std::atomic<int> guitarChannelIndex { 0 };   // 모드 2
    std::atomic<int> vocalChannelIndex { 1 };    // 모드 2
    // 녹음에만 함께 담을 채널(-1 = 없음). 처리 경로에는 쓰지 않는다.
    std::atomic<int> roomChannelIndex { -1 };
    std::atomic<int> mode1GuitarChannelIndex { 1 };
    std::atomic<int> mode1MicChannelIndex { 0 };

    std::vector<float> guitarInputScratch;
    std::vector<float> vocalInputScratch;
    std::vector<float> roomInputScratch;

    // --- 모드 1 상태 -----------------------------------------------------
    // 모드 1 UI 전체를 한 패널에 담아 화면 전환 때 한 번에 보이고 숨긴다.
    // 패널은 배경을 그리지 않으므로 MainComponent가 그린 종이 질감이 그대로 비친다.
    juce::Component mode1Panel;
    juce::TextButton mode1HomeButton;
    juce::TextButton advancedToggleButton;
    // 접힌 상태가 기본이다. 펼침 여부는 화면 전환과 무관하게 유지된다.
    bool showAdvancedControls = false;
    std::vector<juce::Component*> advancedMode1Controls;

    std::atomic<float> liveGuitarPeak { 0.0f };
    std::atomic<float> liveMicrophonePeak { 0.0f };

    mode1::Mode1Controller mode1Controller;
    AudioLatencyCalibrator audioLatencyCalibrator;
    AudioLatencyCalibrator::Result lastLatencyResult;
    bool hasLatencyResult = false;
    std::atomic<bool> measuredLatencyApplied { false };
    std::atomic<double> effectiveInputLatencySeconds { 0.0 };
    std::atomic<double> effectiveOutputLatencySeconds { 0.0 };
    double reportedInputLatencySamples = 0.0;
    double reportedOutputLatencySamples = 0.0;
    double latencyMeasurementSampleRate = 0.0;
    int latencyMeasurementBufferSize = 0;
    juce::String latencyMeasurementDeviceName;
    voice_capture::VoiceRecorder voiceRecorder;
    voice_capture::VoiceRecorder guitarTestRecorder;
    voice_capture::VoiceRecorder performanceOutputRecorder;
    voice_capture::InputLevelCalibrator inputLevelCalibrator;
    voice_capture::GuidedRecordingSession guidedRecordingSession;
    voice_capture::VoiceModelTrainer voiceModelTrainer;
    bool trainingRequestedForSession = false;
    std::atomic<bool> recordingPreflightActive { false };
    float calibratedRoomToneDb = -100.0f;
    float calibratedInputGain = 1.0f;
    juce::File lastReferenceFile;
    juce::File profileDirectory;
    juce::Array<juce::var> manifestClips;
    int clipIndex = 0;
    int acceptedClipCount = 0;
    double acceptedDurationSeconds = 0.0;
    double profileProgressValue = 0.0;
    juce::String guidedStatusMessage;
    bool guidedStatusIsError = false;
    std::unique_ptr<juce::FileChooser> packageChooser;
    juce::AudioBuffer<float> guitarReplayAudio;
    int guitarReplayPosition = 0;
    std::atomic<bool> guitarReplayActive { false };
    std::atomic<bool> guitarReplayFinished { false };
    juce::File lastGuitarTestRecording;
    juce::File lastPerformanceOutputRecording;
    juce::File currentSongPackageFile;

    juce::ComboBox songSelector;
    juce::ComboBox followerModeSelector;
    juce::ToggleButton pauseOnWrongChordToggle {
        L"코드 오류 시 일시정지"
    };
    juce::ToggleButton followPerformanceTempoToggle {
        L"연주 속도에 보컬 맞춤"
    };
    juce::TextButton loadSongButton { L"곡 패키지 불러오기" };
    juce::TextButton nextPhraseButton { L"다음 코드 (Space)" };
    juce::TextButton startPerformanceButton { L"시작" };
    juce::TextButton restartPerformanceButton { L"재시작" };
    juce::TextButton stopPerformanceButton { L"중지" };
    juce::TextButton guitarRecordStartButton { L"기타 입력 녹음" };
    juce::TextButton guitarRecordStopButton { L"녹음 종료·저장" };
    juce::TextButton guitarReplayStartButton { L"녹음으로 테스트" };
    juce::TextButton guitarReplayStopButton { L"테스트 정지" };
    juce::TextButton mode1AudioSettingsButton { L"오디오 장치 설정" };
    juce::TextButton measureLatencyButton { L"레이턴시 측정" };
    juce::TextButton applyLatencyButton { L"측정값 적용" };
    juce::TextButton resetLatencyButton { L"기본값 복원" };
    juce::TextButton automaticPlaybackButton { L"자동 연주 시작" };
    juce::TextButton recordButton { L"가이드 녹음 시작" };
    std::array<juce::TextButton, 12> virtualChordButtons;
    juce::ComboBox guitarChannelSelector;
    juce::Slider expressionSlider;
    juce::Slider keyShiftSlider;
    juce::ComboBox microphoneChannelSelector;
    juce::ToggleButton noiseReductionToggle { L"자동 잡음 제거 (권장)" };
    juce::ProgressBar profileProgressBar { profileProgressValue };

    juce::Label mode1StatusLabel;
    juce::Label songLabel;
    juce::Label lyricLabel;
    juce::Label guitarStatusLabel;
    juce::Label latencyStatusLabel;
    juce::Label virtualChordLabel;
    juce::Label recordingTitleLabel;
    juce::Label profileProgressLabel;
    juce::Label recordingStatusLabel;
    juce::Label expressionLabel;
    juce::Label keyShiftLabel;

    RecordingSessionScreen recordingSessionScreen;

    // --- 모드 2 상태 -----------------------------------------------------
    Mode2Controller mode2Controller;
    Mode2Screen mode2Screen { mode2Controller };

    // 녹음은 파일 두 개를 동시에 쓴다. 둘 다 activeWriter를 오디오 스레드가 읽으므로
    // writerLock으로 함께 보호한다.
    //
    // 진단 파일: 기타 / 목소리 / 앱 출력 (+ 방 마이크나 유튜브). 원인을 짚으려면 처리 전
    //   입력이 남아 있어야 한다. 하지만 채널이 많아 QuickTime에서 바로 못 듣는다.
    // 듣기 파일: 보정된 목소리(앱 출력) + 4번째 채널. 그냥 재생하면 되는 스테레오다.
    //   변환 단계를 거치지 않고 바로 들을 수 있어야 연주 직후 판단이 빨라진다.
    juce::TimeSliceThread recorderThread { "VocalGuitarApp wav writer" };
    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> threadedWriter;
    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> listenWriter;
    juce::CriticalSection writerLock;
    juce::AudioFormatWriter::ThreadedWriter* activeWriter = nullptr;
    juce::AudioFormatWriter::ThreadedWriter* activeListenWriter = nullptr;
    juce::File recordingFile;
    juce::File listenFile;
    // 녹음 시작 시점에 정해진다. 방 마이크가 없으면 3, 있으면 4.
    int recordingChannelCount = 3;
    // 듣기 파일에 쓸 스테레오 스크래치(앱 출력 + 4번째 채널).
    std::vector<float> listenLeftScratch;
    std::vector<float> listenRightScratch;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};
