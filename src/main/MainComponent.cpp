#include "MainComponent.h"

#include <algorithm>
#include <cmath>

namespace
{
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
    setWantsKeyboardFocus(true);
    setAudioChannels(8, 2);

    statusLabel.setText(L"모드를 선택하세요.", juce::dontSendNotification);
    statusLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(statusLabel);
    selectPreferredLowLatencyDevice();

    mode1Button.onClick = [this] { selectMode1(); };
    mode2Button.onClick = [this]
    {
        if (guidedRecordingSession.isActive())
            cancelGuidedRecordingSession();
        activeMode = ActiveMode::mode2Placeholder;
        mode1Controller.reset();
        automaticPlaybackButton.setToggleState(
            false, juce::dontSendNotification);
        automaticPlaybackButton.setButtonText(L"자동 연주 시작");
        setVirtualControlsEnabled(false);
        guitarReplayActive.store(false);
        guitarTestRecorder.stop();
        performanceOutputRecorder.stop();
        refreshGuitarTestControls();
        statusLabel.setText(
            L"Mode 2는 이 작업공간에 아직 연결되지 않았습니다.",
            juce::dontSendNotification);
    };
    addAndMakeVisible(mode1Button);
    addAndMakeVisible(mode2Button);

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
            statusLabel.setText(
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
    followerModeSelector.setSelectedId(2, juce::dontSendNotification);
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
        statusLabel.setText(
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
    nextPhraseButton.onClick = [this] { triggerNextPhrase(); };
    nextPhraseButton.setEnabled(false);
    addAndMakeVisible(songSelector);
    addAndMakeVisible(followerModeSelector);
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
        statusLabel.setText(
            enabled
                ? L"코드 오류 일시정지 켜짐 · 불일치 2회 시 정지"
                : L"코드 오류 일시정지 꺼짐",
            juce::dontSendNotification);
        grabKeyboardFocus();
    };
    addAndMakeVisible(pauseOnWrongChordToggle);
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
        statusLabel.setText(
            enabled
                ? L"연주 속도 보컬 추종 켜짐 · 안정화된 템포를 부드럽게 적용"
                : L"연주 속도 보컬 추종 꺼짐 · 원래 발음 길이 유지",
            juce::dontSendNotification);
        grabKeyboardFocus();
    };
    addAndMakeVisible(followPerformanceTempoToggle);
    addAndMakeVisible(loadSongButton);
    addAndMakeVisible(nextPhraseButton);

    startPerformanceButton.onClick =
        [this] { startMode1Performance(false); };
    restartPerformanceButton.onClick =
        [this] { startMode1Performance(true); };
    stopPerformanceButton.onClick =
        [this] { stopMode1Performance(); };
    startPerformanceButton.setEnabled(false);
    restartPerformanceButton.setEnabled(false);
    stopPerformanceButton.setEnabled(false);
    addAndMakeVisible(startPerformanceButton);
    addAndMakeVisible(restartPerformanceButton);
    addAndMakeVisible(stopPerformanceButton);

    guitarRecordStartButton.onClick =
        [this] { startGuitarTestRecording(); };
    guitarRecordStopButton.onClick =
        [this] { stopGuitarTestRecording(); };
    guitarReplayStartButton.onClick =
        [this] { startGuitarTestReplay(); };
    guitarReplayStopButton.onClick =
        [this] { stopGuitarTestReplay(); };
    addAndMakeVisible(guitarRecordStartButton);
    addAndMakeVisible(guitarRecordStopButton);
    addAndMakeVisible(guitarReplayStartButton);
    addAndMakeVisible(guitarReplayStopButton);
    refreshGuitarTestControls();

    audioSettingsButton.onClick = [this] { showAudioSettings(); };
    audioSettingsButton.setTooltip(
        L"오인페, 드라이버, 샘플레이트와 버퍼 크기를 설정합니다.");
    addAndMakeVisible(audioSettingsButton);

    measureLatencyButton.onClick = [this] { requestLatencyMeasurement(); };
    measureLatencyButton.setTooltip(
        L"오인페 출력을 선택한 기타 입력으로 연결해 실제 왕복 레이턴시를 측정합니다.");
    applyLatencyButton.onClick = [this] { applyLatencyMeasurement(); };
    applyLatencyButton.setEnabled(false);
    resetLatencyButton.onClick = [this] { resetLatencyCompensation(); };
    latencyStatusLabel.setJustificationType(juce::Justification::centredLeft);
    latencyStatusLabel.setMinimumHorizontalScale(0.72f);
    addAndMakeVisible(measureLatencyButton);
    addAndMakeVisible(applyLatencyButton);
    addAndMakeVisible(resetLatencyButton);
    addAndMakeVisible(latencyStatusLabel);

    guitarChannelSelector.setTextWhenNothingSelected("Guitar input channel");
    for (int channel = 0; channel < 8; ++channel)
        guitarChannelSelector.addItem("Guitar: Input " + juce::String(channel + 1), channel + 1);
    guitarChannelSelector.setSelectedId(2, juce::dontSendNotification);
    guitarChannelSelector.onChange = [this]
    {
        guitarChannelIndex.store(
            std::max(0, guitarChannelSelector.getSelectedId() - 1));
        liveGuitarPeak.store(0.0f);
        guitarStatusLabel.setText(
            L"기타 입력 채널 "
                + juce::String(guitarChannelSelector.getSelectedId())
                + L" 선택됨 · 기타를 쳐서 입력 레벨을 확인하세요",
            juce::dontSendNotification);
        grabKeyboardFocus();
    };
    addAndMakeVisible(guitarChannelSelector);

    virtualChordLabel.setText(
        L"가상 기타 코드 (현재 엔진은 코드 루트 기준)",
        juce::dontSendNotification);
    virtualChordLabel.setJustificationType(
        juce::Justification::centredLeft);
    addAndMakeVisible(virtualChordLabel);

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
            statusLabel.setText(
                L"가상 기타 입력: "
                    + virtualChordButtons[static_cast<size_t>(root)]
                        .getButtonText(),
                juce::dontSendNotification);
            grabKeyboardFocus();
        };
        addAndMakeVisible(button);
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
        statusLabel.setText(
            enabled
                ? L"자동 연주 중: 곡의 프레이즈 타이밍대로 재생합니다."
                : L"기타 대기 모드: 코드 버튼, 실제 기타 또는 Space를 기다립니다.",
            juce::dontSendNotification);
        grabKeyboardFocus();
    };
    addAndMakeVisible(automaticPlaybackButton);

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
                statusLabel.setText(
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
    addAndMakeVisible(expressionLabel);
    addAndMakeVisible(expressionSlider);

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
                statusLabel.setText(
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
    addAndMakeVisible(keyShiftLabel);
    addAndMakeVisible(keyShiftSlider);

    songLabel.setText(L"곡 패키지: 미선택", juce::dontSendNotification);
    songLabel.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(songLabel);

    lyricLabel.setText(L"가사가 여기에 표시됩니다.", juce::dontSendNotification);
    lyricLabel.setFont(juce::FontOptions(24.0f, juce::Font::bold));
    lyricLabel.setJustificationType(juce::Justification::centred);
    lyricLabel.setColour(juce::Label::backgroundColourId, juce::Colours::black.withAlpha(0.25f));
    addAndMakeVisible(lyricLabel);

    guitarStatusLabel.setText(L"기타 입력 대기", juce::dontSendNotification);
    guitarStatusLabel.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(guitarStatusLabel);

    recordingTitleLabel.setText(L"내 목소리 녹음", juce::dontSendNotification);
    recordingTitleLabel.setFont(juce::FontOptions(20.0f, juce::Font::bold));
    addAndMakeVisible(recordingTitleLabel);

    microphoneChannelSelector.setTextWhenNothingSelected(L"마이크 입력");
    for (int channel = 0; channel < 8; ++channel)
        microphoneChannelSelector.addItem(
            L"마이크: Input " + juce::String(channel + 1),
            channel + 1);
    microphoneChannelSelector.setSelectedId(1, juce::dontSendNotification);
    microphoneChannelSelector.onChange = [this]
    {
        microphoneChannelIndex.store(
            std::max(0, microphoneChannelSelector.getSelectedId() - 1));
    };
    addAndMakeVisible(microphoneChannelSelector);

    noiseReductionToggle.setToggleState(true, juce::dontSendNotification);
    addAndMakeVisible(noiseReductionToggle);

    recordButton.onClick = [this] { startGuidedRecordingSession(); };
    recordButton.setColour(
        juce::TextButton::buttonColourId,
        juce::Colour::fromRGB(185, 38, 55));
    addAndMakeVisible(recordButton);

    profileProgressBar.setPercentageDisplay(false);
    addAndMakeVisible(profileProgressBar);

    profileProgressLabel.setText(
        L"유효 음성 0초 / 최소 180초 · 통과 클립 0개",
        juce::dontSendNotification);
    addAndMakeVisible(profileProgressLabel);

    recordingStatusLabel.setText(
        L"준비됨 · '가이드 녹음 시작'을 누르면 문장이 순서대로 안내됩니다.",
        juce::dontSendNotification);
    recordingStatusLabel.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(recordingStatusLabel);

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

    configureLowLatencyAudio();
    refreshLatencyDisplay();
    startTimerHz(15);
    setSize(980, 1000);
}

MainComponent::~MainComponent()
{
    stopTimer();
    voiceRecorder.stop();
    {
        const juce::ScopedLock callbackLock(
            deviceManager.getAudioCallbackLock());
        guitarTestRecorder.stop();
        performanceOutputRecorder.stop();
        mode1Controller.setRealtimeTraceEnabled(false);
    }
    shutdownAudio();
}

void MainComponent::prepareToPlay(int samplesPerBlockExpected, double sampleRate)
{
    currentSampleRate = sampleRate;
    guitarInputScratch.assign(
        static_cast<size_t>(std::max(1, samplesPerBlockExpected)),
        0.0f);
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

    if ((recordingPreflightActive.load() || guidedRecordingSession.isActive())
        && buffer.getNumChannels() > 0)
    {
        const int micChannel = juce::jlimit(
            0,
            buffer.getNumChannels() - 1,
            microphoneChannelIndex.load());
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
            microphoneChannelIndex.load());
        inputLevelCalibrator.processBlock(
            buffer.getReadPointer(micChannel, startSample), numSamples);
    }

    if (guidedRecordingSession.isActive() && buffer.getNumChannels() > 0)
    {
        const int micChannel = juce::jlimit(
            0,
            buffer.getNumChannels() - 1,
            microphoneChannelIndex.load());
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
            buffer, microphoneChannelIndex.load(), startSample, numSamples);

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

    if (activeMode != ActiveMode::mode1
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
    if (performanceOutputRecorder.isRecording())
        performanceOutputRecorder.processBlock(
            buffer, 0, startSample, numSamples);
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
        statusLabel.setText(
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
        statusLabel.setText(
            L"저지연 버퍼 설정 실패: " + error,
            juce::dontSendNotification);
    }
}

void MainComponent::showAudioSettings()
{
    auto selector = std::make_unique<juce::AudioDeviceSelectorComponent>(
        deviceManager,
        1,
        8,
        2,
        2,
        false,
        false,
        true,
        false);
    selector->setSize(560, 430);

    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(selector.release());
    options.dialogTitle = L"오디오 인터페이스 / 저지연 설정";
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
        statusLabel.setText(
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
    statusLabel.setText(
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
    statusLabel.setText(
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
        statusLabel.setText(
            L"기타 테스트 녹음을 시작하지 못했습니다.",
            juce::dontSendNotification);
        return;
    }
    if (!outputRecorderStarted)
    {
        statusLabel.setText(
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
    statusLabel.setText(
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
        statusLabel.setText(
            L"기타 테스트 녹음 폴더를 만들 수 없습니다.",
            juce::dontSendNotification);
        refreshGuitarTestControls();
        return;
    }

    const auto result = guitarTestRecorder.saveAsWav(destination, false);
    if (result.failed())
    {
        statusLabel.setText(
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
        statusLabel.setText(
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
        statusLabel.setText(
            L"기타와 보컬은 저장했지만 합본 생성 실패: "
                + mixResult.getErrorMessage(),
            juce::dontSendNotification);
        refreshGuitarTestControls();
        return;
    }
    const auto diagnosticsResult = savePerformanceDiagnostics();
    if (diagnosticsResult.failed())
    {
        statusLabel.setText(
            L"Performance WAV saved, but diagnostics failed: "
                + diagnosticsResult.getErrorMessage(),
            juce::dontSendNotification);
        refreshGuitarTestControls();
        return;
    }
    const bool loaded = loadGuitarTestReplay(destination);
    statusLabel.setText(
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
            statusLabel.setText(
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
    statusLabel.setText(
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
    statusLabel.setText(
        L"저장된 기타 입력 테스트를 중지했습니다.",
        juce::dontSendNotification);
    refreshTransportControls();
    refreshGuitarTestControls();
}

void MainComponent::selectMode1()
{
    if (guidedRecordingSession.isActive())
        cancelGuidedRecordingSession();

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
    statusLabel.setText(
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
        statusLabel.setText(L"곡 로드 실패: " + error, juce::dontSendNotification);
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
    statusLabel.setText(
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
    if (key == juce::KeyPress::spaceKey)
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
    profileDirectory = {};
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
    activeMode = ActiveMode::idle;
    refreshGuitarTestControls();

    lastReferenceFile = {};
    guidedStatusMessage = {};
    guidedStatusIsError = false;
    guidedRecordingSession.setSongLines(loadSongLyricLines());
    recordingPreflightActive = true;
    inputLevelCalibrator.start();

    mode1Button.setEnabled(false);
    mode2Button.setEnabled(false);
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
    recordButton.setEnabled(true);

    recordingStatusLabel.setColour(
        juce::Label::textColourId,
        juce::Colours::white);
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
    graphics.fillAll(
        getLookAndFeel().findColour(
            juce::ResizableWindow::backgroundColourId));
}

void MainComponent::resized()
{
    auto area = getLocalBounds().reduced(24);
    statusLabel.setBounds(area.removeFromTop(34));
    area.removeFromTop(10);

    auto modeRow = area.removeFromTop(42);
    mode1Button.setBounds(
        modeRow.removeFromLeft(modeRow.getWidth() / 2).reduced(4, 0));
    mode2Button.setBounds(modeRow.reduced(4, 0));
    area.removeFromTop(14);

    auto songRow = area.removeFromTop(42);
    songSelector.setBounds(
        songRow.removeFromLeft(
            juce::roundToInt(songRow.getWidth() * 0.42f)).reduced(4, 0));
    followerModeSelector.setBounds(
        songRow.removeFromLeft(
            juce::roundToInt(songRow.getWidth() * 0.50f)).reduced(4, 0));
    loadSongButton.setBounds(songRow.reduced(4, 0));
    area.removeFromTop(6);

    auto transportRow = area.removeFromTop(40);
    const int transportButtonWidth = transportRow.getWidth() / 4;
    startPerformanceButton.setBounds(
        transportRow.removeFromLeft(transportButtonWidth).reduced(4, 0));
    restartPerformanceButton.setBounds(
        transportRow.removeFromLeft(transportButtonWidth).reduced(4, 0));
    stopPerformanceButton.setBounds(
        transportRow.removeFromLeft(transportButtonWidth).reduced(4, 0));
    nextPhraseButton.setBounds(transportRow.reduced(4, 0));
    area.removeFromTop(5);

    auto guitarTestRow = area.removeFromTop(36);
    const int guitarTestButtonWidth = guitarTestRow.getWidth() / 4;
    guitarRecordStartButton.setBounds(
        guitarTestRow.removeFromLeft(guitarTestButtonWidth).reduced(4, 0));
    guitarRecordStopButton.setBounds(
        guitarTestRow.removeFromLeft(guitarTestButtonWidth).reduced(4, 0));
    guitarReplayStartButton.setBounds(
        guitarTestRow.removeFromLeft(guitarTestButtonWidth).reduced(4, 0));
    guitarReplayStopButton.setBounds(guitarTestRow.reduced(4, 0));
    area.removeFromTop(8);

    songLabel.setBounds(area.removeFromTop(30));
    area.removeFromTop(8);
    lyricLabel.setBounds(area.removeFromTop(115));
    area.removeFromTop(8);
    auto guitarRow = area.removeFromTop(34);
    audioSettingsButton.setBounds(
        guitarRow.removeFromLeft(165).reduced(4, 0));
    guitarChannelSelector.setBounds(
        guitarRow.removeFromLeft(180).reduced(4, 0));
    guitarStatusLabel.setBounds(guitarRow.reduced(4, 0));
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
            std::max(120, virtualHeader.getWidth() - 630)).reduced(4, 0));
    pauseOnWrongChordToggle.setBounds(
        virtualHeader.removeFromLeft(220).reduced(4, 0));
    followPerformanceTempoToggle.setBounds(
        virtualHeader.removeFromLeft(220).reduced(4, 0));
    automaticPlaybackButton.setBounds(
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

    recordingSessionScreen.setBounds(getLocalBounds());
}
