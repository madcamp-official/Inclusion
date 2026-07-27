#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include "mode2_guitar_vocoder/Mode2Controller.h"
#include "ui/mode2/Mode2Screen.h"

#include <atomic>
#include <vector>

// 앱 진입점 — 오디오 디바이스를 열고, 모드 1 / 모드 2 화면을 전환한다.
// 입력 채널 0=기타, 1=마이크 목소리로 가정한다(모드 2 사양서 PART 1 1절).
// macOS 기본 입력 장치가 1채널(내장 마이크)인 경우가 많아, 앱 안에서 오디오 장치를
// 직접 고를 수 있는 설정 화면을 제공한다(예: 2채널 오디오 인터페이스로 전환).
class MainComponent : public juce::AudioAppComponent, private juce::ChangeListener
{
public:
    MainComponent();
    ~MainComponent() override;

    void prepareToPlay(int samplesPerBlockExpected, double sampleRate) override;
    void getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill) override;
    void releaseResources() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

public:
    // 진단용 3채널 녹음(기타 입력 / 목소리 입력 / 최종 출력). 입력까지 함께 남기면 같은 연주로
    // 파라미터를 바꿔가며 오프라인 재현할 수 있어, 매번 다시 연주하지 않아도 된다.
    // 오디오 스레드에서 디스크를 만지면 안 되므로 JUCE의 ThreadedWriter를 쓴다.
    bool toggleRecording();
    bool isRecording() const;
    juce::File getLastRecordingFile() const { return recordingFile; }

private:
    void showMode2();
    void showAudioSettings();
    void updateAudioDeviceStatus();
    void changeListenerCallback(juce::ChangeBroadcaster* source) override;

    // 오디오 장치 선택은 기본적으로 이번 실행에만 적용되므로(macOS는 매번 시스템
    // 기본 장치로 되돌아감), 다음 실행에도 이어지도록 XML로 저장/복원한다.
    static juce::File getAudioSettingsFile();
    static juce::File getChannelMapFile();
    void saveAudioSettings();
    void saveChannelMap();
    void loadChannelMap();
    void refreshChannelChoices();
    void updateLatencyInfo();

    juce::TextButton mode1Button { "Mode 1: Vocal Follower" };
    juce::TextButton mode2Button { "Mode 2: Guitar Vocoder" };
    juce::TextButton audioSettingsButton;
    juce::Label statusLabel;

    Mode2Controller mode2Controller;
    Mode2Screen mode2Screen { mode2Controller };
    bool mode2Active = false;

    // 기타/목소리가 어느 입력 채널로 들어오는지는 장치 구성에 따라 다르므로(예: Scarlett Solo는
    // INPUT 1=XLR 마이크, INPUT 2=기타 잭. 통합 기기를 쓰면 채널이 3개 이상) 사용자가 직접 고른다.
    std::atomic<int> guitarChannelIndex { 0 };
    std::atomic<int> vocalChannelIndex { 1 };

    std::vector<float> guitarInputScratch;
    std::vector<float> vocalInputScratch;

    // 진단 녹음. activeWriter는 오디오 스레드가 읽으므로 writerLock으로 보호한다.
    juce::TimeSliceThread recorderThread { "VocalGuitarApp wav writer" };
    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> threadedWriter;
    juce::CriticalSection writerLock;
    juce::AudioFormatWriter::ThreadedWriter* activeWriter = nullptr;
    juce::File recordingFile;
    double currentSampleRate = 48000.0;
    static juce::File getRecordingsDirectory();
    void startRecording();
    void stopRecording();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};
