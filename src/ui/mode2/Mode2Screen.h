#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include "mode2_guitar_vocoder/Mode2Controller.h"

// 사양서 9절 S6 화면: 기타 음정 / 내 목소리 음정 / 보정 후 음정 / 추종도.
class Mode2Screen : public juce::Component, private juce::Timer
{
public:
    explicit Mode2Screen(Mode2Controller& controllerIn);
    ~Mode2Screen() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

    // 기타/목소리가 들어오는 입력 채널을 화면에서 직접 고른다. 장치 구성에 따라 채널 배치가
    // 달라지므로(Scarlett Solo는 INPUT 1=XLR, INPUT 2=기타 잭. 통합 기기는 채널 3개 이상)
    // 하드코딩하지 않는다.
    std::function<void(int guitarChannel, int vocalChannel)> onInputChannelsChanged;
    void setAvailableInputChannels(int numChannels, int guitarChannel, int vocalChannel);

    // 진단 녹음 토글. 눌리면 호출자가 실제 녹음 상태를 돌려준다(true = 녹음 중).
    std::function<bool()> onToggleRecording;

    // 하드웨어 왕복 지연을 화면에 표시한다. 블루투스 출력(에어팟 등)은 여기서 수백 ms로
    // 드러나므로, 연주감을 평가할 수 있는 환경인지 바로 판단할 수 있다.
    void setLatencyInfo(double inputMs, double outputMs, double processingMs);

private:
    void timerCallback() override;
    static juce::String midiToDisplayString(float midi);

    Mode2Controller& controller;

    juce::Label guitarPitchLabel;
    juce::Label vocalPitchLabel;
    juce::Label correctedPitchLabel;
    juce::Label followLabel;
    double followProgress = 0.0;
    juce::ProgressBar followProgressBar { followProgress };
    juce::TextButton calibrateButton { juce::String(juce::CharPointer_UTF8("캘리브레이션 시작")) };
    juce::TextButton recordButton;

    // 피치 검출 신뢰도와 무관하게, 실제로 오디오 신호가 채널에 들어오는지 확인하기 위한
    // 원시 입력 레벨 미터(하드웨어/채널 배선 확인용).
    juce::Label guitarLevelLabel;
    juce::Label vocalLevelLabel;
    double guitarLevelProgress = 0.0;
    double vocalLevelProgress = 0.0;
    juce::ProgressBar guitarLevelBar { guitarLevelProgress };
    juce::ProgressBar vocalLevelBar { vocalLevelProgress };

    juce::Label channelMapLabel;
    juce::ComboBox guitarChannelBox;
    juce::ComboBox vocalChannelBox;

    juce::Label outputLevelLabel;
    double outputLevelProgress = 0.0;
    juce::ProgressBar outputLevelBar { outputLevelProgress };

    // 목표 음 옥타브 이동. 기타 음역과 노래 음역이 옥타브 단위로 다른 게 보통이라 화면에서
    // 바로 맞출 수 있게 둔다(0 = 기타 음 그대로).
    juce::Label targetOctaveLabel;
    juce::Slider targetOctaveSlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };

    // 글라이드(포르타멘토) 속도. 낮추면 음 사이를 미끄러져 레가토처럼 이어진다.
    juce::Label glideLabel;
    juce::Slider glideSlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };

    // 기타 유입 상쇄 토글과, 지금 목소리 채널에서 몇 dB를 걷어내고 있는지의 표시.
    // 제거량은 노래를 쉬고 기타만 칠 때 읽어야 실제 값이 보인다(노래 중에는 목소리가
    // 분모에 남아 0에 가깝게 나온다).
    juce::ToggleButton bleedCancelButton;
    juce::Label bleedCancelStatusLabel;

    juce::Label vocalGainLabel;
    juce::Slider vocalGainSlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
    juce::Label outputVolumeLabel;
    juce::Slider outputVolumeSlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
    juce::TextButton resetCalibrationButton;
    juce::ToggleButton howlGuardButton;
    juce::Label notchLabel;
    juce::Label latencyLabel;

    juce::ToggleButton noiseGateButton;
    juce::Slider noiseGateSlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };

    // 출력 하울링 트립 게이트 임계(하울링 억제 토글에 종속, 값만 별도 조절).
    juce::Label outputTripLabel;
    juce::Slider outputTripSlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Mode2Screen)
};
