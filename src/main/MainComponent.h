#pragma once

#include "AudioLatencyCalibrator.h"
#include "RecordingSessionScreen.h"
#include "mode1_vocal_follower/Mode1Controller.h"
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

class MainComponent : public juce::AudioAppComponent,
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

private:
    enum class ActiveMode
    {
        idle,
        mode1,
        mode2Placeholder,
    };

    void timerCallback() override;
    void selectPreferredLowLatencyDevice();
    void configureLowLatencyAudio();
    void showAudioSettings();
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
    void selectMode1();
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

    ActiveMode activeMode = ActiveMode::idle;
    double currentSampleRate = 48'000.0;
    std::vector<float> guitarInputScratch;
    // Scarlett Solo exposes the microphone on input 1 and INST on input 2.
    std::atomic<int> guitarChannelIndex { 1 };
    std::atomic<int> microphoneChannelIndex { 0 };
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

    juce::TextButton mode1Button { "Mode 1: Vocal Follower" };
    juce::TextButton mode2Button { "Mode 2: Guitar Vocoder" };
    juce::ComboBox songSelector;
    juce::ComboBox followerModeSelector;
    juce::ToggleButton pauseOnWrongChordToggle {
        L"코드 오류 시 일시정지"
    };
    juce::ToggleButton followPerformanceTempoToggle {
        L"연주 속도에 보컬 맞춤"
    };
    juce::TextButton loadSongButton { "Load song package" };
    juce::TextButton nextPhraseButton { L"다음 코드 (Space)" };
    juce::TextButton startPerformanceButton { L"시작" };
    juce::TextButton restartPerformanceButton { L"재시작" };
    juce::TextButton stopPerformanceButton { L"중지" };
    juce::TextButton guitarRecordStartButton { L"기타 입력 녹음" };
    juce::TextButton guitarRecordStopButton { L"녹음 종료·저장" };
    juce::TextButton guitarReplayStartButton { L"녹음으로 테스트" };
    juce::TextButton guitarReplayStopButton { L"테스트 정지" };
    juce::TextButton audioSettingsButton { L"오디오 / ASIO 설정" };
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

    juce::Label statusLabel;
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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};
