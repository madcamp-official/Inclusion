#pragma once

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
    void configureLowLatencyAudio();
    void showAudioSettings();
    void setVirtualControlsEnabled(bool enabled);
    void selectMode1();
    void chooseSongPackage();
    bool loadSongPackage(const juce::File& file);
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
    std::atomic<int> guitarChannelIndex { 0 };
    std::atomic<int> microphoneChannelIndex { 0 };
    std::atomic<float> liveMicrophonePeak { 0.0f };

    mode1::Mode1Controller mode1Controller;
    voice_capture::VoiceRecorder voiceRecorder;
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

    juce::TextButton mode1Button { "Mode 1: Vocal Follower" };
    juce::TextButton mode2Button { "Mode 2: Guitar Vocoder" };
    juce::TextButton loadSongButton { "Load song package" };
    juce::TextButton nextPhraseButton { "Trigger next phrase (Space)" };
    juce::TextButton audioSettingsButton { L"오디오 / ASIO 설정" };
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
    juce::Label virtualChordLabel;
    juce::Label recordingTitleLabel;
    juce::Label profileProgressLabel;
    juce::Label recordingStatusLabel;
    juce::Label expressionLabel;
    juce::Label keyShiftLabel;

    RecordingSessionScreen recordingSessionScreen;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};
