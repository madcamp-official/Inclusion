#include "Mode2Screen.h"
#include "params/Mode2Params.h"

namespace
{
    // 8비트 리터럴을 ASCII로 오해하는 juce::String(const char*) 대신 UTF-8로 명시 변환한다.
    juce::String utf8(const char* text)
    {
        return juce::String(juce::CharPointer_UTF8(text));
    }

    // 원시 RMS를 0~1 미터 값으로 변환. 게이트 임계 슬라이더도 같은 눈금을 쓴다.
    double levelToMeterValue(float rms)
    {
        return juce::jlimit(0.0, 1.0,
                            static_cast<double>(rms) / mode2::params::levelMeterReferenceRms);
    }
}

Mode2Screen::Mode2Screen(Mode2Controller& controllerIn)
    : controller(controllerIn)
{
    guitarPitchLabel.setJustificationType(juce::Justification::centredLeft);
    vocalPitchLabel.setJustificationType(juce::Justification::centredLeft);
    correctedPitchLabel.setJustificationType(juce::Justification::centredLeft);
    followLabel.setJustificationType(juce::Justification::centredLeft);

    addAndMakeVisible(guitarPitchLabel);
    addAndMakeVisible(vocalPitchLabel);
    addAndMakeVisible(correctedPitchLabel);
    addAndMakeVisible(followLabel);
    addAndMakeVisible(followProgressBar);

    guitarLevelLabel.setJustificationType(juce::Justification::centredLeft);
    vocalLevelLabel.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(guitarLevelLabel);
    addAndMakeVisible(vocalLevelLabel);
    addAndMakeVisible(guitarLevelBar);
    addAndMakeVisible(vocalLevelBar);

    channelMapLabel.setText(utf8("입력 채널  기타 / 목소리"), juce::dontSendNotification);
    channelMapLabel.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(channelMapLabel);

    auto notifyChannelChange = [this]
    {
        if (onInputChannelsChanged != nullptr)
            onInputChannelsChanged(guitarChannelBox.getSelectedId() - 1, vocalChannelBox.getSelectedId() - 1);
    };
    guitarChannelBox.onChange = notifyChannelChange;
    vocalChannelBox.onChange = notifyChannelChange;
    addAndMakeVisible(guitarChannelBox);
    addAndMakeVisible(vocalChannelBox);

    outputLevelLabel.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(outputLevelLabel);
    addAndMakeVisible(outputLevelBar);

    vocalGainLabel.setText(utf8("목소리 입력 부스트"), juce::dontSendNotification);
    vocalGainLabel.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(vocalGainLabel);
    vocalGainSlider.setRange(1.0, 40.0, 0.1);
    vocalGainSlider.setValue(1.0, juce::dontSendNotification);
    vocalGainSlider.onValueChange = [this]
    {
        controller.setVocalInputGain(static_cast<float>(vocalGainSlider.getValue()));
    };
    addAndMakeVisible(vocalGainSlider);

    outputVolumeLabel.setText(utf8("출력 볼륨"), juce::dontSendNotification);
    outputVolumeLabel.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(outputVolumeLabel);
    outputVolumeSlider.setRange(0.0, 2.0, 0.01);
    outputVolumeSlider.setValue(1.0, juce::dontSendNotification);
    outputVolumeSlider.onValueChange = [this]
    {
        controller.setOutputVolume(static_cast<float>(outputVolumeSlider.getValue()));
    };
    addAndMakeVisible(outputVolumeSlider);

    notchLabel.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(notchLabel);

    latencyLabel.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(latencyLabel);

    // 게이트 임계는 "목소리 입력 레벨" 미터와 같은 % 눈금으로 조절한다. 미터를 보면서
    // "이 정도 아래는 잡음"이라고 판단한 값을 그대로 넣을 수 있다.
    noiseGateButton.setButtonText(utf8("노이즈 게이트  임계"));
    noiseGateButton.setToggleState(true, juce::dontSendNotification);
    noiseGateButton.onClick = [this]
    {
        controller.setNoiseGateEnabled(noiseGateButton.getToggleState());
    };
    addAndMakeVisible(noiseGateButton);

    noiseGateSlider.setRange(0.0, 50.0, 0.5);
    noiseGateSlider.setTextValueSuffix("%");
    noiseGateSlider.setValue(mode2::params::vocalNoiseGateThreshold
                                 / mode2::params::levelMeterReferenceRms * 100.0,
                             juce::dontSendNotification);
    noiseGateSlider.onValueChange = [this]
    {
        const float rms = static_cast<float>(noiseGateSlider.getValue() / 100.0)
                          * mode2::params::levelMeterReferenceRms;
        controller.setNoiseGateThreshold(rms);
    };
    addAndMakeVisible(noiseGateSlider);

    howlGuardButton.setButtonText(utf8("하울링 억제 (헤드폰이면 끄세요)"));
    howlGuardButton.setToggleState(true, juce::dontSendNotification);
    howlGuardButton.onClick = [this]
    {
        controller.setHowlGuardEnabled(howlGuardButton.getToggleState());
    };
    addAndMakeVisible(howlGuardButton);

    // 출력 트립 게이트 임계도 같은 % 눈금. "출력 레벨"이 평소 이 %를 넘어가면 즉시
    // 완전 묵음시킨다 — feedbackDuck보다 훨씬 강한 하울링 대응이다.
    outputTripLabel.setText(utf8("하울링 트립 임계 (출력 레벨 기준)"), juce::dontSendNotification);
    outputTripLabel.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(outputTripLabel);

    outputTripSlider.setRange(10.0, 100.0, 1.0);
    outputTripSlider.setTextValueSuffix("%");
    outputTripSlider.setValue(mode2::params::outputTripThreshold
                                  / mode2::params::levelMeterReferenceRms * 100.0,
                              juce::dontSendNotification);
    outputTripSlider.onValueChange = [this]
    {
        const float rms = static_cast<float>(outputTripSlider.getValue() / 100.0)
                          * mode2::params::levelMeterReferenceRms;
        controller.setOutputTripThreshold(rms);
    };
    addAndMakeVisible(outputTripSlider);

    calibrateButton.onClick = [this] { controller.startCalibration(); };
    addAndMakeVisible(calibrateButton);

    resetCalibrationButton.setButtonText(utf8("게인 상한 리셋"));
    resetCalibrationButton.onClick = [this] { controller.resetCalibration(); };
    addAndMakeVisible(resetCalibrationButton);

    startTimerHz(15);
}

Mode2Screen::~Mode2Screen()
{
    stopTimer();
}

void Mode2Screen::setAvailableInputChannels(int numChannels, int guitarChannel, int vocalChannel)
{
    // ComboBox의 item id는 0을 쓸 수 없으므로 채널 인덱스 + 1로 저장한다.
    guitarChannelBox.clear(juce::dontSendNotification);
    vocalChannelBox.clear(juce::dontSendNotification);

    for (int ch = 0; ch < numChannels; ++ch)
    {
        const auto name = utf8("채널 ") + juce::String(ch + 1);
        guitarChannelBox.addItem(name, ch + 1);
        vocalChannelBox.addItem(name, ch + 1);
    }

    guitarChannelBox.setSelectedId(juce::jlimit(0, numChannels - 1, guitarChannel) + 1, juce::dontSendNotification);
    vocalChannelBox.setSelectedId(juce::jlimit(0, numChannels - 1, vocalChannel) + 1, juce::dontSendNotification);
}

void Mode2Screen::setLatencyInfo(double inputMs, double outputMs, double processingMs)
{
    const double total = inputMs + outputMs + processingMs;
    juce::String text = utf8("지연 왕복 ") + juce::String(total, 1) + "ms"
                        + utf8("  (입력 ") + juce::String(inputMs, 1)
                        + utf8(" / 출력 ") + juce::String(outputMs, 1)
                        + utf8(" / 처리 ") + juce::String(processingMs, 1) + ")";
    if (total > 60.0)
        text += utf8("   ← 연주감 평가엔 부적합");
    latencyLabel.setText(text, juce::dontSendNotification);
}

juce::String Mode2Screen::midiToDisplayString(float midi)
{
    static const char* noteNames[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

    const int rounded = juce::roundToInt(midi);
    const float cents = (midi - static_cast<float>(rounded)) * 100.0f;

    int noteIndex = rounded % 12;
    if (noteIndex < 0)
        noteIndex += 12;
    const int octave = rounded / 12 - 1;

    const juce::String centsStr = (cents >= 0.0f ? "+" : "") + juce::String(cents, 0);
    return juce::String(noteNames[noteIndex]) + juce::String(octave) + " (" + centsStr + " cent)";
}

void Mode2Screen::timerCallback()
{
    const auto state = controller.getDisplayState();

    guitarPitchLabel.setText(utf8("기타      ") + (state.hasGuitarTarget ? midiToDisplayString(state.guitarTargetMidi) : juce::String("--")),
                              juce::dontSendNotification);
    vocalPitchLabel.setText(utf8("내 목소리 ") + (state.hasVocalPitch ? midiToDisplayString(state.vocalMidi) : juce::String("--")),
                             juce::dontSendNotification);
    correctedPitchLabel.setText(utf8("보정 후   ") + (state.hasVocalPitch ? midiToDisplayString(state.correctedMidi) : juce::String("--")),
                                 juce::dontSendNotification);

    followProgress = static_cast<double>(state.wetness);
    followLabel.setText(utf8("추종도 ") + juce::String(juce::roundToInt(state.wetness * 100.0f)) + "%", juce::dontSendNotification);

    guitarLevelProgress = levelToMeterValue(state.guitarInputLevel);
    vocalLevelProgress = levelToMeterValue(state.vocalInputLevel);
    guitarLevelLabel.setText(utf8("기타 입력 레벨 ") + juce::String(juce::roundToInt(guitarLevelProgress * 100.0)) + "%",
                              juce::dontSendNotification);
    vocalLevelLabel.setText(utf8("목소리 입력 레벨(부스트 후) ") + juce::String(juce::roundToInt(vocalLevelProgress * 100.0)) + "%"
                                 + utf8("   피치 신뢰도 ") + juce::String(state.vocalConfidence, 2),
                             juce::dontSendNotification);

    outputLevelProgress = levelToMeterValue(state.outputLevel);
    outputLevelLabel.setText(utf8("출력 레벨 ") + juce::String(juce::roundToInt(outputLevelProgress * 100.0)) + "%"
                                 + utf8("   게인 ") + juce::String(state.outputGain, 2)
                                 + utf8("   하울링억제 ") + juce::String(state.feedbackDuckGain, 2)
                                 + utf8("   게이트 ") + juce::String(state.noiseGateGain, 2)
                                 + (state.noiseGateOpen ? utf8(" (열림)") : utf8(" (닫힘)"))
                                 + utf8("   트립 ") + juce::String(state.outputTripGain, 2)
                                 + (state.outputTripActive ? utf8(" ⚠") : juce::String()),
                              juce::dontSendNotification);

    juce::String notchText = utf8("하울링 노치 ") + juce::String(state.notchCount) + "/"
                             + juce::String(FeedbackNotchSuppressor::maxNotches);
    if (state.notchCount > 0)
    {
        notchText += "  ";
        for (int i = 0; i < state.notchCount; ++i)
            notchText += juce::String(juce::roundToInt(state.notchFrequencies[static_cast<size_t>(i)])) + "Hz ";
    }
    notchLabel.setText(notchText, juce::dontSendNotification);

    calibrateButton.setButtonText(state.calibrating ? utf8("캘리브레이션 중...") : utf8("캘리브레이션 시작"));
    calibrateButton.setEnabled(!state.calibrating);
}

void Mode2Screen::paint(juce::Graphics& g)
{
    g.fillAll(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));
}

void Mode2Screen::resized()
{
    auto area = getLocalBounds().reduced(20);
    guitarPitchLabel.setBounds(area.removeFromTop(30));
    vocalPitchLabel.setBounds(area.removeFromTop(30));
    correctedPitchLabel.setBounds(area.removeFromTop(30));
    area.removeFromTop(10);
    guitarLevelLabel.setBounds(area.removeFromTop(20));
    guitarLevelBar.setBounds(area.removeFromTop(14));
    area.removeFromTop(6);
    vocalLevelLabel.setBounds(area.removeFromTop(20));
    vocalLevelBar.setBounds(area.removeFromTop(14));
    area.removeFromTop(8);
    channelMapLabel.setBounds(area.removeFromTop(20));
    {
        auto row = area.removeFromTop(26);
        guitarChannelBox.setBounds(row.removeFromLeft(row.getWidth() / 2).reduced(0, 0));
        vocalChannelBox.setBounds(row.reduced(6, 0));
    }
    area.removeFromTop(8);
    outputLevelLabel.setBounds(area.removeFromTop(20));
    outputLevelBar.setBounds(area.removeFromTop(14));
    area.removeFromTop(8);
    vocalGainLabel.setBounds(area.removeFromTop(18));
    vocalGainSlider.setBounds(area.removeFromTop(24));
    outputVolumeLabel.setBounds(area.removeFromTop(18));
    outputVolumeSlider.setBounds(area.removeFromTop(24));
    area.removeFromTop(4);
    noiseGateButton.setBounds(area.removeFromTop(22));
    noiseGateSlider.setBounds(area.removeFromTop(24));
    area.removeFromTop(8);
    followLabel.setBounds(area.removeFromTop(22));
    followProgressBar.setBounds(area.removeFromTop(16));
    area.removeFromTop(8);
    latencyLabel.setBounds(area.removeFromTop(20));
    notchLabel.setBounds(area.removeFromTop(20));
    howlGuardButton.setBounds(area.removeFromTop(26));
    area.removeFromTop(4);
    outputTripLabel.setBounds(area.removeFromTop(18));
    outputTripSlider.setBounds(area.removeFromTop(24));
    area.removeFromTop(8);
    {
        auto row = area.removeFromTop(34);
        calibrateButton.setBounds(row.removeFromLeft(row.getWidth() / 2).reduced(0, 0));
        resetCalibrationButton.setBounds(row.reduced(6, 0));
    }
}
