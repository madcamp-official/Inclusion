#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_extra/juce_gui_extra.h>

// 앱 진입점 — 오디오 디바이스를 열고, 모드 1 / 모드 2 화면을 전환한다.
// 실제 DSP 배선은 src/mode1_vocal_follower, src/mode2_guitar_vocoder 쪽 컨트롤러가
// 준비되면 여기서 연결한다 (지금은 무음 패스스루 스켈레톤).
class MainComponent : public juce::AudioAppComponent
{
public:
    MainComponent();
    ~MainComponent() override;

    void prepareToPlay(int samplesPerBlockExpected, double sampleRate) override;
    void getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill) override;
    void releaseResources() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    juce::TextButton mode1Button { "Mode 1: Vocal Follower" };
    juce::TextButton mode2Button { "Mode 2: Guitar Vocoder" };
    juce::Label statusLabel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};
