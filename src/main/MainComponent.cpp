#include "MainComponent.h"
#include "params/Mode2Params.h"
#include "BinaryData.h"

#include <algorithm>
#include <cmath>

// 입력 채널 RMS 진단 로그. 채널 배선이나 기타 유입을 쫓을 때만 켠다(-DMODE2_DIAGNOSTIC_LOG=1).
#ifndef MODE2_DIAGNOSTIC_LOG
 #define MODE2_DIAGNOSTIC_LOG 0
#endif

namespace
{
// 8비트 리터럴을 ASCII로 오해하는 juce::String(const char*) 대신 UTF-8로 명시 변환한다.
juce::String utf8(const char* text)
{
    return juce::String(juce::CharPointer_UTF8(text));
}

// 레퍼런스 이미지의 흰 종이는 앱의 종이 질감과 경계가 생기므로 걷어내고
// 크레용 획의 원래 색만 불투명하게 남긴다.
void removeImagePaper(juce::Image& image)
{
    if (! image.isValid())
        return;

    juce::Image transparent(juce::Image::ARGB, image.getWidth(), image.getHeight(), true);
    juce::Image::BitmapData source(image, juce::Image::BitmapData::readOnly);
    juce::Image::BitmapData destination(transparent, juce::Image::BitmapData::writeOnly);

    for (int y = 0; y < image.getHeight(); ++y)
    {
        for (int x = 0; x < image.getWidth(); ++x)
        {
            const auto colour = source.getPixelColour(x, y);
            const int darkest = juce::jmin(static_cast<int>(colour.getRed()),
                                           juce::jmin(static_cast<int>(colour.getGreen()),
                                                      static_cast<int>(colour.getBlue())));
            const int lightest = juce::jmax(static_cast<int>(colour.getRed()),
                                            juce::jmax(static_cast<int>(colour.getGreen()),
                                                       static_cast<int>(colour.getBlue())));
            const int signal = juce::jmax(255 - lightest, (lightest - darkest) * 2);
            if (signal < 20)
                continue;

            const auto alpha = static_cast<juce::uint8>(
                juce::jlimit(0, 255, (signal - 12) * 5));
            destination.setPixelColour(x, y, colour.withAlpha(alpha));
        }
    }

    image = std::move(transparent);
}

void localiseAudioSelector(juce::Component& component)
{
    if (auto* label = dynamic_cast<juce::Label*>(&component))
    {
        const auto text = label->getText();
        if (text == "Output:")
            label->setText(utf8("출력 장치"), juce::dontSendNotification);
        else if (text == "Input:")
            label->setText(utf8("입력 장치"), juce::dontSendNotification);
        else if (text == "Active output channels:")
            label->setText(utf8("사용할 출력 채널"), juce::dontSendNotification);
        else if (text == "Active input channels:")
            label->setText(utf8("사용할 입력 채널"), juce::dontSendNotification);
        else if (text == "Sample rate:")
            label->setText(utf8("샘플레이트"), juce::dontSendNotification);
        else if (text == "Audio buffer size:")
            label->setText(utf8("오디오 버퍼 크기"), juce::dontSendNotification);

        label->setColour(juce::Label::textColourId, guitaru::green());
    }
    else if (auto* button = dynamic_cast<juce::TextButton*>(&component))
    {
        if (button->getButtonText() == "Test")
            button->setButtonText(utf8("테스트"));
    }

    for (int i = 0; i < component.getNumChildComponents(); ++i)
        localiseAudioSelector(*component.getChildComponent(i));
}

const char* realtimeTraceTypeName(mode1::RealtimeTraceType type) noexcept
{
    switch (type)
    {
        case mode1::RealtimeTraceType::onsetDetected:
            return "onset_detected";
        case mode1::RealtimeTraceType::scoreEventChanged:
            return "score_event_changed";
        case mode1::RealtimeTraceType::phraseRequested:
            return "phrase_requested";
        case mode1::RealtimeTraceType::vocalFirstOutput:
            return "vocal_first_output";
        case mode1::RealtimeTraceType::beatClockObservation:
            return "beat_clock_observation";
        case mode1::RealtimeTraceType::beatClockStateChanged:
            return "beat_clock_state_changed";
        case mode1::RealtimeTraceType::introChromaAlignmentLocked:
            return "intro_chroma_alignment_locked";
        case mode1::RealtimeTraceType::predictiveTimingCorrection:
            return "predictive_timing_correction";
        case mode1::RealtimeTraceType::predictiveTransportState:
            return "predictive_transport_state";
        case mode1::RealtimeTraceType::predictivePhraseScheduled:
            return "predictive_phrase_scheduled";
        case mode1::RealtimeTraceType::predictivePhraseCancelled:
            return "predictive_phrase_cancelled";
        case mode1::RealtimeTraceType::chordMismatchObservation:
            return "chord_mismatch_observation";
        case mode1::RealtimeTraceType::chordMismatchPaused:
            return "chord_mismatch_paused";
        case mode1::RealtimeTraceType::chordMismatchResumed:
            return "chord_mismatch_resumed";
    }
    return "unknown";
}
}

MainComponent::MainComponent()
{
    setLookAndFeel(&guitaruLookAndFeel);
    setWantsKeyboardFocus(true);

    logoImage = juce::ImageFileFormat::loadFrom(BinaryData::guitarulogocentered_png,
                                                 BinaryData::guitarulogocentered_pngSize);
    mascotImage = juce::ImageFileFormat::loadFrom(BinaryData::guitarumascot_png,
                                                   BinaryData::guitarumascot_pngSize);
    if (logoImage.isValid())
        logoImage = logoImage.getClippedImage({ 210, 180, 1360, 520 })
                            .rescaled(540, 206, juce::Graphics::highResamplingQuality);
    if (mascotImage.isValid())
        mascotImage = mascotImage.getClippedImage({ 260, 65, 860, 1100 })
                                .rescaled(440, 590, juce::Graphics::highResamplingQuality);
    removeImagePaper(logoImage);
    removeImagePaper(mascotImage);

    std::unique_ptr<juce::XmlElement> savedAudioState;
    if (const auto settingsFile = getAudioSettingsFile(); settingsFile.existsAsFile())
        savedAudioState = juce::XmlDocument::parse(settingsFile);
    const bool hadSavedAudioState = savedAudioState != nullptr;

    // 입력은 여유 있게 요청한다. 통합 기기(Aggregate Device)로 오디오 인터페이스와 내장
    // 마이크를 함께 쓰면 입력 채널이 3개 이상이 되고, 어느 채널을 쓸지는 화면에서 고른다.
    setAudioChannels(8, 2, savedAudioState.get());
    deviceManager.addChangeListener(this);

    // 저장된 장치 설정이 있으면 사용자가 직접 고른 것이므로 건드리지 않는다.
    // 첫 실행에서만 저지연 기본값(ASIO / 128 프레임 근처)을 찾아 적용한다.
    if (! hadSavedAudioState)
        selectPreferredLowLatencyDevice();

    // --- 랜딩 화면 -------------------------------------------------------
    statusLabel.setJustificationType(juce::Justification::centredLeft);
    statusLabel.setColour(juce::Label::textColourId, guitaru::inkMuted());
    statusLabel.setFont(guitaru::chalkFont(18.0f));
    // 장치명은 오디오 장치 설정 창에서만 보여준다.
    addChildComponent(statusLabel);
    updateAudioDeviceStatus();

    mode1Button.setButtonText(utf8("MODE 1 · 보컬 팔로워  →"));
    mode1Button.setColour(juce::TextButton::buttonColourId, guitaru::green());
    mode1Button.onClick = [this] { showMode1(); };
    mode2Button.setButtonText(utf8("MODE 2 · 기타 보코더  →"));
    mode2Button.setColour(juce::TextButton::buttonColourId, guitaru::blue());
    mode2Button.onClick = [this] { showMode2(); };
    addAndMakeVisible(mode1Button);
    addAndMakeVisible(mode2Button);

    audioSettingsButton.setButtonText(utf8("오디오 장치 설정  ⚙"));
    audioSettingsButton.setColour(juce::TextButton::buttonColourId, guitaru::inkMuted());
    audioSettingsButton.onClick = [this] { showAudioSettings(); };
    addAndMakeVisible(audioSettingsButton);

    // --- 모드 1 화면 -----------------------------------------------------
    addChildComponent(mode1Panel);

    mode1StatusLabel.setText(L"곡을 선택하고 시작을 누르세요.", juce::dontSendNotification);
    mode1StatusLabel.setJustificationType(juce::Justification::centredLeft);
    mode1Panel.addAndMakeVisible(mode1StatusLabel);

    mode1HomeButton.setButtonText(utf8("←  홈"));
    mode1HomeButton.setColour(juce::TextButton::buttonColourId, guitaru::paperDark());
    mode1HomeButton.setColour(juce::TextButton::textColourOffId, guitaru::green());
    mode1HomeButton.onClick = [this] { showLanding(); };
    mode1Panel.addAndMakeVisible(mode1HomeButton);

    advancedToggleButton.setColour(juce::TextButton::buttonColourId, guitaru::paperDark());
    advancedToggleButton.setColour(juce::TextButton::textColourOffId, guitaru::inkMuted());
    advancedToggleButton.onClick = [this]
    {
        setAdvancedControlsVisible(! showAdvancedControls);
    };
    mode1Panel.addAndMakeVisible(advancedToggleButton);

    loadSongButton.onClick = [this] { chooseSongPackage(); };
    songSelector.setTextWhenNothingSelected(L"곡 선택");
    songSelector.addItem(L"만찬가", 1);
    songSelector.addItem("Don't Look Back in Anger", 2);
    songSelector.addItem("Hype Boy", 3);
    songSelector.setTooltip(L"Mode 1에서 연주할 곡을 선택합니다.");
    songSelector.onChange = [this]
    {
        juce::String songSlug;
        if (songSelector.getSelectedId() == 1)
            songSlug = "bansanka";
        else if (songSelector.getSelectedId() == 2)
            songSlug = "dont_look_back_in_anger";
        else if (songSelector.getSelectedId() == 3)
            songSlug = "hype_boy";
        else
            return;

        const auto package = findBundledSongPackage(songSlug);
        if (!package.existsAsFile())
        {
            mode1StatusLabel.setText(
                L"곡 패키지를 찾을 수 없습니다: "
                    + package.getFullPathName(),
                juce::dontSendNotification);
            return;
        }

        loadSongPackage(package);
    };
    followerModeSelector.addItem(L"기준선", 1);
    followerModeSelector.addItem(L"예측 Shadow", 2);
    followerModeSelector.addItem(L"예측 Active", 3);
    followerModeSelector.addItem(L"Active v2 Shadow", 4);
    followerModeSelector.addItem(L"Active v2", 5);
    followerModeSelector.setTooltip(
        L"Shadow는 측정만 하고, Active만 제한된 다음 경계 예측을 재생에 적용합니다.");
    followerModeSelector.onChange = [this]
    {
        const auto selected = followerModeSelector.getSelectedId();
        const auto mode = selected == 5
            ? mode1::FollowerMode::activeV2
            : selected == 4
            ? mode1::FollowerMode::activeV2Shadow
            : selected == 3
                ? mode1::FollowerMode::predictiveActive
            : selected == 2
                ? mode1::FollowerMode::predictiveShadow
                : mode1::FollowerMode::baseline;
        {
            const juce::ScopedLock callbackLock(
                deviceManager.getAudioCallbackLock());
            mode1Controller.setFollowerMode(mode);
        }
        mode1StatusLabel.setText(
            selected == 5
                ? L"Active v2: 연속 transport 예측을 실제 보컬 출력에 적용"
                : selected == 4
                ? L"Active v2 Shadow: 소리는 기준선, 연속 transport 예약/취소만 측정"
                : selected == 3
                ? L"예측 Active: 신뢰도 gate를 통과한 다음 악보 경계만 미리 예약"
                : selected == 2
                    ? L"예측 Shadow: 소리는 기준선, BeatClock만 측정"
                    : L"기준선: 기타 이벤트 기반 재생",
            juce::dontSendNotification);
    };
    // 기본값을 Active v2로 둔다. 같은 기타 입력으로 오프라인 렌더러를 돌려 보면
    // 기준선/예측 Shadow는 프레이즈를 통째로 버리고 악보에 없는 공백을 만드는데
    // (연속 연주 40초 기준: 73개 중 34개 유실, 악보보다 0.5초 이상 늘어진 구간 6곳),
    // Active v2는 같은 입력에서 유실 0개, 늘어진 구간 0곳이었다.
    // onChange가 컨트롤러까지 함께 맞추므로 콤보 설정은 핸들러를 단 뒤에 한다.
    followerModeSelector.setSelectedId(5, juce::sendNotificationSync);
    nextPhraseButton.onClick = [this] { triggerNextPhrase(); };
    nextPhraseButton.setEnabled(false);
    mode1Panel.addAndMakeVisible(songSelector);
    mode1Panel.addAndMakeVisible(followerModeSelector);
    pauseOnWrongChordToggle.setToggleState(
        false, juce::dontSendNotification);
    pauseOnWrongChordToggle.setTooltip(
        L"연속된 코드 불일치를 감지하면 보컬을 멈추고, 올바른 코드에서 다음 가사부터 재개합니다.");
    pauseOnWrongChordToggle.onClick = [this]
    {
        const bool enabled =
            pauseOnWrongChordToggle.getToggleState();
        {
            const juce::ScopedLock callbackLock(
                deviceManager.getAudioCallbackLock());
            mode1Controller.setPauseOnChordMismatchEnabled(enabled);
        }
        mode1StatusLabel.setText(
            enabled
                ? L"코드 오류 일시정지 켜짐 · 불일치 2회 시 정지"
                : L"코드 오류 일시정지 꺼짐",
            juce::dontSendNotification);
        grabKeyboardFocus();
    };
    mode1Panel.addAndMakeVisible(pauseOnWrongChordToggle);
    followPerformanceTempoToggle.setToggleState(
        false, juce::dontSendNotification);
    followPerformanceTempoToggle.setTooltip(
        L"기타 연주의 안정화된 템포에 맞춰 보컬 발음 길이를 자연스럽게 늘리거나 줄입니다.");
    followPerformanceTempoToggle.onClick = [this]
    {
        const bool enabled =
            followPerformanceTempoToggle.getToggleState();
        {
            const juce::ScopedLock callbackLock(
                deviceManager.getAudioCallbackLock());
            mode1Controller.setFollowPerformanceTempoEnabled(enabled);
        }
        mode1StatusLabel.setText(
            enabled
                ? L"연주 속도 보컬 추종 켜짐 · 안정화된 템포를 부드럽게 적용"
                : L"연주 속도 보컬 추종 꺼짐 · 원래 발음 길이 유지",
            juce::dontSendNotification);
        grabKeyboardFocus();
    };
    mode1Panel.addAndMakeVisible(followPerformanceTempoToggle);
    mode1Panel.addAndMakeVisible(loadSongButton);
    mode1Panel.addAndMakeVisible(nextPhraseButton);

    startPerformanceButton.onClick =
        [this] { startMode1Performance(false); };
    restartPerformanceButton.onClick =
        [this] { startMode1Performance(true); };
    stopPerformanceButton.onClick =
        [this] { stopMode1Performance(); };
    startPerformanceButton.setEnabled(false);
    restartPerformanceButton.setEnabled(false);
    stopPerformanceButton.setEnabled(false);
    mode1Panel.addAndMakeVisible(startPerformanceButton);
    mode1Panel.addAndMakeVisible(restartPerformanceButton);
    mode1Panel.addAndMakeVisible(stopPerformanceButton);

    guitarRecordStartButton.onClick =
        [this] { startGuitarTestRecording(); };
    guitarRecordStopButton.onClick =
        [this] { stopGuitarTestRecording(); };
    guitarReplayStartButton.onClick =
        [this] { startGuitarTestReplay(); };
    guitarReplayStopButton.onClick =
        [this] { stopGuitarTestReplay(); };
    mode1Panel.addAndMakeVisible(guitarRecordStartButton);
    mode1Panel.addAndMakeVisible(guitarRecordStopButton);
    mode1Panel.addAndMakeVisible(guitarReplayStartButton);
    mode1Panel.addAndMakeVisible(guitarReplayStopButton);
    refreshGuitarTestControls();

    mode1AudioSettingsButton.onClick = [this] { showAudioSettings(); };
    mode1AudioSettingsButton.setTooltip(
        L"오인페, 드라이버, 샘플레이트와 버퍼 크기를 설정합니다.");
    mode1Panel.addAndMakeVisible(mode1AudioSettingsButton);

    measureLatencyButton.onClick = [this] { requestLatencyMeasurement(); };
    measureLatencyButton.setTooltip(
        L"오인페 출력을 선택한 기타 입력으로 연결해 실제 왕복 레이턴시를 측정합니다.");
    applyLatencyButton.onClick = [this] { applyLatencyMeasurement(); };
    applyLatencyButton.setEnabled(false);
    resetLatencyButton.onClick = [this] { resetLatencyCompensation(); };
    latencyStatusLabel.setJustificationType(juce::Justification::centredLeft);
    latencyStatusLabel.setMinimumHorizontalScale(0.72f);
    mode1Panel.addAndMakeVisible(measureLatencyButton);
    mode1Panel.addAndMakeVisible(applyLatencyButton);
    mode1Panel.addAndMakeVisible(resetLatencyButton);
    mode1Panel.addAndMakeVisible(latencyStatusLabel);

    guitarChannelSelector.setTextWhenNothingSelected(L"기타 입력");
    for (int channel = 0; channel < 8; ++channel)
        guitarChannelSelector.addItem(L"기타: Input " + juce::String(channel + 1), channel + 1);
    guitarChannelSelector.onChange = [this]
    {
        guitarChannelIndex.store(
            std::max(0, guitarChannelSelector.getSelectedId() - 1));
        // 채널 매핑은 두 모드가 공유하므로 여기서 고른 값도 저장해 둔다.
        saveChannelMap();
        liveGuitarPeak.store(0.0f);
        guitarStatusLabel.setText(
            L"기타 입력 채널 "
                + juce::String(guitarChannelSelector.getSelectedId())
                + L" 선택됨 · 기타를 쳐서 입력 레벨을 확인하세요",
            juce::dontSendNotification);
        grabKeyboardFocus();
    };
    mode1Panel.addAndMakeVisible(guitarChannelSelector);

    virtualChordLabel.setText(
        L"가상 기타 코드 (현재 엔진은 코드 루트 기준)",
        juce::dontSendNotification);
    virtualChordLabel.setJustificationType(
        juce::Justification::centredLeft);
    mode1Panel.addAndMakeVisible(virtualChordLabel);

    static constexpr const char* chordLabels[] = {
        "C", "C#/Db", "D", "Eb", "E", "F",
        "F#/Gb", "G", "Ab", "A", "Bb", "B",
    };
    for (int root = 0; root < 12; ++root)
    {
        auto& button =
            virtualChordButtons[static_cast<size_t>(root)];
        button.setButtonText(chordLabels[root]);
        button.setEnabled(false);
        button.onClick = [this, root]
        {
            if (activeMode != ActiveMode::mode1
                || !mode1Controller.hasSong())
                return;
            mode1Controller.triggerVirtualChord(root);
            mode1StatusLabel.setText(
                L"가상 기타 입력: "
                    + virtualChordButtons[static_cast<size_t>(root)]
                        .getButtonText(),
                juce::dontSendNotification);
            grabKeyboardFocus();
        };
        mode1Panel.addAndMakeVisible(button);
    }

    automaticPlaybackButton.setClickingTogglesState(true);
    automaticPlaybackButton.setEnabled(false);
    automaticPlaybackButton.onClick = [this]
    {
        const bool enabled =
            automaticPlaybackButton.getToggleState();
        {
            const juce::ScopedLock callbackLock(
                deviceManager.getAudioCallbackLock());
            if (enabled)
                mode1Controller.startAutomaticPlayback();
            else
                mode1Controller.startPerformance();
        }
        automaticPlaybackButton.setButtonText(
            enabled ? L"자동 연주 정지" : L"자동 연주 시작");
        refreshTransportControls();
        mode1StatusLabel.setText(
            enabled
                ? L"자동 연주 중: 곡의 프레이즈 타이밍대로 재생합니다."
                : L"기타 대기 모드: 코드 버튼, 실제 기타 또는 Space를 기다립니다.",
            juce::dontSendNotification);
        grabKeyboardFocus();
    };
    mode1Panel.addAndMakeVisible(automaticPlaybackButton);

    expressionSlider.setRange(0.0, 100.0, 25.0);
    expressionSlider.setValue(25.0, juce::dontSendNotification);
    expressionSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    expressionSlider.setTextBoxStyle(
        juce::Slider::TextBoxRight, false, 64, 24);
    expressionSlider.onValueChange = [this]
    {
        if (expressionSlider.isMouseButtonDown())
            return;
        const int strength = juce::roundToInt(expressionSlider.getValue());
        juce::String expressionError;
        {
            const juce::ScopedLock callbackLock(
                deviceManager.getAudioCallbackLock());
            if (!mode1Controller.setExpressionStrength(
                    strength, &expressionError))
            {
                mode1StatusLabel.setText(
                    L"표현 음원 로드 실패: " + expressionError,
                    juce::dontSendNotification);
                return;
            }
        }
        expressionLabel.setText(
            L"내 스타일  ←  원곡 표현 " + juce::String(strength) + L"%",
            juce::dontSendNotification);
    };
    expressionSlider.onDragEnd = [this]
    {
        if (expressionSlider.onValueChange)
            expressionSlider.onValueChange();
    };
    expressionLabel.setText(
        L"내 스타일  ←  원곡 표현 25% (기본)",
        juce::dontSendNotification);
    expressionLabel.setJustificationType(juce::Justification::centredLeft);
    mode1Panel.addAndMakeVisible(expressionLabel);
    mode1Panel.addAndMakeVisible(expressionSlider);

    keyShiftSlider.setRange(-6.0, 6.0, 1.0);
    keyShiftSlider.setValue(0.0, juce::dontSendNotification);
    keyShiftSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    keyShiftSlider.setTextBoxStyle(
        juce::Slider::TextBoxRight, false, 64, 24);
    keyShiftSlider.onValueChange = [this]
    {
        if (keyShiftSlider.isMouseButtonDown())
            return;
        const int shift = juce::roundToInt(keyShiftSlider.getValue());
        juce::String keyError;
        {
            const juce::ScopedLock callbackLock(
                deviceManager.getAudioCallbackLock());
            if (!mode1Controller.setManualKeyShift(shift, &keyError))
            {
                mode1StatusLabel.setText(
                    L"키 음원 로드 실패: " + keyError,
                    juce::dontSendNotification);
                return;
            }
        }
        keyShiftLabel.setText(
            L"키 조절 "
                + juce::String(shift >= 0 ? L"+" : L"")
                + juce::String(shift)
                + L" st · 최종 기준 "
                + juce::String(mode1Controller.getEffectiveBaseKeyShift())
                + L" st"
                + L" · anchor "
                + juce::String(mode1Controller.getSelectedKeyAnchor())
                + L" st · residual "
                + juce::String(mode1Controller.getResidualKeyShift())
                + L" st"
                + (mode1Controller.getEffectiveBaseKeyShift() == 0
                    ? L" (원곡 키)"
                    : L"")
                + (std::abs(mode1Controller.getResidualKeyShift()) > 3
                    ? L" · 큰 이동: 음질 저하 가능"
                    : L""),
            juce::dontSendNotification);
    };
    keyShiftSlider.onDragEnd = [this]
    {
        if (keyShiftSlider.onValueChange)
            keyShiftSlider.onValueChange();
    };
    keyShiftLabel.setText(
        L"키 조절 +0 st · 최종 기준 -17 st",
        juce::dontSendNotification);
    keyShiftLabel.setJustificationType(juce::Justification::centredLeft);
    mode1Panel.addAndMakeVisible(keyShiftLabel);
    mode1Panel.addAndMakeVisible(keyShiftSlider);

    songLabel.setText(L"곡 패키지: 미선택", juce::dontSendNotification);
    songLabel.setJustificationType(juce::Justification::centredLeft);
    mode1Panel.addAndMakeVisible(songLabel);

    lyricLabel.setText(L"가사가 여기에 표시됩니다.", juce::dontSendNotification);
    lyricLabel.setFont(juce::FontOptions(24.0f, juce::Font::bold));
    lyricLabel.setJustificationType(juce::Justification::centred);
    lyricLabel.setColour(juce::Label::backgroundColourId, juce::Colours::black.withAlpha(0.25f));
    mode1Panel.addAndMakeVisible(lyricLabel);

    guitarStatusLabel.setText(L"기타 입력 대기", juce::dontSendNotification);
    guitarStatusLabel.setJustificationType(juce::Justification::centredLeft);
    mode1Panel.addAndMakeVisible(guitarStatusLabel);

    recordingTitleLabel.setText(L"내 목소리 녹음", juce::dontSendNotification);
    recordingTitleLabel.setFont(juce::FontOptions(20.0f, juce::Font::bold));
    mode1Panel.addAndMakeVisible(recordingTitleLabel);

    microphoneChannelSelector.setTextWhenNothingSelected(L"마이크 입력");
    for (int channel = 0; channel < 8; ++channel)
        microphoneChannelSelector.addItem(
            L"마이크: Input " + juce::String(channel + 1),
            channel + 1);
    microphoneChannelSelector.onChange = [this]
    {
        vocalChannelIndex.store(
            std::max(0, microphoneChannelSelector.getSelectedId() - 1));
        saveChannelMap();
    };
    mode1Panel.addAndMakeVisible(microphoneChannelSelector);

    noiseReductionToggle.setToggleState(true, juce::dontSendNotification);
    mode1Panel.addAndMakeVisible(noiseReductionToggle);

    recordButton.onClick = [this] { startGuidedRecordingSession(); };
    recordButton.setColour(
        juce::TextButton::buttonColourId,
        juce::Colour::fromRGB(185, 38, 55));
    mode1Panel.addAndMakeVisible(recordButton);

    profileProgressBar.setPercentageDisplay(false);
    mode1Panel.addAndMakeVisible(profileProgressBar);

    profileProgressLabel.setText(
        L"유효 음성 0초 / 최소 180초 · 통과 클립 0개",
        juce::dontSendNotification);
    mode1Panel.addAndMakeVisible(profileProgressLabel);

    recordingStatusLabel.setText(
        L"준비됨 · '가이드 녹음 시작'을 누르면 문장이 순서대로 안내됩니다.",
        juce::dontSendNotification);
    recordingStatusLabel.setJustificationType(juce::Justification::centredLeft);
    mode1Panel.addAndMakeVisible(recordingStatusLabel);

    recordingSessionScreen.onRetryRequested = [this]
    {
        if (recordingPreflightActive)
        {
            inputLevelCalibrator.start();
            return;
        }
        guidedRecordingSession.requestRetry();
    };
    recordingSessionScreen.onSkipRequested = [this]
    {
        guidedRecordingSession.requestSkip();
    };
    recordingSessionScreen.onCancelRequested = [this]
    {
        cancelGuidedRecordingSession();
    };
    recordingSessionScreen.onStartTrainingRequested = [this]
    {
        startVoiceModelTraining();
    };
    addChildComponent(recordingSessionScreen);

    applyChalkStyleToMode1Controls();
    collectAdvancedMode1Controls();
    setAdvancedControlsVisible(false);

    // --- 모드 2 화면 -----------------------------------------------------
    loadChannelMap();
    mode2Screen.onInputChannelsChanged = [this](int guitarChannel, int vocalChannel)
    {
        if (guitarChannel < 0 || vocalChannel < 0)
            return;
        guitarChannelIndex.store(guitarChannel);
        vocalChannelIndex.store(vocalChannel);
        saveChannelMap();
        refreshChannelChoices();
    };
    mode2Screen.onRoomChannelChanged = [this](int roomChannel)
    {
        roomChannelIndex.store(roomChannel < 0 ? -1 : roomChannel);
        saveChannelMap();
    };
    mode2Screen.onToggleRecording = [this] { return toggleRecording(); };
    mode2Screen.onPitchShifterBackendChanged = [this](PitchShifterEngine::Backend backend)
    {
        selectPitchShifterBackend(backend);
    };
    mode2Screen.onBackRequested = [this] { showLanding(); };
    addChildComponent(mode2Screen);

    refreshChannelChoices();

    if (! hadSavedAudioState)
        configureLowLatencyAudio();
    refreshLatencyDisplay();
    startTimerHz(15);
    // 브랜드 랜딩과 모드 1의 긴 한 열 레이아웃이 모두 들어가는 기본 크기.
    setSize(1120, 1000);
}

MainComponent::~MainComponent()
{
    stopTimer();
    setLookAndFeel(nullptr);
    deviceManager.removeChangeListener(this);
    voiceRecorder.stop();
    {
        const juce::ScopedLock callbackLock(
            deviceManager.getAudioCallbackLock());
        guitarTestRecorder.stop();
        performanceOutputRecorder.stop();
        mode1Controller.setRealtimeTraceEnabled(false);
    }
    shutdownAudio();
    stopRecording();
    recorderThread.stopThread(2000);
}

void MainComponent::prepareToPlay(int samplesPerBlockExpected, double sampleRate)
{
    currentSampleRate = sampleRate;
    if (! recorderThread.isThreadRunning())
        recorderThread.startThread();

    const auto blockSize = static_cast<size_t>(std::max(1, samplesPerBlockExpected));
    guitarInputScratch.assign(blockSize, 0.0f);
    vocalInputScratch.assign(blockSize, 0.0f);
    roomInputScratch.assign(blockSize, 0.0f);
    listenLeftScratch.assign(blockSize, 0.0f);
    listenRightScratch.assign(blockSize, 0.0f);

    mode2Controller.prepare(sampleRate, samplesPerBlockExpected);
    mode1Controller.prepare(sampleRate, samplesPerBlockExpected);
    audioLatencyCalibrator.prepare(sampleRate);
    if (auto* device = deviceManager.getCurrentAudioDevice())
    {
        reportedInputLatencySamples = device->getInputLatencyInSamples();
        reportedOutputLatencySamples = device->getOutputLatencyInSamples();
        const bool canReuseAppliedMeasurement =
            measuredLatencyApplied.load()
            && latencyResultMatchesCurrentDevice();
        const double inputSeconds = canReuseAppliedMeasurement
            ? effectiveInputLatencySeconds.load()
            : reportedInputLatencySamples / sampleRate;
        const double outputSeconds = canReuseAppliedMeasurement
            ? effectiveOutputLatencySeconds.load()
            : reportedOutputLatencySamples / sampleRate;
        if (!canReuseAppliedMeasurement)
            measuredLatencyApplied.store(false);
        mode1Controller.setInputLatencySeconds(inputSeconds);
        mode1Controller.setOutputLatencySeconds(outputSeconds);
    }
    voiceRecorder.prepare(sampleRate);
    guitarTestRecorder.prepare(sampleRate);
    performanceOutputRecorder.prepare(sampleRate);
    inputLevelCalibrator.prepare(sampleRate);
    guidedRecordingSession.prepare(sampleRate);
}

void MainComponent::getNextAudioBlock(
    const juce::AudioSourceChannelInfo& bufferToFill)
{
    if (bufferToFill.buffer == nullptr)
        return;

    auto& buffer = *bufferToFill.buffer;
    const int numSamples = bufferToFill.numSamples;
    const int startSample = bufferToFill.startSample;

    // 모드 2는 입력을 먼저 스크래치로 옮긴 뒤 같은 버퍼에 출력을 덮어쓰므로,
    // 아래 모드 1 경로와 섞이지 않게 완전히 분리해 처리한다.
    if (activeMode == ActiveMode::mode2)
    {
        processMode2Block(bufferToFill);
        return;
    }

    if ((recordingPreflightActive.load() || guidedRecordingSession.isActive())
        && buffer.getNumChannels() > 0)
    {
        const int micChannel = juce::jlimit(
            0,
            buffer.getNumChannels() - 1,
            vocalChannelIndex.load());
        const auto peak =
            buffer.getMagnitude(micChannel, startSample, numSamples);
        liveMicrophonePeak.store(
            juce::jmax(peak, liveMicrophonePeak.load() * 0.82f));
    }

    if (recordingPreflightActive && buffer.getNumChannels() > 0)
    {
        const int micChannel = juce::jlimit(
            0,
            buffer.getNumChannels() - 1,
            vocalChannelIndex.load());
        inputLevelCalibrator.processBlock(
            buffer.getReadPointer(micChannel, startSample), numSamples);
    }

    if (guidedRecordingSession.isActive() && buffer.getNumChannels() > 0)
    {
        const int micChannel = juce::jlimit(
            0,
            buffer.getNumChannels() - 1,
            vocalChannelIndex.load());
        const auto* micInput = buffer.getReadPointer(micChannel, startSample);
        guidedRecordingSession.processAudioBlock(micInput, numSamples);

        const bool shouldCapture = guidedRecordingSession.shouldBeCapturing();
        if (shouldCapture && !voiceRecorder.isRecording())
            voiceRecorder.start();
        else if (!shouldCapture && voiceRecorder.isRecording())
            voiceRecorder.stop();
    }

    if (voiceRecorder.isRecording())
        voiceRecorder.processBlock(
            buffer, vocalChannelIndex.load(), startSample, numSamples);

    if (static_cast<int>(guitarInputScratch.size()) < numSamples)
    {
        // The device is not expected to exceed the prepared block size.
        // Stay silent instead of allocating on the real-time audio thread.
        bufferToFill.clearActiveBufferRegion();
        return;
    }

    const int guitarChannel = buffer.getNumChannels() > 0
        ? juce::jlimit(
            0,
            buffer.getNumChannels() - 1,
            guitarChannelIndex.load())
        : 0;
    if (guitarTestRecorder.isRecording() && buffer.getNumChannels() > 0)
        guitarTestRecorder.processBlock(
            buffer, guitarChannel, startSample, numSamples);

    if (guitarReplayActive.load())
    {
        const int available =
            guitarReplayAudio.getNumSamples() - guitarReplayPosition;
        const int copied = std::max(0, std::min(numSamples, available));
        if (copied > 0)
            juce::FloatVectorOperations::copy(
                guitarInputScratch.data(),
                guitarReplayAudio.getReadPointer(0, guitarReplayPosition),
                copied);
        if (copied < numSamples)
            std::fill(
                guitarInputScratch.begin() + copied,
                guitarInputScratch.begin() + numSamples,
                0.0f);
        guitarReplayPosition += copied;
        if (guitarReplayPosition >= guitarReplayAudio.getNumSamples())
        {
            guitarReplayActive.store(false);
            guitarReplayFinished.store(true);
        }
        const auto replayRange =
            juce::FloatVectorOperations::findMinAndMax(
                guitarInputScratch.data(), numSamples);
        const float replayPeak = std::max(
            std::abs(replayRange.getStart()),
            std::abs(replayRange.getEnd()));
        liveGuitarPeak.store(std::max(
            replayPeak,
            liveGuitarPeak.load() * 0.82f));
    }
    else if (buffer.getNumChannels() > 0)
    {
        const auto* guitarInput =
            buffer.getReadPointer(guitarChannel, startSample);
        const auto guitarPeak =
            buffer.getMagnitude(guitarChannel, startSample, numSamples);
        liveGuitarPeak.store(
            juce::jmax(guitarPeak, liveGuitarPeak.load() * 0.82f));
        std::copy(
            guitarInput,
            guitarInput + numSamples,
            guitarInputScratch.begin());
    }
    else
    {
        std::fill(
            guitarInputScratch.begin(),
            guitarInputScratch.begin() + numSamples,
            0.0f);
    }

    bufferToFill.clearActiveBufferRegion();

    if (audioLatencyCalibrator.isMeasuring())
    {
        auto* outputLeft = buffer.getNumChannels() > 0
            ? buffer.getWritePointer(0, startSample)
            : nullptr;
        auto* outputRight = buffer.getNumChannels() > 1
            ? buffer.getWritePointer(1, startSample)
            : nullptr;
        audioLatencyCalibrator.processBlock(
            guitarInputScratch.data(),
            outputLeft,
            outputRight,
            numSamples);
        return;
    }

    // 가이드 녹음/입력 레벨 측정 중에는 모드 1 화면을 떠나지 않은 채 전체 화면 오버레이만
    // 덮이므로, 여기서 보컬 재생을 함께 멈춰 녹음에 앱 출력이 섞이지 않게 한다.
    if (activeMode != ActiveMode::mode1
        || recordingPreflightActive.load()
        || guidedRecordingSession.isActive()
        || !mode1Controller.hasSong()
        || buffer.getNumChannels() < 2)
        return;

    auto* outputLeft = buffer.getWritePointer(0, startSample);
    auto* outputRight = buffer.getWritePointer(1, startSample);
    mode1Controller.processBlock(
        guitarInputScratch.data(),
        outputLeft,
        outputRight,
        numSamples);
    // Mix the guitar signal into the speaker output alongside the vocal, so
    // monitoring through the speaker alone (no separate acoustic/hardware
    // guitar path) still hears both together.
    for (int sample = 0; sample < numSamples; ++sample)
    {
        outputLeft[sample] = juce::jlimit(
            -1.0f, 1.0f,
            outputLeft[sample] + guitarInputScratch[static_cast<size_t>(sample)]);
        outputRight[sample] = juce::jlimit(
            -1.0f, 1.0f,
            outputRight[sample] + guitarInputScratch[static_cast<size_t>(sample)]);
    }
    if (performanceOutputRecorder.isRecording())
        performanceOutputRecorder.processBlock(
            buffer, 0, startSample, numSamples);
}

void MainComponent::processMode2Block(const juce::AudioSourceChannelInfo& bufferToFill)
{
    // 주의: 이 버퍼에는 입력 샘플이 담겨 온다. clearActiveBufferRegion()을 먼저 부르면
    // 입력을 읽기 전에 지워버리므로, 처리하지 않는 경우에만 비운다.
    if (bufferToFill.buffer->getNumChannels() < 2)
    {
        bufferToFill.clearActiveBufferRegion();
        return;
    }

    auto& buffer = *bufferToFill.buffer;
    const int numSamples = bufferToFill.numSamples;
    const int startSample = bufferToFill.startSample;

    // 세 버퍼를 각각 확인한다. 예전에는 기타 버퍼 하나만 보고 셋을 함께 늘렸는데, 방 마이크
    // 버퍼를 prepareToPlay에서 할당하는 것을 빠뜨리자 이 검사가 통과해 버려(기타 버퍼는 이미
    // 충분했다) 크기 0인 벡터에 블록을 복사하고 세그폴트가 났다.
    const auto ensure = [numSamples](std::vector<float>& v)
    {
        if (static_cast<int>(v.size()) < numSamples)
            v.resize(static_cast<size_t>(numSamples), 0.0f);
    };
    ensure(guitarInputScratch);
    ensure(vocalInputScratch);
    ensure(roomInputScratch);

    // 뒤에서 outL/outR로 같은 버퍼 채널에 덮어쓰므로, 입력을 먼저 스크래치로 복사해 둔다.
    const int lastChannel = buffer.getNumChannels() - 1;
    const int guitarChannel = juce::jlimit(0, lastChannel, guitarChannelIndex.load());
    const int vocalChannel = juce::jlimit(0, lastChannel, vocalChannelIndex.load());
    const float* guitarRead = buffer.getReadPointer(guitarChannel, startSample);
    const float* vocalRead = buffer.getReadPointer(vocalChannel, startSample);
    std::copy(guitarRead, guitarRead + numSamples, guitarInputScratch.begin());
    std::copy(vocalRead, vocalRead + numSamples, vocalInputScratch.begin());

    // 방 마이크도 여기서 복사해 둔다. 아래에서 출력이 채널 0-1을 덮어쓰므로, 방 마이크를
    // 그 채널에 물린 경우에도 덮이기 전 값이 남아야 한다.
    const int roomChannel = roomChannelIndex.load();
    const bool haveRoom = roomChannel >= 0 && roomChannel <= lastChannel;
    if (haveRoom)
    {
        const float* roomRead = buffer.getReadPointer(roomChannel, startSample);
        std::copy(roomRead, roomRead + numSamples, roomInputScratch.begin());
    }

#if MODE2_DIAGNOSTIC_LOG
    {
        // 통합기기의 채널 순서가 바뀌었는지, 아니면 마이크가 기타를 줍는지 구분하기 위해
        // 전체 입력 채널의 RMS를 찍는다.
        static int chLogCount = 0;
        if (++chLogCount % 24 == 0)
        {
            juce::String line = "[CH] ";
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            {
                const float* read = buffer.getReadPointer(ch, startSample);
                double ss = 0.0;
                for (int i = 0; i < numSamples; ++i)
                    ss += static_cast<double>(read[i]) * read[i];
                line += "ch" + juce::String(ch) + "=" + juce::String(std::sqrt(ss / numSamples), 5) + "  ";
            }
            line += "(기타=ch" + juce::String(guitarChannel) + " 목소리=ch" + juce::String(vocalChannel) + ")";
            juce::Logger::writeToLog(line);
        }
    }
#endif

    float* outL = buffer.getWritePointer(0, startSample);
    float* outR = buffer.getWritePointer(1, startSample);

    mode2Controller.processBlock(guitarInputScratch.data(), vocalInputScratch.data(), outL, outR, numSamples);

    {
        // 진단 녹음: 기타 입력 / 목소리 입력 / 최종 출력을 한 파일에 남긴다.
        // ThreadedWriter::write는 락 없는 FIFO에 밀어넣기만 하므로 오디오 스레드에서 안전하다.
        const juce::ScopedTryLock stl(writerLock);
        if (stl.isLocked() && activeWriter != nullptr)
        {
            // 채널 수는 녹음을 시작할 때 정해졌다. 도중에 방 마이크 선택이 바뀌어도
            // writer가 기대하는 개수를 그대로 유지해야 한다(무음으로 채운다).
            const float* channels[4] = { guitarInputScratch.data(), vocalInputScratch.data(), outL,
                                         haveRoom ? roomInputScratch.data() : nullptr };
            if (recordingChannelCount == 4 && channels[3] == nullptr)
            {
                std::fill(roomInputScratch.begin(), roomInputScratch.begin() + numSamples, 0.0f);
                channels[3] = roomInputScratch.data();
            }
            activeWriter->write(channels, numSamples);

            // 듣기 파일: 보정된 목소리 + 반주만 섞는다. 기타 라인과 목소리 원본은 넣지 않는다.
            // 0.8은 둘을 더할 때 피크가 1을 넘지 않게 하는 여유다.
            if (activeListenWriter != nullptr)
            {
                const float* extra = recordingChannelCount == 4 ? channels[3] : nullptr;
                for (int i = 0; i < numSamples; ++i)
                {
                    const float mixed = 0.8f * outL[i] + (extra != nullptr ? 0.8f * extra[i] : 0.0f);
                    listenLeftScratch[static_cast<size_t>(i)] = mixed;
                    listenRightScratch[static_cast<size_t>(i)] = mixed;
                }
                const float* listenChannels[2] = { listenLeftScratch.data(),
                                                   listenRightScratch.data() };
                activeListenWriter->write(listenChannels, numSamples);
            }
        }
    }
}

void MainComponent::releaseResources()
{
    audioLatencyCalibrator.cancel();
    mode1Controller.reset();
    voiceRecorder.stop();
    guitarTestRecorder.stop();
    performanceOutputRecorder.stop();
    guitarReplayActive.store(false);
    guidedRecordingSession.stop();
    stopRecording();
    mode2Controller.reset();
}

void MainComponent::requestLatencyMeasurement()
{
    const auto options = juce::MessageBoxOptions()
        .withIconType(juce::MessageBoxIconType::WarningIcon)
        .withTitle(L"오디오 왕복 레이턴시 측정")
        .withMessage(
            L"1. 스피커/헤드폰 볼륨을 낮추세요.\n"
            L"2. 오인페 출력 1을 현재 선택한 기타 입력 "
            + juce::String(guitarChannelIndex.load() + 1)
            + L"에 케이블로 연결하세요.\n"
            L"3. 직접 모니터링과 이펙트를 끄세요.\n\n"
            L"약 2초간 작은 테스트 신호를 5번 보냅니다.")
        .withButton(L"측정 시작")
        .withButton(L"취소");

    juce::AlertWindow::showAsync(
        options,
        [safeThis = juce::Component::SafePointer<MainComponent>(this)]
        (int result)
        {
            if (result == 1 && safeThis != nullptr)
                safeThis->beginLatencyMeasurement();
        });
}

void MainComponent::beginLatencyMeasurement()
{
    auto* device = deviceManager.getCurrentAudioDevice();
    if (device == nullptr)
    {
        latencyStatusLabel.setText(
            L"오디오 장치가 열려 있지 않습니다.",
            juce::dontSendNotification);
        return;
    }

    {
        const juce::ScopedLock callbackLock(
            deviceManager.getAudioCallbackLock());
        guitarReplayActive.store(false);
        guitarReplayFinished.store(false);
        guitarTestRecorder.stop();
        performanceOutputRecorder.stop();
        mode1Controller.stopPerformance();
        if (!audioLatencyCalibrator.start())
        {
            latencyStatusLabel.setText(
                L"측정을 시작할 수 없습니다. 잠시 후 다시 시도하세요.",
                juce::dontSendNotification);
            return;
        }
    }

    hasLatencyResult = false;
    applyLatencyButton.setEnabled(false);
    measureLatencyButton.setEnabled(false);
    latencyStatusLabel.setColour(
        juce::Label::textColourId, juce::Colours::yellow);
    latencyStatusLabel.setText(
        L"측정 중… 케이블을 건드리지 마세요.",
        juce::dontSendNotification);
    refreshTransportControls();
}

void MainComponent::applyLatencyMeasurement()
{
    if (!hasLatencyResult
        || !lastLatencyResult.valid
        || !latencyResultMatchesCurrentDevice())
    {
        latencyStatusLabel.setText(
            L"현재 장치 설정과 일치하는 유효한 측정값이 없습니다.",
            juce::dontSendNotification);
        return;
    }

    const double reportedTotal =
        reportedInputLatencySamples + reportedOutputLatencySamples;
    const double measuredTotal = lastLatencyResult.roundTripSamples;
    const double inputRatio = reportedTotal > 0.0
        ? reportedInputLatencySamples / reportedTotal
        : 0.5;
    const double effectiveInputSamples = measuredTotal * inputRatio;
    const double effectiveOutputSamples =
        measuredTotal - effectiveInputSamples;

    effectiveInputLatencySeconds.store(
        effectiveInputSamples / currentSampleRate);
    effectiveOutputLatencySeconds.store(
        effectiveOutputSamples / currentSampleRate);
    measuredLatencyApplied.store(true);
    {
        const juce::ScopedLock callbackLock(
            deviceManager.getAudioCallbackLock());
        mode1Controller.setInputLatencySeconds(
            effectiveInputLatencySeconds.load());
        mode1Controller.setOutputLatencySeconds(
            effectiveOutputLatencySeconds.load());
    }
    refreshLatencyDisplay();
}

void MainComponent::resetLatencyCompensation()
{
    measuredLatencyApplied.store(false);
    const double inputSeconds =
        reportedInputLatencySamples / currentSampleRate;
    const double outputSeconds =
        reportedOutputLatencySamples / currentSampleRate;
    {
        const juce::ScopedLock callbackLock(
            deviceManager.getAudioCallbackLock());
        mode1Controller.setInputLatencySeconds(inputSeconds);
        mode1Controller.setOutputLatencySeconds(outputSeconds);
    }
    refreshLatencyDisplay();
}

bool MainComponent::latencyResultMatchesCurrentDevice() const
{
    auto* device = deviceManager.getCurrentAudioDevice();
    return device != nullptr
        && latencyMeasurementDeviceName == device->getName()
        && std::abs(latencyMeasurementSampleRate - currentSampleRate) < 1.0
        && latencyMeasurementBufferSize
            == device->getCurrentBufferSizeSamples();
}

void MainComponent::refreshLatencyDisplay()
{
    auto* device = deviceManager.getCurrentAudioDevice();
    if (device == nullptr)
    {
        latencyStatusLabel.setText(
            L"오디오 장치 없음", juce::dontSendNotification);
        return;
    }

    const double reportedMs =
        1000.0 * (reportedInputLatencySamples + reportedOutputLatencySamples)
        / currentSampleRate;
    juce::String text =
        device->getTypeName() + L" / " + device->getName()
        + L" · " + juce::String(currentSampleRate / 1000.0, 1)
        + L"kHz / " + juce::String(device->getCurrentBufferSizeSamples())
        + L" samples · 드라이버 왕복 "
        + juce::String(reportedMs, 1) + L"ms";

    if (hasLatencyResult && latencyResultMatchesCurrentDevice())
    {
        const double measuredMs =
            1000.0 * lastLatencyResult.roundTripSamples / currentSampleRate;
        const double jitterMs =
            1000.0 * lastLatencyResult.jitterSamples / currentSampleRate;
        text += L" · 실측 " + juce::String(measuredMs, 1)
            + L"ms (편차 " + juce::String(jitterMs, 2)
            + L"ms, " + juce::String(lastLatencyResult.successfulProbes)
            + L"/" + juce::String(lastLatencyResult.totalProbes) + L")";
    }
    text += measuredLatencyApplied.load() ? L" · [실측 적용]" : L" · [드라이버 값]";
    latencyStatusLabel.setColour(
        juce::Label::textColourId,
        measuredLatencyApplied.load()
            ? juce::Colours::lightgreen
            : juce::Colours::lightgrey);
    latencyStatusLabel.setText(text, juce::dontSendNotification);

    juce::StringArray supportedBufferSizes;
    for (const int size : device->getAvailableBufferSizes())
        supportedBufferSizes.add(juce::String(size));
    latencyStatusLabel.setTooltip(
        L"현재 드라이버: " + device->getTypeName()
        + L"\n지원 버퍼(samples): "
        + supportedBufferSizes.joinIntoString(", "));
}

void MainComponent::selectPreferredLowLatencyDevice()
{
   #if JUCE_WINDOWS && JUCE_ASIO
    auto* currentDevice = deviceManager.getCurrentAudioDevice();
    if (deviceManager.getCurrentAudioDeviceType() == "ASIO"
        && currentDevice != nullptr
        && currentDevice->getName().containsIgnoreCase("Focusrite USB"))
        return;

    deviceManager.setCurrentAudioDeviceType("ASIO", true);
    auto* type = deviceManager.getCurrentDeviceTypeObject();
    if (type == nullptr || type->getTypeName() != "ASIO")
        return;

    type->scanForDevices();
    const auto inputNames = type->getDeviceNames(true);
    const auto outputNames = type->getDeviceNames(false);
    juce::String focusriteInput;
    juce::String focusriteOutput;
    for (const auto& name : inputNames)
        if (name.containsIgnoreCase("Focusrite USB"))
            focusriteInput = name;
    for (const auto& name : outputNames)
        if (name.containsIgnoreCase("Focusrite USB"))
            focusriteOutput = name;

    if (focusriteInput.isEmpty() || focusriteOutput.isEmpty())
        return;

    auto setup = deviceManager.getAudioDeviceSetup();
    setup.inputDeviceName = focusriteInput;
    setup.outputDeviceName = focusriteOutput;
    setup.sampleRate = 48'000.0;
    setup.bufferSize = 128;
    auto error = deviceManager.setAudioDeviceSetup(setup, true);
    if (error.isNotEmpty())
    {
        // Some Focusrite driver revisions require buffer changes through
        // Focusrite Device Settings. Keep ASIO selected in that case.
        setup.bufferSize = 0;
        error = deviceManager.setAudioDeviceSetup(setup, true);
    }

    if (error.isNotEmpty())
        mode1StatusLabel.setText(
            L"Focusrite USB ASIO 열기 실패: " + error,
            juce::dontSendNotification);
   #endif
}

void MainComponent::configureLowLatencyAudio()
{
    auto* device = deviceManager.getCurrentAudioDevice();
    if (device == nullptr)
        return;

    auto setup = deviceManager.getAudioDeviceSetup();
    const auto availableSizes = device->getAvailableBufferSizes();
    if (!availableSizes.isEmpty())
    {
        int targetSize = availableSizes[0];
        int targetDistance = std::abs(targetSize - 128);
        for (const int candidate : availableSizes)
        {
            const int distance = std::abs(candidate - 128);
            if (distance < targetDistance)
            {
                targetSize = candidate;
                targetDistance = distance;
            }
        }
        setup.bufferSize = targetSize;
    }

    const auto error = deviceManager.setAudioDeviceSetup(setup, true);
    if (error.isNotEmpty())
    {
        mode1StatusLabel.setText(
            L"저지연 버퍼 설정 실패: " + error,
            juce::dontSendNotification);
    }
}

void MainComponent::showAudioSettings()
{
    // 입력 채널은 개별로 고를 수 있어야 한다. 통합 기기에서는 기타와 목소리가
    // 스테레오 쌍이 아닌 서로 다른 채널로 들어오기 때문이다.
    auto selector = std::make_unique<juce::AudioDeviceSelectorComponent>(
        deviceManager, 1, 8, 1, 2, false, false, false, false);
    selector->setSize(560, 450);
    localiseAudioSelector(*selector);

    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(selector.release());
    options.dialogTitle = utf8("오디오 장치 설정");
    options.dialogBackgroundColour =
        getLookAndFeel().findColour(
            juce::ResizableWindow::backgroundColourId);
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = true;
    options.launchAsync();
}

void MainComponent::setVirtualControlsEnabled(bool enabled)
{
    automaticPlaybackButton.setEnabled(enabled);
    startPerformanceButton.setEnabled(
        enabled && !mode1Controller.isPerformanceRunning());
    restartPerformanceButton.setEnabled(enabled);
    stopPerformanceButton.setEnabled(
        enabled && mode1Controller.isPerformanceRunning());
    const bool chordButtonsEnabled =
        enabled
        && mode1Controller.isPerformanceRunning()
        && !automaticPlaybackButton.getToggleState();
    for (auto& button : virtualChordButtons)
        button.setEnabled(chordButtonsEnabled);
    nextPhraseButton.setEnabled(chordButtonsEnabled);
}

void MainComponent::refreshTransportControls()
{
    setVirtualControlsEnabled(
        activeMode == ActiveMode::mode1
        && mode1Controller.hasSong());
}

void MainComponent::startMode1Performance(bool restart)
{
    if (activeMode != ActiveMode::mode1 || !mode1Controller.hasSong())
        return;

    if (guitarReplayFinished.exchange(false))
    {
        {
            const juce::ScopedLock callbackLock(
                deviceManager.getAudioCallbackLock());
            mode1Controller.stopPerformance();
            guitarReplayPosition = 0;
        }
        lyricLabel.setText(
            L"저장된 기타 입력 테스트 완료",
            juce::dontSendNotification);
        mode1StatusLabel.setText(
            L"기타 WAV 테스트가 끝났습니다. 다시 재생할 수 있습니다.",
            juce::dontSendNotification);
        refreshTransportControls();
    }
    refreshGuitarTestControls();

    {
        const juce::ScopedLock callbackLock(
            deviceManager.getAudioCallbackLock());
        guitarReplayActive.store(false);
        guitarReplayFinished.store(false);
        guitarReplayPosition = 0;
        if (restart)
            mode1Controller.restartPerformance();
        else
            mode1Controller.startPerformance();
    }
    automaticPlaybackButton.setToggleState(
        false, juce::dontSendNotification);
    automaticPlaybackButton.setButtonText(L"자동 연주 시작");
    lyricLabel.setText(
        L"인트로 대기 · 악보 첫 코드부터 연주하세요",
        juce::dontSendNotification);
    mode1StatusLabel.setText(
        restart
            ? L"Mode 1 재시작: 처음부터 기타 입력을 기다립니다."
            : L"Mode 1 시작: 처음부터 기타 입력을 기다립니다.",
        juce::dontSendNotification);
    refreshTransportControls();
    refreshGuitarTestControls();
    grabKeyboardFocus();
}

void MainComponent::stopMode1Performance()
{
    if (!mode1Controller.hasSong())
        return;

    {
        const juce::ScopedLock callbackLock(
            deviceManager.getAudioCallbackLock());
        guitarReplayActive.store(false);
        guitarReplayFinished.store(false);
        guitarReplayPosition = 0;
        mode1Controller.stopPerformance();
    }
    automaticPlaybackButton.setToggleState(
        false, juce::dontSendNotification);
    automaticPlaybackButton.setButtonText(L"자동 연주 시작");
    lyricLabel.setText(
        L"연주 중지됨 · 시작 또는 재시작을 누르세요",
        juce::dontSendNotification);
    mode1StatusLabel.setText(
        L"Mode 1 중지: 기타 입력 레벨만 확인합니다.",
        juce::dontSendNotification);
    refreshTransportControls();
    refreshGuitarTestControls();
    grabKeyboardFocus();
}

juce::String MainComponent::getPerformanceRecordingSongSlug() const
{
    auto slug = currentSongPackageFile.getParentDirectory().getFileName()
        .retainCharacters(
            "abcdefghijklmnopqrstuvwxyz"
            "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-");
    return slug.isNotEmpty() ? slug : "mode1";
}

juce::File MainComponent::getGuitarTestRecordingFile() const
{
    return findRepositoryRoot()
        .getChildFile("output")
        .getChildFile("audio")
        .getChildFile("guitar_tests")
        .getChildFile(
            getPerformanceRecordingSongSlug()
                + "_last_guitar_take.wav");
}

juce::File MainComponent::getPerformanceOutputRecordingFile() const
{
    return findRepositoryRoot()
        .getChildFile("output")
        .getChildFile("audio")
        .getChildFile("guitar_tests")
        .getChildFile(
            "mode1_" + getPerformanceRecordingSongSlug()
                + "_last_vocal_output.wav");
}

juce::File MainComponent::getPerformanceMixRecordingFile() const
{
    return findRepositoryRoot()
        .getChildFile("output")
        .getChildFile("audio")
        .getChildFile("guitar_tests")
        .getChildFile(
            "mode1_" + getPerformanceRecordingSongSlug()
                + "_last_guitar_and_vocal_mix.wav");
}

juce::File MainComponent::getPerformanceTraceFile() const
{
    return getPerformanceMixRecordingFile()
        .getSiblingFile("mode1_last_realtime_trace.csv");
}

juce::File MainComponent::getPerformanceSessionFile() const
{
    return getPerformanceMixRecordingFile()
        .getSiblingFile("mode1_last_session.json");
}

juce::Result MainComponent::savePerformanceDiagnostics()
{
    juce::StringArray rows;
    rows.add(
        "type,sample,time_sec,related_sample,related_time_sec,"
        "delta_ms,index,value,flags,value2");
    mode1::RealtimeTraceEvent event;
    while (mode1Controller.popRealtimeTraceEvent(event))
    {
        rows.add(
            juce::String(realtimeTraceTypeName(event.type))
            + "," + juce::String(event.sample)
            + "," + juce::String(event.sample / currentSampleRate, 9)
            + "," + juce::String(event.relatedSample)
            + "," + juce::String(
                event.relatedSample / currentSampleRate, 9)
            + "," + juce::String(
                (event.relatedSample - event.sample)
                    * 1000.0 / currentSampleRate,
                6)
            + "," + juce::String(event.index)
            + "," + juce::String(event.value, 6)
            + "," + juce::String(event.flags)
            + "," + juce::String(event.value2, 6));
    }
    const auto traceFile = getPerformanceTraceFile();
    if (!traceFile.replaceWithText(
            rows.joinIntoString("\n") + "\n",
            false,
            false,
            "\n"))
        return juce::Result::fail("Could not save realtime trace.");

    auto* root = new juce::DynamicObject();
    root->setProperty("schema_version", 1);
    root->setProperty(
        "recorded_at",
        juce::Time::getCurrentTime().toISO8601(true));
    root->setProperty(
        "song_package",
        currentSongPackageFile.getFullPathName());
    root->setProperty(
        "song_package_size",
        static_cast<juce::int64>(currentSongPackageFile.getSize()));
    root->setProperty(
        "song_package_modified_ms",
        static_cast<juce::int64>(
            currentSongPackageFile.getLastModificationTime()
                .toMilliseconds()));
    root->setProperty("sample_rate", currentSampleRate);
    int blockSize = 0;
    juce::String deviceName;
    if (auto* device = deviceManager.getCurrentAudioDevice())
    {
        blockSize = device->getCurrentBufferSizeSamples();
        deviceName = device->getName();
    }
    root->setProperty("block_size", blockSize);
    root->setProperty("audio_device", deviceName);
    root->setProperty(
        "follower_mode_id",
        followerModeSelector.getSelectedId());
    root->setProperty(
        "pause_on_chord_mismatch",
        pauseOnWrongChordToggle.getToggleState());
    root->setProperty(
        "follow_performance_tempo",
        followPerformanceTempoToggle.getToggleState());
    root->setProperty(
        "expression_strength",
        mode1Controller.getExpressionStrength());
    root->setProperty(
        "manual_key_shift",
        mode1Controller.getManualKeyShift());
    root->setProperty(
        "selected_key_anchor",
        mode1Controller.getSelectedKeyAnchor());
    root->setProperty(
        "measured_latency_applied",
        measuredLatencyApplied.load());
    const double inputLatencySeconds =
        measuredLatencyApplied.load()
            ? effectiveInputLatencySeconds.load()
            : reportedInputLatencySamples / currentSampleRate;
    const double outputLatencySeconds =
        measuredLatencyApplied.load()
            ? effectiveOutputLatencySeconds.load()
            : reportedOutputLatencySamples / currentSampleRate;
    root->setProperty(
        "input_latency_ms", inputLatencySeconds * 1000.0);
    root->setProperty(
        "output_latency_ms", outputLatencySeconds * 1000.0);
    root->setProperty(
        "trace_dropped_events",
        static_cast<juce::int64>(
            mode1Controller.getDroppedRealtimeTraceCount()));
    root->setProperty(
        "guitar_wav",
        getGuitarTestRecordingFile().getFullPathName());
    root->setProperty(
        "vocal_wav",
        getPerformanceOutputRecordingFile().getFullPathName());
    root->setProperty(
        "mix_wav",
        getPerformanceMixRecordingFile().getFullPathName());
    root->setProperty("trace_csv", traceFile.getFullPathName());
    if (!getPerformanceSessionFile().replaceWithText(
            juce::JSON::toString(juce::var(root), true),
            false,
            false,
            "\n"))
        return juce::Result::fail("Could not save performance session.");
    return juce::Result::ok();
}

juce::Result MainComponent::createPerformanceMix(
    const juce::File& guitarFile,
    const juce::File& vocalFile,
    const juce::File& destination) const
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> guitarReader(
        formats.createReaderFor(guitarFile));
    std::unique_ptr<juce::AudioFormatReader> vocalReader(
        formats.createReaderFor(vocalFile));
    if (guitarReader == nullptr || vocalReader == nullptr)
        return juce::Result::fail(L"기타 또는 보컬 WAV를 읽을 수 없습니다.");
    if (std::abs(guitarReader->sampleRate - vocalReader->sampleRate) > 1.0)
        return juce::Result::fail(L"기타와 보컬 WAV의 샘플레이트가 다릅니다.");

    const int numSamples = static_cast<int>(std::max(
        guitarReader->lengthInSamples, vocalReader->lengthInSamples));
    if (numSamples <= 0)
        return juce::Result::fail(L"합칠 오디오가 없습니다.");

    juce::AudioBuffer<float> guitar(1, numSamples);
    juce::AudioBuffer<float> vocal(1, numSamples);
    guitar.clear();
    vocal.clear();
    guitarReader->read(
        &guitar, 0, numSamples, 0, true, false);
    vocalReader->read(
        &vocal, 0, numSamples, 0, true, false);

    const float guitarPeak = guitar.getMagnitude(0, 0, numSamples);
    const float vocalPeak = vocal.getMagnitude(0, 0, numSamples);
    const float guitarGain = guitarPeak > 1.0e-5f
        ? juce::jmin(8.0f, 0.58f / guitarPeak)
        : 1.0f;
    const float vocalGain = vocalPeak > 1.0e-5f
        ? juce::jmin(8.0f, 0.72f / vocalPeak)
        : 1.0f;
    juce::AudioBuffer<float> mix(2, numSamples);
    for (int sample = 0; sample < numSamples; ++sample)
    {
        const float mixed = std::tanh(
            guitar.getSample(0, sample) * guitarGain
            + vocal.getSample(0, sample) * vocalGain);
        mix.setSample(0, sample, mixed);
        mix.setSample(1, sample, mixed);
    }

    if (destination.existsAsFile() && !destination.deleteFile())
        return juce::Result::fail(L"기존 믹스 WAV를 덮어쓸 수 없습니다.");
    std::unique_ptr<juce::OutputStream> stream =
        destination.createOutputStream();
    if (stream == nullptr)
        return juce::Result::fail(L"믹스 WAV를 만들 수 없습니다.");

    juce::WavAudioFormat wav;
    const auto options = juce::AudioFormatWriterOptions()
        .withSampleRate(guitarReader->sampleRate)
        .withNumChannels(2)
        .withBitsPerSample(24);
    auto writer = wav.createWriterFor(stream, options);
    if (writer == nullptr
        || !writer->writeFromAudioSampleBuffer(mix, 0, numSamples))
        return juce::Result::fail(L"믹스 WAV 저장에 실패했습니다.");
    return juce::Result::ok();
}

void MainComponent::refreshGuitarTestControls()
{
    const bool modeReady =
        activeMode == ActiveMode::mode1 && mode1Controller.hasSong();
    const bool recording = guitarTestRecorder.isRecording();
    const bool replaying = guitarReplayActive.load();
    const bool hasRecording =
        guitarReplayAudio.getNumSamples() > 0
        || getGuitarTestRecordingFile().existsAsFile();
    guitarRecordStartButton.setEnabled(
        modeReady && !recording && !replaying);
    guitarRecordStopButton.setEnabled(modeReady && recording);
    guitarReplayStartButton.setEnabled(
        modeReady && hasRecording && !recording && !replaying);
    guitarReplayStopButton.setEnabled(modeReady && replaying);
}

void MainComponent::startGuitarTestRecording()
{
    if (activeMode != ActiveMode::mode1
        || !mode1Controller.hasSong()
        || guitarTestRecorder.isRecording())
        return;

    bool guitarRecorderStarted = false;
    bool outputRecorderStarted = false;
    {
        // All three timelines must begin at the same audio callback.
        const juce::ScopedLock callbackLock(
            deviceManager.getAudioCallbackLock());
        guitarReplayActive.store(false);
        guitarReplayFinished.store(false);
        guitarRecorderStarted = guitarTestRecorder.start();
        if (guitarRecorderStarted)
            outputRecorderStarted = performanceOutputRecorder.start();
        if (guitarRecorderStarted && outputRecorderStarted)
        {
            mode1Controller.setRealtimeTraceEnabled(true);
            mode1Controller.restartPerformance();
        }
        else
        {
            guitarTestRecorder.stop();
            performanceOutputRecorder.stop();
            mode1Controller.setRealtimeTraceEnabled(false);
        }
    }
    if (!guitarRecorderStarted)
    {
        mode1StatusLabel.setText(
            L"기타 테스트 녹음을 시작하지 못했습니다.",
            juce::dontSendNotification);
        return;
    }
    if (!outputRecorderStarted)
    {
        mode1StatusLabel.setText(
            L"보컬 출력 녹음을 시작하지 못했습니다.",
            juce::dontSendNotification);
        return;
    }

    automaticPlaybackButton.setToggleState(
        false, juce::dontSendNotification);
    automaticPlaybackButton.setButtonText(L"자동 연주 시작");
    lyricLabel.setText(
        L"기타 입력 녹음 중 · 악보 처음부터 연주하세요",
        juce::dontSendNotification);
    mode1StatusLabel.setText(
        L"기타 입력과 보컬 출력 동시 녹음 중: Input "
            + juce::String(guitarChannelIndex.load() + 1),
        juce::dontSendNotification);
    refreshTransportControls();
    refreshGuitarTestControls();
}

void MainComponent::stopGuitarTestRecording()
{
    if (!guitarTestRecorder.isRecording())
        return;

    {
        const juce::ScopedLock callbackLock(
            deviceManager.getAudioCallbackLock());
        guitarTestRecorder.stop();
        performanceOutputRecorder.stop();
        mode1Controller.setRealtimeTraceEnabled(false);
    }
    const auto destination = getGuitarTestRecordingFile();
    if (destination.getParentDirectory().createDirectory().failed())
    {
        mode1StatusLabel.setText(
            L"기타 테스트 녹음 폴더를 만들 수 없습니다.",
            juce::dontSendNotification);
        refreshGuitarTestControls();
        return;
    }

    const auto result = guitarTestRecorder.saveAsWav(destination, false);
    if (result.failed())
    {
        mode1StatusLabel.setText(
            L"기타 테스트 WAV 저장 실패: "
                + result.getErrorMessage(),
            juce::dontSendNotification);
        refreshGuitarTestControls();
        return;
    }

    const auto outputDestination = getPerformanceOutputRecordingFile();
    const auto outputResult =
        performanceOutputRecorder.saveAsWav(outputDestination, false);
    if (outputResult.failed())
    {
        mode1StatusLabel.setText(
            L"기타 WAV는 저장했지만 보컬 출력 저장 실패: "
                + outputResult.getErrorMessage(),
            juce::dontSendNotification);
        refreshGuitarTestControls();
        return;
    }

    lastGuitarTestRecording = destination;
    lastPerformanceOutputRecording = outputDestination;
    const auto mixDestination = getPerformanceMixRecordingFile();
    const auto mixResult = createPerformanceMix(
        destination, outputDestination, mixDestination);
    if (mixResult.failed())
    {
        mode1StatusLabel.setText(
            L"기타와 보컬은 저장했지만 합본 생성 실패: "
                + mixResult.getErrorMessage(),
            juce::dontSendNotification);
        refreshGuitarTestControls();
        return;
    }
    const auto diagnosticsResult = savePerformanceDiagnostics();
    if (diagnosticsResult.failed())
    {
        mode1StatusLabel.setText(
            L"Performance WAV saved, but diagnostics failed: "
                + diagnosticsResult.getErrorMessage(),
            juce::dontSendNotification);
        refreshGuitarTestControls();
        return;
    }
    const bool loaded = loadGuitarTestReplay(destination);
    mode1StatusLabel.setText(
        loaded
            ? L"기타+보컬 합본 저장 완료: "
                + mixDestination.getFullPathName()
            : L"WAV는 저장했지만 테스트 재생용 로드에 실패했습니다.",
        juce::dontSendNotification);
    lyricLabel.setText(
        L"녹음으로 테스트를 누르면 같은 연주를 처음부터 재현합니다",
        juce::dontSendNotification);
    refreshGuitarTestControls();
}

bool MainComponent::loadGuitarTestReplay(const juce::File& file)
{
    if (!file.existsAsFile())
        return false;

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(
        formats.createReaderFor(file));
    if (reader == nullptr || reader->lengthInSamples <= 0)
        return false;

    juce::AudioBuffer<float> source(
        1, static_cast<int>(reader->lengthInSamples));
    if (!reader->read(
            &source,
            0,
            source.getNumSamples(),
            0,
            true,
            true))
        return false;

    const double targetRate = std::max(1.0, currentSampleRate);
    const double ratio = targetRate / reader->sampleRate;
    const int outputSamples = std::max(
        1,
        juce::roundToInt(source.getNumSamples() * ratio));
    juce::AudioBuffer<float> loaded(1, outputSamples);
    if (std::abs(reader->sampleRate - targetRate) < 0.5)
    {
        loaded.copyFrom(0, 0, source, 0, 0, outputSamples);
    }
    else
    {
        const double inputStep = reader->sampleRate / targetRate;
        const auto* input = source.getReadPointer(0);
        auto* output = loaded.getWritePointer(0);
        for (int sample = 0; sample < outputSamples; ++sample)
        {
            const double position = sample * inputStep;
            const int index = juce::jlimit(
                0,
                source.getNumSamples() - 1,
                static_cast<int>(position));
            const int next =
                std::min(index + 1, source.getNumSamples() - 1);
            const float fraction =
                static_cast<float>(position - index);
            output[sample] =
                input[index] + fraction * (input[next] - input[index]);
        }
    }

    const juce::ScopedLock callbackLock(
        deviceManager.getAudioCallbackLock());
    guitarReplayActive.store(false);
    guitarReplayPosition = 0;
    guitarReplayAudio = std::move(loaded);
    return true;
}

void MainComponent::startGuitarTestReplay()
{
    if (activeMode != ActiveMode::mode1
        || !mode1Controller.hasSong()
        || guitarTestRecorder.isRecording())
        return;

    if (guitarReplayAudio.getNumSamples() == 0)
    {
        const auto file = getGuitarTestRecordingFile();
        if (!loadGuitarTestReplay(file))
        {
            mode1StatusLabel.setText(
                L"재생할 기타 테스트 녹음이 없습니다.",
                juce::dontSendNotification);
            refreshGuitarTestControls();
            return;
        }
        lastGuitarTestRecording = file;
    }

    {
        const juce::ScopedLock callbackLock(
            deviceManager.getAudioCallbackLock());
        guitarReplayPosition = 0;
        guitarReplayFinished.store(false);
        guitarReplayActive.store(true);
        mode1Controller.restartPerformance();
    }
    automaticPlaybackButton.setToggleState(
        false, juce::dontSendNotification);
    automaticPlaybackButton.setButtonText(L"자동 연주 시작");
    lyricLabel.setText(
        L"저장된 기타 입력으로 Mode 1 테스트 중",
        juce::dontSendNotification);
    mode1StatusLabel.setText(
        L"가상 기타 입력 재생: "
            + (
                lastGuitarTestRecording.existsAsFile()
                    ? lastGuitarTestRecording.getFileName()
                    : juce::String("bansanka_last_guitar_take.wav")),
        juce::dontSendNotification);
    refreshTransportControls();
    refreshGuitarTestControls();
}

void MainComponent::stopGuitarTestReplay()
{
    {
        const juce::ScopedLock callbackLock(
            deviceManager.getAudioCallbackLock());
        guitarReplayActive.store(false);
        guitarReplayFinished.store(false);
        guitarReplayPosition = 0;
        mode1Controller.stopPerformance();
    }
    lyricLabel.setText(
        L"기타 녹음 테스트 중지됨",
        juce::dontSendNotification);
    mode1StatusLabel.setText(
        L"저장된 기타 입력 테스트를 중지했습니다.",
        juce::dontSendNotification);
    refreshTransportControls();
    refreshGuitarTestControls();
}

void MainComponent::showLanding()
{
    // 두 모드 모두 화면을 떠날 때 소리와 파일을 정리한다. 오디오 콜백은 activeMode를
    // 보고 즉시 무음으로 전환하지만, 컨트롤러 설정은 남겨 다시 들어왔을 때 이어 쓴다.
    if (guidedRecordingSession.isActive())
        cancelGuidedRecordingSession();

    {
        const juce::ScopedLock callbackLock(deviceManager.getAudioCallbackLock());
        mode1Controller.stopPerformance();
        guitarReplayActive.store(false);
        guitarTestRecorder.stop();
        performanceOutputRecorder.stop();
    }
    automaticPlaybackButton.setToggleState(false, juce::dontSendNotification);
    automaticPlaybackButton.setButtonText(L"자동 연주 시작");
    setVirtualControlsEnabled(false);
    refreshGuitarTestControls();

    stopRecording();
    mode2Screen.setRecordingState(false);

    activeMode = ActiveMode::landing;
    mode1Panel.setVisible(false);
    mode2Screen.setVisible(false);
    statusLabel.setVisible(false);
    mode1Button.setVisible(true);
    mode2Button.setVisible(true);
    audioSettingsButton.setVisible(true);
    updateAudioDeviceStatus();
    resized();
    repaint();
}

void MainComponent::showMode2()
{
    // 이전 캘리브레이션이 게인 상한을 낮게 잠근 상태로 남아 무음처럼 들리는 일을 막는다.
    mode2Controller.resetCalibration();
    mode2Screen.showOverview();
    updateLatencyInfo();

    activeMode = ActiveMode::mode2;
    mode1Panel.setVisible(false);
    statusLabel.setVisible(false);
    mode1Button.setVisible(false);
    mode2Button.setVisible(false);
    audioSettingsButton.setVisible(false);
    mode2Screen.setVisible(true);
    resized();
    repaint();
}

void MainComponent::showMode1()
{
    if (guidedRecordingSession.isActive())
        cancelGuidedRecordingSession();

    activeMode = ActiveMode::mode1;
    mode2Screen.setVisible(false);
    statusLabel.setVisible(false);
    mode1Button.setVisible(false);
    mode2Button.setVisible(false);
    audioSettingsButton.setVisible(false);
    mode1Panel.setVisible(true);
    resized();
    repaint();

    keyShiftSlider.setRange(
        static_cast<double>(mode1Controller.getMinimumManualKeyShift()),
        static_cast<double>(mode1Controller.getMaximumManualKeyShift()),
        1.0);
    expressionSlider.setEnabled(
        mode1Controller.hasMultipleExpressionStrengths());
    expressionLabel.setText(
        mode1Controller.hasMultipleExpressionStrengths()
            ? L"내 스타일  ←  원곡 표현"
            : L"표현 강도 25% · 사용자 맞춤으로 고정",
        juce::dontSendNotification);
    expressionSlider.setValue(
        mode1Controller.getDefaultExpressionStrength(),
        juce::sendNotificationSync);
    keyShiftSlider.setValue(0.0, juce::sendNotificationSync);
    mode1StatusLabel.setText(
        L"Mode 1 활성화: 기타 스트로크가 프레이즈 시작을 보정합니다.",
        juce::dontSendNotification);

    if (!mode1Controller.hasSong())
    {
        const auto developmentPackage = findDevelopmentSongPackage();
        if (developmentPackage.existsAsFile())
            loadSongPackage(developmentPackage);
    }
    setVirtualControlsEnabled(mode1Controller.hasSong());
    refreshGuitarTestControls();

    grabKeyboardFocus();
}

void MainComponent::chooseSongPackage()
{
    packageChooser = std::make_unique<juce::FileChooser>(
        L"Mode 1 song_package.json 선택",
        findDevelopmentSongPackage(),
        "*.json");
    packageChooser->launchAsync(
        juce::FileBrowserComponent::openMode
            | juce::FileBrowserComponent::canSelectFiles,
        [this](const juce::FileChooser& chooser)
        {
            const auto selected = chooser.getResult();
            if (selected.existsAsFile())
                loadSongPackage(selected);
        });
}

bool MainComponent::loadSongPackage(const juce::File& file)
{
    juce::String error;
    const juce::ScopedLock callbackLock(deviceManager.getAudioCallbackLock());
    if (!mode1Controller.loadSongPackage(file, error))
    {
        mode1StatusLabel.setText(L"곡 로드 실패: " + error, juce::dontSendNotification);
        nextPhraseButton.setEnabled(false);
        return false;
    }
    currentSongPackageFile = file;
    mode1Controller.stopPerformance();
    automaticPlaybackButton.setToggleState(
        false, juce::dontSendNotification);
    automaticPlaybackButton.setButtonText(L"자동 연주 시작");

    activeMode = ActiveMode::mode1;
    keyShiftSlider.setRange(
        static_cast<double>(mode1Controller.getMinimumManualKeyShift()),
        static_cast<double>(mode1Controller.getMaximumManualKeyShift()),
        1.0);
    expressionSlider.setEnabled(
        mode1Controller.hasMultipleExpressionStrengths());
    expressionLabel.setText(
        mode1Controller.hasMultipleExpressionStrengths()
            ? L"내 스타일  ←  원곡 표현"
            : L"표현 강도 25% · 사용자 맞춤으로 고정",
        juce::dontSendNotification);
    expressionSlider.setValue(
        mode1Controller.getDefaultExpressionStrength(),
        juce::sendNotificationSync);
    keyShiftSlider.setValue(0.0, juce::sendNotificationSync);
    songLabel.setText(
        L"곡 패키지: " + mode1Controller.getSongName()
            + L"  기준 키 "
            + (mode1Controller.getBaseKeyShift() >= 0 ? "+" : "")
            + juce::String(mode1Controller.getBaseKeyShift()) + " st"
            + (mode1Controller.getRangeWarning().isNotEmpty()
                ? L"  [주의] " + mode1Controller.getRangeWarning()
                : L""),
        juce::dontSendNotification);
    const auto packageDirectory = file.getParentDirectory().getFileName();
    if (packageDirectory == "bansanka")
        songSelector.setSelectedId(1, juce::dontSendNotification);
    else if (packageDirectory == "dont_look_back_in_anger")
        songSelector.setSelectedId(2, juce::dontSendNotification);
    else if (packageDirectory == "hype_boy")
        songSelector.setSelectedId(3, juce::dontSendNotification);
    else
        songSelector.setSelectedId(0, juce::dontSendNotification);
    lyricLabel.setText(
        L"시작 버튼을 누른 뒤 악보 첫 코드부터 연주하세요",
        juce::dontSendNotification);
    mode1StatusLabel.setText(
        L"Mode 1 준비 완료: " + file.getFullPathName(),
        juce::dontSendNotification);
    refreshTransportControls();
    refreshGuitarTestControls();
    grabKeyboardFocus();
    return true;
}

juce::File MainComponent::findDevelopmentSongPackage() const
{
    return findBundledSongPackage("bansanka");
}

juce::File MainComponent::findBundledSongPackage(
    const juce::String& songSlug) const
{
    const auto relativePath =
        juce::String("build/mode1/")
        + songSlug
        + "/song_package.json";
    const auto fromWorkingDirectory =
        juce::File::getCurrentWorkingDirectory().getChildFile(relativePath);
    if (fromWorkingDirectory.existsAsFile())
        return fromWorkingDirectory;

    auto executableDirectory =
        juce::File::getSpecialLocation(juce::File::currentApplicationFile)
            .getParentDirectory();
    for (int level = 0; level < 5; ++level)
    {
        const auto candidate = executableDirectory
            .getChildFile(
                juce::String("mode1/")
                + songSlug
                + "/song_package.json");
        if (candidate.existsAsFile())
            return candidate;
        executableDirectory = executableDirectory.getParentDirectory();
    }

    return fromWorkingDirectory;
}

juce::File MainComponent::findRepositoryRoot() const
{
    const auto marker = juce::String("tools/mode1_song_package/train_rvc_voice.py");

    auto candidate = juce::File::getCurrentWorkingDirectory();
    for (int level = 0; level < 6; ++level)
    {
        if (candidate.getChildFile(marker).existsAsFile())
            return candidate;
        candidate = candidate.getParentDirectory();
    }

    candidate = juce::File::getSpecialLocation(juce::File::currentApplicationFile)
        .getParentDirectory();
    for (int level = 0; level < 6; ++level)
    {
        if (candidate.getChildFile(marker).existsAsFile())
            return candidate;
        candidate = candidate.getParentDirectory();
    }

    return juce::File::getCurrentWorkingDirectory();
}

void MainComponent::startVoiceModelTraining()
{
    if (voiceModelTrainer.isBusy())
        return;
    if (profileDirectory == juce::File() || !profileDirectory.isDirectory())
        return;
    if (acceptedClipCount == 0 || acceptedDurationSeconds < 180.0)
    {
        guidedStatusIsError = true;
        guidedStatusMessage =
            L"재학습하려면 통과한 음성이 최소 180초 필요합니다. 현재 "
            + juce::String(acceptedDurationSeconds, 0) + L"초입니다.";
        return;
    }

    trainingRequestedForSession = true;

    const auto repositoryRoot = findRepositoryRoot();
    const auto pythonExecutable = repositoryRoot.getChildFile(
        "external/seed-vc/venv/Scripts/python.exe");

    voice_capture::VoiceModelTrainer::Config config;
    config.pythonExecutable = pythonExecutable;
    config.repositoryRoot = repositoryRoot;
    config.profileDirectory = profileDirectory;
    config.songPackage = findDevelopmentSongPackage();
    config.sshHost = "root@172.10.5.154";
    config.experimentName = "rvc_user_" + profileDirectory.getFileName();
    voiceModelTrainer.start(std::move(config));
}

void MainComponent::triggerNextPhrase()
{
    if (activeMode == ActiveMode::mode1
        && mode1Controller.hasSong()
        && mode1Controller.isPerformanceRunning())
        mode1Controller.triggerNextPhrase();
    grabKeyboardFocus();
}

bool MainComponent::keyPressed(const juce::KeyPress& key)
{
    // 모드 1에서만 스페이스를 가져간다. 모드 2 화면의 위젯이 스페이스로 조작될 때
    // 여기서 먼저 삼켜 버리면 안 된다.
    if (activeMode == ActiveMode::mode1 && key == juce::KeyPress::spaceKey)
    {
        triggerNextPhrase();
        return true;
    }
    return false;
}

juce::StringArray MainComponent::loadSongLyricLines() const
{
    // Reuses whatever local song_package.json the Mode 1 pipeline already
    // produced (see findDevelopmentSongPackage()) so the guided song stage
    // can show a Korean-readable line per phrase. This file is generated
    // by the user's own local tooling and is not tracked in git.
    //
    // We deliberately only read an optional "lyrics_reading_ko" field per
    // phrase — a Korean phonetic reading the user adds themselves (e.g.
    // via any kana/romaji-to-Hangul converter they choose) — and never the
    // original "lyrics" text directly, so this app never auto-generates or
    // displays a transliteration of someone else's lyrics on its own; it
    // only shows readings the user has explicitly supplied. If no phrase
    // has that field filled in yet, the song stage falls back to the
    // built-in generic placeholder lines.
    const auto packageFile = findDevelopmentSongPackage();
    if (!packageFile.existsAsFile())
        return {};

    juce::var root;
    if (juce::JSON::parse(packageFile.loadFileAsString(), root).failed())
        return {};

    const auto* rootObject = root.getDynamicObject();
    if (rootObject == nullptr)
        return {};

    // "phrases" (line-level granularity) is the natural unit for a
    // Korean reading line, unlike the finer "micro_phrases" breakdown —
    // this also matches tools/mode1_song_package/apply_lyrics_reading.py's
    // default target.
    const auto* phraseArray = rootObject->getProperty("phrases").getArray();
    if (phraseArray == nullptr)
        return {};

    juce::StringArray lines;
    juce::String previousLine;
    for (const auto& phraseValueItem : *phraseArray)
    {
        const auto* phraseObject = phraseValueItem.getDynamicObject();
        if (phraseObject == nullptr)
            continue;
        const auto reading =
            phraseObject->getProperty("lyrics_reading_ko").toString().trim();
        if (reading.isEmpty() || reading == previousLine)
            continue;
        lines.add(reading);
        previousLine = reading;
        if (lines.size() >= 60)
            break;
    }
    return lines;
}

void MainComponent::startGuidedRecordingSession()
{
    if (guidedRecordingSession.isActive()
        || recordingPreflightActive
        || voiceModelTrainer.isBusy())
        return;

    trainingRequestedForSession = false;
    profileDirectory = juce::File();
    manifestClips.clear();
    clipIndex = 0;
    acceptedClipCount = 0;
    acceptedDurationSeconds = 0.0;
    profileProgressValue = 0.0;

    mode1Controller.reset();
    automaticPlaybackButton.setToggleState(
        false, juce::dontSendNotification);
    automaticPlaybackButton.setButtonText(L"자동 연주 시작");
    setVirtualControlsEnabled(false);
    guitarTestRecorder.stop();
    performanceOutputRecorder.stop();
    guitarReplayActive.store(false);
    refreshGuitarTestControls();

    lastReferenceFile = juce::File();
    guidedStatusMessage = {};
    guidedStatusIsError = false;
    guidedRecordingSession.setSongLines(loadSongLyricLines());
    recordingPreflightActive = true;
    inputLevelCalibrator.start();

    mode1Button.setEnabled(false);
    mode2Button.setEnabled(false);
    mode1HomeButton.setEnabled(false);
    recordButton.setEnabled(false);

    recordingSessionScreen.setBounds(getLocalBounds());
    recordingSessionScreen.setVisible(true);
    recordingSessionScreen.toFront(false);
}

void MainComponent::cancelGuidedRecordingSession()
{
    if (voiceRecorder.isRecording())
        voiceRecorder.stop();
    guidedRecordingSession.stop();
    recordingPreflightActive = false;
    inputLevelCalibrator.cancel();

    recordingSessionScreen.setVisible(false);
    mode1Button.setEnabled(true);
    mode2Button.setEnabled(true);
    mode1HomeButton.setEnabled(true);
    recordButton.setEnabled(true);

    recordingStatusLabel.setColour(
        juce::Label::textColourId,
        guitaru::inkMuted());
    recordingStatusLabel.setText(
        L"가이드 녹음을 마쳤습니다. 지금까지 통과한 클립 "
            + juce::String(acceptedClipCount) + L"개가 저장되어 있습니다.",
        juce::dontSendNotification);
}

void MainComponent::finalizeGuidedItem(const voice_capture::GuidedRecordingState&)
{
    voiceRecorder.stop();

    auto quality = voiceRecorder.getQuality();
    const bool usableContinuousTake =
        quality.durationSeconds >= 180.0
        && quality.activeSpeechSeconds >= 30.0
        && quality.clippingRatio <= 0.05f;
    if (!quality.passed && usableContinuousTake)
    {
        quality.passed = true;
        quality.hasWarning = true;
        quality.summary =
            L"연속 녹음은 저장했습니다 · 최종 품질 수치는 참고용입니다";
    }
    const auto details =
        juce::String(quality.durationSeconds, 1) + L"초 · 평균 "
        + juce::String(quality.rmsDb, 1) + L" dBFS · SNR "
        + juce::String(quality.snrDb, 1) + L" dB";

    const auto profile = ensureProfileDirectory();
    const auto rawDirectory = profile.getChildFile("raw");
    const auto acceptedDirectory = profile.getChildFile("accepted");
    rawDirectory.createDirectory();
    acceptedDirectory.createDirectory();

    ++clipIndex;
    const auto baseName = getRecordingCategorySlug()
        + "_" + juce::String(clipIndex).paddedLeft('0', 3) + ".wav";
    const auto rawFile = rawDirectory.getChildFile(baseName);
    const auto rawSaveResult = voiceRecorder.saveAsWav(rawFile, false);
    if (rawSaveResult.failed())
    {
        guidedStatusIsError = true;
        guidedStatusMessage = L"원본 저장 실패: " + rawSaveResult.getErrorMessage();
        guidedRecordingSession.notifyItemFinalized();
        return;
    }

    if (!quality.passed)
    {
        writeManifestEntry(rawFile, {}, quality, false);
        guidedStatusIsError = true;
        guidedStatusMessage =
            L"품질 검사를 통과하지 못해 원본만 보관했습니다 · " + details
                + L" · " + quality.summary;
        guidedRecordingSession.notifyItemRejected();
        return;
    }

    lastReferenceFile = acceptedDirectory.getChildFile(baseName);
    const bool reduceNoise = noiseReductionToggle.getToggleState();
    const auto saveResult =
        voiceRecorder.saveAsWav(lastReferenceFile, reduceNoise);

    if (saveResult.failed())
    {
        guidedStatusIsError = true;
        guidedStatusMessage = L"저장 실패: " + saveResult.getErrorMessage();
        guidedRecordingSession.notifyItemFinalized();
        return;
    }

    ++acceptedClipCount;
    acceptedDurationSeconds += quality.durationSeconds;
    profileProgressValue =
        juce::jlimit(0.0, 1.0, acceptedDurationSeconds / 180.0);
    profileProgressLabel.setText(
        L"유효 음성 " + juce::String(acceptedDurationSeconds, 0)
            + L"초 / 최소 180초 · 통과 클립 "
            + juce::String(acceptedClipCount) + L"개",
        juce::dontSendNotification);
    writeManifestEntry(rawFile, lastReferenceFile, quality, reduceNoise);

    guidedStatusIsError = false;
    guidedStatusMessage = L"좋아요! " + details;
    guidedRecordingSession.notifyItemFinalized();
}

juce::File MainComponent::ensureProfileDirectory()
{
    if (profileDirectory != juce::File())
        return profileDirectory;

    const auto profileId =
        juce::Time::getCurrentTime().formatted("%Y%m%d_%H%M%S");
    profileDirectory =
        juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
            .getChildFile("VocalGuitarApp")
            .getChildFile("voice_profiles")
            .getChildFile(profileId);
    profileDirectory.createDirectory();
    return profileDirectory;
}

juce::String MainComponent::getRecordingCategorySlug() const
{
    return guidedRecordingSession.getCurrentCategorySlug();
}

void MainComponent::writeManifestEntry(
    const juce::File& rawFile,
    const juce::File& acceptedFile,
    const voice_capture::RecordingQuality& quality,
    bool noiseReduced)
{
    auto* clip = new juce::DynamicObject();
    clip->setProperty("file", acceptedFile == juce::File()
        ? juce::String()
        : acceptedFile.getRelativePathFrom(profileDirectory).replaceCharacter('\\', '/'));
    clip->setProperty(
        "raw_file",
        rawFile.getRelativePathFrom(profileDirectory).replaceCharacter('\\', '/'));
    clip->setProperty("category", getRecordingCategorySlug());
    clip->setProperty("duration_sec", quality.durationSeconds);
    clip->setProperty("peak_dbfs", quality.peakDb);
    clip->setProperty("snr_db", quality.snrDb);
    clip->setProperty("voiced_ratio", quality.activeSpeechRatio);
    clip->setProperty("quality", quality.passed ? "pass" : "fail");
    clip->setProperty("noise_reduced", noiseReduced);
    manifestClips.add(juce::var(clip));

    auto* root = new juce::DynamicObject();
    root->setProperty("schema_version", 1);
    root->setProperty("profile_id", profileDirectory.getFileName());
    root->setProperty(
        "created_at",
        juce::Time::getCurrentTime().toISO8601(true));
    root->setProperty("input_sample_rate", currentSampleRate);
    root->setProperty("calibrated_room_tone_dbfs", calibratedRoomToneDb);
    root->setProperty("recording_input_gain", calibratedInputGain);
    root->setProperty("storage_format", "WAV PCM, mono, 48000 Hz, 24-bit");
    root->setProperty("clips", juce::var(manifestClips));
    profileDirectory.getChildFile("manifest.json").replaceWithText(
        juce::JSON::toString(juce::var(root), true),
        false,
        false,
        "\n");
}

void MainComponent::timerCallback()
{
    if (audioLatencyCalibrator.isAnalysisPending())
    {
        lastLatencyResult =
            audioLatencyCalibrator.analyseCompletedCapture();
        hasLatencyResult = lastLatencyResult.valid;
        if (auto* device = deviceManager.getCurrentAudioDevice())
        {
            reportedInputLatencySamples =
                device->getInputLatencyInSamples();
            reportedOutputLatencySamples =
                device->getOutputLatencyInSamples();
            latencyMeasurementDeviceName = device->getName();
            latencyMeasurementSampleRate = currentSampleRate;
            latencyMeasurementBufferSize =
                device->getCurrentBufferSizeSamples();
        }

        measureLatencyButton.setEnabled(true);
        applyLatencyButton.setEnabled(hasLatencyResult);
        if (hasLatencyResult)
        {
            refreshLatencyDisplay();
        }
        else
        {
            latencyStatusLabel.setColour(
                juce::Label::textColourId, juce::Colours::orange);
            latencyStatusLabel.setText(
                lastLatencyResult.message,
                juce::dontSendNotification);
        }
        refreshTransportControls();
    }

    if (recordingSessionScreen.isVisible())
    {
        if (recordingPreflightActive)
        {
            const auto calibration = inputLevelCalibrator.getResult();
            const auto peakDb = juce::Decibels::gainToDecibels(
                inputLevelCalibrator.getInputPeak(), -60.0f);
            const float level01 =
                juce::jlimit(0.0f, 1.0f, (peakDb + 60.0f) / 60.0f);

            if (calibration.phase
                == voice_capture::InputLevelCalibrator::Phase::complete)
            {
                calibratedRoomToneDb = calibration.roomToneDb;
                calibratedInputGain = calibration.recommendedGain;
                voiceRecorder.setInputGain(
                    calibratedInputGain, calibratedRoomToneDb);
                recordingPreflightActive = false;
                guidedStatusIsError = false;
                guidedStatusMessage = calibration.message;
                guidedRecordingSession.start();
            }
            else
            {
                const bool failed = calibration.phase
                    == voice_capture::InputLevelCalibrator::Phase::failed;
                recordingSessionScreen.updateCalibrationState(
                    calibration.message,
                    calibration.progress,
                    level01,
                    failed);
                return;
            }
        }

        auto state = guidedRecordingSession.getState();

        if (state.awaitingFinalize)
        {
            finalizeGuidedItem(state);
            state = guidedRecordingSession.getState();
        }

        const auto peakDb = juce::Decibels::gainToDecibels(
            liveMicrophonePeak.load() * voiceRecorder.getInputGain(),
            -60.0f);
        const float level01 = juce::jlimit(
            0.0f, 1.0f, (peakDb + 60.0f) / 60.0f);

        recordingSessionScreen.updateState(
            state, level01, guidedStatusMessage, guidedStatusIsError);

        if (state.sessionFinished)
        {
            if (!trainingRequestedForSession
                && acceptedClipCount > 0
                && acceptedDurationSeconds >= 180.0)
            {
                guidedStatusMessage =
                    L"녹음 파일 저장 완료 · 재학습을 자동으로 시작합니다.";
                guidedStatusIsError = false;
                startVoiceModelTraining();
            }

            const bool trainingFinished =
                trainingRequestedForSession && voiceModelTrainer.isFinished();
            const bool trainingSucceeded =
                trainingFinished && voiceModelTrainer.didSucceed();
            recordingStatusLabel.setColour(
                juce::Label::textColourId,
                trainingFinished && !trainingSucceeded
                    ? juce::Colours::orange
                    : juce::Colours::lightgreen);
            recordingStatusLabel.setText(
                trainingSucceeded
                    ? L"[완료] 새 목소리 학습 및 노래 적용 완료"
                    : trainingFinished
                        ? L"[실패] 재학습 또는 노래 변환 실패"
                        : trainingRequestedForSession
                            ? L"[진행 중] 모델 재학습 및 노래 변환 중"
                            : L"[대기] 녹음 저장 완료",
                juce::dontSendNotification);

            recordingSessionScreen.updateTrainingStatus(
                trainingFinished && !voiceModelTrainer.didSucceed(),
                voiceModelTrainer.isBusy(),
                trainingFinished,
                trainingSucceeded,
                !trainingRequestedForSession
                        && acceptedDurationSeconds < 180.0
                    ? L"재학습까지 "
                        + juce::String(
                            juce::jmax(0.0, 180.0 - acceptedDurationSeconds),
                            0)
                        + L"초의 통과 음성이 더 필요합니다."
                    : voiceModelTrainer.getLatestStatusLine());
        }
        return;
    }

    if (activeMode != ActiveMode::mode1 || !mode1Controller.hasSong())
        return;

    const auto lyrics = mode1Controller.getCurrentLyrics();
    if (lyrics.isNotEmpty())
        lyricLabel.setText(lyrics, juce::dontSendNotification);

    const auto rmsDb = juce::Decibels::gainToDecibels(
        mode1Controller.getGuitarRms(),
        -100.0f);
    const auto rawInputDb = juce::Decibels::gainToDecibels(
        liveGuitarPeak.load(),
        -100.0f);
    guitarStatusLabel.setText(
        (
            guitarReplayActive.load()
                ? juce::String(L"WAV 테스트 입력")
                : L"Input " + juce::String(guitarChannelIndex.load() + 1))
            + L" · " + juce::String(rawInputDb, 1) + L" dBFS · "
            + juce::String(mode1Controller.isGuitarActive()
                ? L"기타 입력 감지"
                : L"기타 입력 대기")
            + "  RMS " + juce::String(rmsDb, 1) + " dBFS"
            + L"  가이드 코드 " + mode1Controller.getDetectedChordName()
            + L"  (입력 추정 "
            + mode1Controller.getRawDetectedChordName() + ")"
            + L"  보컬 " + (mode1Controller.getPitchShiftSemitones() >= 0 ? L"+" : L"")
            + juce::String(mode1Controller.getPitchShiftSemitones()) + " st"
            + L"  템포 x"
            + juce::String(
                mode1Controller.getPerformanceTempoScale(), 2)
            + (
                mode1Controller.isFollowPerformanceTempoEnabled()
                    ? L"  보컬 길이 x"
                        + juce::String(
                            mode1Controller.getVocalDurationScale(), 2)
                    : juce::String())
            + (
                mode1Controller.getRecoveredSkippedChordCount() > 0
                    ? L"  재동기화 "
                        + juce::String(
                            mode1Controller
                                .getRecoveredSkippedChordCount())
                    : juce::String())
            + (
                mode1Controller.isPausedForChordMismatch()
                    ? L"  [코드 오류 · 일시정지]"
                    : juce::String())
            + (mode1Controller.consumedOnset() ? "  [ONSET]" : ""),
        juce::dontSendNotification);
}

void MainComponent::paint(juce::Graphics& graphics)
{
    guitaru::drawPaperTexture(graphics, getLocalBounds(), 20260730);

    if (activeMode != ActiveMode::landing)
        return;

    const auto bounds = getLocalBounds().toFloat();
    const float scale = juce::jlimit(0.72f, 1.0f, bounds.getWidth() / 1120.0f);

    if (logoImage.isValid())
    {
        graphics.setOpacity(1.0f);
        graphics.drawImage(logoImage,
                           juce::roundToInt(58.0f * scale), juce::roundToInt(132.0f * scale),
                           juce::roundToInt(540.0f * scale), juce::roundToInt(206.0f * scale),
                           0, 0, logoImage.getWidth(), logoImage.getHeight(), false);
    }

    graphics.setColour(guitaru::green());
    graphics.setFont(guitaru::chalkFont(29.0f * scale, true));
    graphics.drawText(utf8("기타를 치면, 목소리가 따라와요."),
                      juce::Rectangle<float>(70.0f * scale, 365.0f * scale,
                                             525.0f * scale, 40.0f * scale),
                      juce::Justification::centredLeft);

    if (mascotImage.isValid())
    {
        graphics.setOpacity(1.0f);
        graphics.drawImage(mascotImage,
                           juce::roundToInt(625.0f * scale), juce::roundToInt(105.0f * scale),
                           juce::roundToInt(440.0f * scale), juce::roundToInt(590.0f * scale),
                           0, 0, mascotImage.getWidth(), mascotImage.getHeight(), false);
    }
}

void MainComponent::resized()
{
    const auto area = getLocalBounds();

    // 세 화면이 같은 영역을 나눠 쓴다. 보이지 않는 화면에도 bounds를 넣어두면
    // 다시 들어올 때 레이아웃이 한 프레임 늦게 잡히는 일이 없다.
    mode1Panel.setBounds(area);
    mode2Screen.setBounds(area);
    recordingSessionScreen.setBounds(area);
    layoutMode1Panel();

    if (activeMode != ActiveMode::landing)
        return;

    const float scale = juce::jlimit(0.72f, 1.0f,
                                     static_cast<float>(getWidth()) / 1120.0f);
    const auto rect = [scale](float x, float y, float w, float h)
    {
        return juce::Rectangle<int>(juce::roundToInt(x * scale), juce::roundToInt(y * scale),
                                    juce::roundToInt(w * scale), juce::roundToInt(h * scale));
    };

    mode1Button.setBounds(rect(70.0f, 475.0f, 330.0f, 60.0f));
    mode2Button.setBounds(rect(70.0f, 545.0f, 330.0f, 60.0f));
    audioSettingsButton.setBounds(rect(415.0f, 545.0f, 180.0f, 60.0f));
}

void MainComponent::layoutMode1Panel()
{
    auto area = mode1Panel.getLocalBounds().reduced(24);
    if (area.isEmpty())
        return;

    // --- 항상 보이는 것 -------------------------------------------------
    auto headerRow = area.removeFromTop(36);
    mode1HomeButton.setBounds(headerRow.removeFromLeft(96).reduced(2, 0));
    headerRow.removeFromLeft(12);
    advancedToggleButton.setBounds(headerRow.removeFromRight(150).reduced(2, 0));
    headerRow.removeFromRight(12);
    mode1StatusLabel.setBounds(headerRow);
    area.removeFromTop(14);

    auto songRow = area.removeFromTop(44);
    songSelector.setBounds(
        songRow.removeFromLeft(
            juce::roundToInt(songRow.getWidth() * 0.34f)).reduced(4, 0));
    followerModeSelector.setBounds(
        songRow.removeFromLeft(
            juce::roundToInt(songRow.getWidth() * 0.45f)).reduced(4, 0));
    mode1AudioSettingsButton.setBounds(songRow.reduced(4, 0));
    area.removeFromTop(10);

    songLabel.setBounds(area.removeFromTop(28));
    area.removeFromTop(8);

    // 가사가 이 화면의 주인공이다. 접힌 상태에서는 남는 공간을 가사에 준다.
    const int lyricHeight = showAdvancedControls
        ? 110
        : juce::jlimit(140, 300, area.getHeight() - 190);
    lyricLabel.setBounds(area.removeFromTop(lyricHeight));
    area.removeFromTop(10);

    guitarStatusLabel.setBounds(area.removeFromTop(30));
    area.removeFromTop(8);

    auto transportRow = area.removeFromTop(46);
    const int transportButtonWidth = transportRow.getWidth() / 3;
    startPerformanceButton.setBounds(
        transportRow.removeFromLeft(transportButtonWidth).reduced(4, 0));
    restartPerformanceButton.setBounds(
        transportRow.removeFromLeft(transportButtonWidth).reduced(4, 0));
    stopPerformanceButton.setBounds(transportRow.reduced(4, 0));
    area.removeFromTop(8);

    auto guitarTestRow = area.removeFromTop(40);
    guitarRecordStartButton.setBounds(
        guitarTestRow.removeFromLeft(guitarTestRow.getWidth() / 2).reduced(4, 0));
    guitarRecordStopButton.setBounds(guitarTestRow.reduced(4, 0));
    area.removeFromTop(10);

    // --- 접히는 것 ------------------------------------------------------
    if (! showAdvancedControls)
        return;

    auto extraRow = area.removeFromTop(36);
    loadSongButton.setBounds(extraRow.removeFromLeft(190).reduced(4, 0));
    nextPhraseButton.setBounds(extraRow.removeFromLeft(190).reduced(4, 0));
    automaticPlaybackButton.setBounds(extraRow.removeFromLeft(190).reduced(4, 0));
    guitarChannelSelector.setBounds(extraRow.reduced(4, 0));
    area.removeFromTop(5);

    auto replayRow = area.removeFromTop(36);
    guitarReplayStartButton.setBounds(
        replayRow.removeFromLeft(190).reduced(4, 0));
    guitarReplayStopButton.setBounds(
        replayRow.removeFromLeft(190).reduced(4, 0));
    area.removeFromTop(5);

    auto latencyButtonRow = area.removeFromTop(34);
    measureLatencyButton.setBounds(
        latencyButtonRow.removeFromLeft(150).reduced(4, 0));
    applyLatencyButton.setBounds(
        latencyButtonRow.removeFromLeft(145).reduced(4, 0));
    resetLatencyButton.setBounds(
        latencyButtonRow.removeFromLeft(130).reduced(4, 0));
    latencyStatusLabel.setBounds(latencyButtonRow.reduced(4, 0));
    area.removeFromTop(7);

    auto virtualHeader = area.removeFromTop(32);
    virtualChordLabel.setBounds(
        virtualHeader.removeFromLeft(
            std::max(120, virtualHeader.getWidth() - 450)).reduced(4, 0));
    pauseOnWrongChordToggle.setBounds(
        virtualHeader.removeFromLeft(220).reduced(4, 0));
    followPerformanceTempoToggle.setBounds(
        virtualHeader.reduced(4, 0));
    area.removeFromTop(4);

    for (int row = 0; row < 2; ++row)
    {
        auto chordRow = area.removeFromTop(30);
        const int buttonsRemaining = 6;
        for (int column = 0; column < buttonsRemaining; ++column)
        {
            const int index = row * buttonsRemaining + column;
            const int width =
                chordRow.getWidth() / (buttonsRemaining - column);
            virtualChordButtons[static_cast<size_t>(index)].setBounds(
                chordRow.removeFromLeft(width).reduced(3, 1));
        }
    }
    area.removeFromTop(10);

    auto expressionRow = area.removeFromTop(36);
    expressionLabel.setBounds(
        expressionRow.removeFromLeft(280).reduced(4, 0));
    expressionSlider.setBounds(expressionRow.reduced(4, 0));
    area.removeFromTop(12);

    auto keyShiftRow = area.removeFromTop(36);
    keyShiftLabel.setBounds(
        keyShiftRow.removeFromLeft(280).reduced(4, 0));
    keyShiftSlider.setBounds(keyShiftRow.reduced(4, 0));
    area.removeFromTop(12);

    recordingTitleLabel.setBounds(area.removeFromTop(28));
    auto recordingOptions = area.removeFromTop(34);
    microphoneChannelSelector.setBounds(
        recordingOptions.removeFromLeft(240).reduced(4, 0));
    noiseReductionToggle.setBounds(recordingOptions.reduced(6, 0));
    area.removeFromTop(8);

    recordButton.setBounds(area.removeFromTop(40).removeFromLeft(250).reduced(4, 0));
    area.removeFromTop(8);

    profileProgressLabel.setBounds(area.removeFromTop(22));
    profileProgressBar.setBounds(area.removeFromTop(14).reduced(4, 1));
    recordingStatusLabel.setBounds(area.removeFromTop(55));
}

// 모드 1 UI는 친구 쪽에서 JUCE 기본 룩앤필을 전제로 만들어졌다. 위젯 종류별 그리기는
// GuitaruLookAndFeel이 알아서 크레용 스타일로 바꿔주지만, 소스에 박혀 있던 색과 폰트는
// 종이 배경에서 읽히지 않으므로 여기서 한 번에 브랜드 색으로 바꿔 준다.
void MainComponent::applyChalkStyleToMode1Controls()
{
    mode1StatusLabel.setColour(juce::Label::textColourId, guitaru::inkMuted());
    mode1StatusLabel.setFont(guitaru::chalkFont(18.0f));

    songLabel.setColour(juce::Label::textColourId, guitaru::green());
    guitarStatusLabel.setColour(juce::Label::textColourId, guitaru::inkMuted());
    latencyStatusLabel.setColour(juce::Label::textColourId, guitaru::inkMuted());
    virtualChordLabel.setColour(juce::Label::textColourId, guitaru::green());
    expressionLabel.setColour(juce::Label::textColourId, guitaru::green());
    keyShiftLabel.setColour(juce::Label::textColourId, guitaru::green());
    profileProgressLabel.setColour(juce::Label::textColourId, guitaru::inkMuted());
    recordingStatusLabel.setColour(juce::Label::textColourId, guitaru::inkMuted());

    // 가사는 이 화면의 주인공이라 크레용 카드 위에 크게 올린다.
    lyricLabel.setFont(guitaru::chalkFont(32.0f, true));
    lyricLabel.setColour(juce::Label::backgroundColourId, guitaru::bluePale());
    lyricLabel.setColour(juce::Label::textColourId, guitaru::green());

    recordingTitleLabel.setFont(guitaru::chalkFont(22.0f, true));
    recordingTitleLabel.setColour(juce::Label::textColourId, guitaru::green());

    startPerformanceButton.setColour(juce::TextButton::buttonColourId, guitaru::blue());
    stopPerformanceButton.setColour(juce::TextButton::buttonColourId, guitaru::red());
    recordButton.setColour(juce::TextButton::buttonColourId, guitaru::red());
    loadSongButton.setColour(juce::TextButton::buttonColourId, guitaru::green());
    mode1AudioSettingsButton.setColour(juce::TextButton::buttonColourId, guitaru::inkMuted());
}

// 기본 화면에 남길 것: 곡/모델 고르기, 시작·재시작·중지, 기타 입력 녹음·저장,
// 오디오 장치 설정, 그리고 연주 중 봐야 하는 가사와 입력 상태.
// 나머지 조정 손잡이와 진단용 컨트롤은 전부 여기로 접는다.
void MainComponent::collectAdvancedMode1Controls()
{
    advancedMode1Controls = {
        &loadSongButton,
        &nextPhraseButton,
        &automaticPlaybackButton,
        &guitarReplayStartButton,
        &guitarReplayStopButton,
        &measureLatencyButton,
        &applyLatencyButton,
        &resetLatencyButton,
        &latencyStatusLabel,
        &pauseOnWrongChordToggle,
        &followPerformanceTempoToggle,
        &virtualChordLabel,
        &guitarChannelSelector,
        &expressionLabel,
        &expressionSlider,
        &keyShiftLabel,
        &keyShiftSlider,
        &recordingTitleLabel,
        &microphoneChannelSelector,
        &noiseReductionToggle,
        &recordButton,
        &profileProgressLabel,
        &profileProgressBar,
        &recordingStatusLabel,
    };
    for (auto& button : virtualChordButtons)
        advancedMode1Controls.push_back(&button);
}

void MainComponent::setAdvancedControlsVisible(bool shouldBeVisible)
{
    showAdvancedControls = shouldBeVisible;
    for (auto* control : advancedMode1Controls)
        control->setVisible(shouldBeVisible);

    advancedToggleButton.setButtonText(
        shouldBeVisible ? utf8("고급 설정  ▾") : utf8("고급 설정  ▸"));
    layoutMode1Panel();
    mode1Panel.repaint();
}

void MainComponent::updateAudioDeviceStatus()
{
    if (activeMode != ActiveMode::landing)
        return;

    if (auto* device = deviceManager.getCurrentAudioDevice())
    {
        const int numIn = device->getActiveInputChannels().countNumberOfSetBits();
        const int numOut = device->getActiveOutputChannels().countNumberOfSetBits();
        const auto setup = deviceManager.getAudioDeviceSetup();
        statusLabel.setText(utf8("입력 ") + setup.inputDeviceName + " (" + juce::String(numIn) + utf8("채널)  /  출력 ")
                                 + setup.outputDeviceName + " (" + juce::String(numOut) + utf8("채널)"),
                             juce::dontSendNotification);
    }
    else
    {
        statusLabel.setText(utf8("오디오 장치 없음 - 설정을 확인하세요"), juce::dontSendNotification);
    }
}

void MainComponent::changeListenerCallback(juce::ChangeBroadcaster* /*source*/)
{
    updateAudioDeviceStatus();
    saveAudioSettings();
    refreshChannelChoices();
    updateLatencyInfo();
    refreshLatencyDisplay();
}

void MainComponent::updateLatencyInfo()
{
    auto* device = deviceManager.getCurrentAudioDevice();
    if (device == nullptr)
        return;

    const double sr = device->getCurrentSampleRate();
    if (sr <= 0.0)
        return;

    const double toMs = 1000.0 / sr;
    const double inputMs = static_cast<double>(device->getInputLatencyInSamples()) * toMs;
    const double outputMs = static_cast<double>(device->getOutputLatencyInSamples()) * toMs;
    // 제어 lookahead용 목소리 지연 + 콜백 블록 1개분 + 시프터 고유 지연.
    const double processingMs =
        static_cast<double>(device->getCurrentBufferSizeSamples()) * toMs
        + 1000.0 * mode2::params::vocalControlLookaheadSeconds
        + static_cast<double>(mode2Controller.getPitchShifterLatencySamples()) * toMs;

    mode2Screen.setLatencyInfo(inputMs, outputMs, processingMs);
}

void MainComponent::selectPitchShifterBackend(PitchShifterEngine::Backend backend)
{
    if (! PitchShifterEngine::isBackendAvailable(backend)
        || backend == mode2Controller.getPitchShifterBackend())
        return;

    auto* device = deviceManager.getCurrentAudioDevice();
    if (device == nullptr)
        return;

    // UI 스레드에서 백엔드 객체를 reset하는 동안 오디오 콜백이 같은 객체를 건드리지
    // 못하게 한다. 전환 순간에는 짧은 무음이 생기지만 A/B 중 데이터 레이스나 클릭은 없다.
    {
        const juce::ScopedLock callbackLock(deviceManager.getAudioCallbackLock());
        mode2Controller.setPitchShifterBackend(backend);
        mode2Controller.prepare(device->getCurrentSampleRate(),
                                device->getCurrentBufferSizeSamples());
    }
    updateLatencyInfo();
}

void MainComponent::refreshChannelChoices()
{
    int numInputs = 2;
    if (auto* device = deviceManager.getCurrentAudioDevice())
        numInputs = juce::jmax(1, device->getActiveInputChannels().countNumberOfSetBits());

    const int guitar = juce::jlimit(0, numInputs - 1, guitarChannelIndex.load());
    const int vocal = juce::jlimit(0, numInputs - 1, vocalChannelIndex.load());
    const int room = roomChannelIndex.load();

    mode2Screen.setAvailableInputChannels(numInputs, guitar, vocal,
                                          room < numInputs ? room : -1);

    // 모드 1의 채널 콤보박스도 같은 값을 가리키게 한다. 두 모드가 채널 매핑을
    // 공유하므로, 한쪽에서 배선을 고치면 다른 쪽에도 반영되어야 한다.
    guitarChannelSelector.setSelectedId(guitar + 1, juce::dontSendNotification);
    microphoneChannelSelector.setSelectedId(vocal + 1, juce::dontSendNotification);
}

juce::File MainComponent::getAudioSettingsFile()
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("VocalGuitarApp")
        .getChildFile("AudioDeviceSettings.xml");
}

void MainComponent::saveAudioSettings()
{
    if (auto xml = deviceManager.createStateXml())
    {
        const auto file = getAudioSettingsFile();
        file.getParentDirectory().createDirectory();
        xml->writeTo(file);
    }
}

juce::File MainComponent::getChannelMapFile()
{
    return getAudioSettingsFile().getSiblingFile("InputChannelMap.xml");
}

void MainComponent::saveChannelMap()
{
    juce::XmlElement xml("CHANNELMAP");
    xml.setAttribute("guitarChannel", guitarChannelIndex.load());
    xml.setAttribute("vocalChannel", vocalChannelIndex.load());
    xml.setAttribute("roomChannel", roomChannelIndex.load());

    const auto file = getChannelMapFile();
    file.getParentDirectory().createDirectory();
    xml.writeTo(file);
}

void MainComponent::loadChannelMap()
{
    auto file = getChannelMapFile();
    // 모드 2 전용이던 시절의 파일 이름도 그대로 읽어 준다.
    if (! file.existsAsFile())
        file = getAudioSettingsFile().getSiblingFile("Mode2ChannelMap.xml");
    if (! file.existsAsFile())
        return;

    if (auto xml = juce::XmlDocument::parse(file))
    {
        guitarChannelIndex.store(xml->getIntAttribute("guitarChannel", 0));
        vocalChannelIndex.store(xml->getIntAttribute("vocalChannel", 1));
        roomChannelIndex.store(xml->getIntAttribute("roomChannel", -1));
    }
}

juce::File MainComponent::getRecordingsDirectory()
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("VocalGuitarApp")
        .getChildFile("recordings");
}

void MainComponent::startRecording()
{
    stopRecording();

    const auto dir = getRecordingsDirectory();
    dir.createDirectory();
    recordingFile = dir.getChildFile("mode2_" + juce::Time::getCurrentTime().formatted("%Y%m%d_%H%M%S") + ".wav");
    recordingFile.deleteFile();

    auto stream = std::unique_ptr<juce::FileOutputStream>(recordingFile.createOutputStream());
    if (stream == nullptr)
        return;

    juce::WavAudioFormat wav;
    // ch0 = 기타 입력, ch1 = 목소리 입력(부스트 전), ch2 = 최종 출력.
    // 방 마이크 채널을 골라 두면 ch3 = 스피커 앞에서 받은 소리(앱 출력 + 반주가 섞인 실제 청취음).
    recordingChannelCount = roomChannelIndex.load() >= 0 ? 4 : 3;
    auto* writer = wav.createWriterFor(stream.get(), currentSampleRate, recordingChannelCount, 24, {}, 0);
    if (writer == nullptr)
        return;

    stream.release(); // writer가 소유권을 가져간다.
    threadedWriter.reset(new juce::AudioFormatWriter::ThreadedWriter(writer, recorderThread, 65536));

    // 듣기용 파일: 보정된 목소리 + 4번째 채널(반주)만. 변환 없이 바로 재생된다.
    // 4번째 채널이 없으면 앱 출력만 남는다.
    listenFile = recordingFile.getSiblingFile(recordingFile.getFileNameWithoutExtension()
                                              + utf8("_들어보기.wav"));
    listenFile.deleteFile();
    if (auto listenStream = std::unique_ptr<juce::FileOutputStream>(listenFile.createOutputStream()))
    {
        juce::WavAudioFormat listenWav;
        if (auto* lw = listenWav.createWriterFor(listenStream.get(), currentSampleRate, 2, 24, {}, 0))
        {
            listenStream.release();
            listenWriter.reset(new juce::AudioFormatWriter::ThreadedWriter(lw, recorderThread, 65536));
        }
    }

    const juce::ScopedLock sl(writerLock);
    activeWriter = threadedWriter.get();
    activeListenWriter = listenWriter.get();
}

void MainComponent::stopRecording()
{
    {
        const juce::ScopedLock sl(writerLock);
        activeWriter = nullptr;
        activeListenWriter = nullptr;
    }
    threadedWriter.reset();
    listenWriter.reset();
}

bool MainComponent::isRecording() const
{
    return threadedWriter != nullptr;
}

bool MainComponent::toggleRecording()
{
    if (isRecording())
        stopRecording();
    else
        startRecording();

    return isRecording();
}
