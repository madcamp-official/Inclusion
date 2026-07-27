#pragma once

#include "mode1_vocal_follower/Mode1Controller.h"
#include "voice_capture/VoiceRecorder.h"

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include <atomic>
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
    void selectMode1();
    void chooseSongPackage();
    bool loadSongPackage(const juce::File& file);
    juce::File findDevelopmentSongPackage() const;
    void triggerNextPhrase();

    void startReferenceRecording();
    void stopReferenceRecording();
    juce::File makeReferenceFile() const;

    ActiveMode activeMode = ActiveMode::idle;
    double currentSampleRate = 48'000.0;
    std::vector<float> guitarInputScratch;
    std::atomic<int> guitarChannelIndex { 0 };

    mode1::Mode1Controller mode1Controller;
    voice_capture::VoiceRecorder voiceRecorder;
    juce::File lastReferenceFile;
    std::unique_ptr<juce::FileChooser> packageChooser;

    juce::TextButton mode1Button { "Mode 1: Vocal Follower" };
    juce::TextButton mode2Button { "Mode 2: Guitar Vocoder" };
    juce::TextButton loadSongButton { "Load song package" };
    juce::TextButton nextPhraseButton { "Trigger next phrase (Space)" };
    juce::TextButton recordButton { "Record voice profile" };
    juce::ComboBox guitarChannelSelector;

    juce::Label statusLabel;
    juce::Label songLabel;
    juce::Label lyricLabel;
    juce::Label guitarStatusLabel;
    juce::Label recordingStatusLabel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};
