#include "MainComponent.h"

#include <algorithm>

MainComponent::MainComponent()
{
    setWantsKeyboardFocus(true);
    setAudioChannels(8, 2);

    statusLabel.setText(L"모드를 선택하세요.", juce::dontSendNotification);
    statusLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(statusLabel);

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
        refreshGuitarTestControls();
        statusLabel.setText(
            L"Mode 2는 이 작업공간에 아직 연결되지 않았습니다.",
            juce::dontSendNotification);
    };
    addAndMakeVisible(mode1Button);
    addAndMakeVisible(mode2Button);

    loadSongButton.onClick = [this] { chooseSongPackage(); };
    nextPhraseButton.onClick = [this] { triggerNextPhrase(); };
    nextPhraseButton.setEnabled(false);
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
    startTimerHz(15);
    setSize(900, 930);
}

MainComponent::~MainComponent()
{
    stopTimer();
    voiceRecorder.stop();
    guitarTestRecorder.stop();
    shutdownAudio();
}

void MainComponent::prepareToPlay(int samplesPerBlockExpected, double sampleRate)
{
    currentSampleRate = sampleRate;
    guitarInputScratch.assign(
        static_cast<size_t>(std::max(1, samplesPerBlockExpected)),
        0.0f);
    mode1Controller.prepare(sampleRate, samplesPerBlockExpected);
    if (auto* device = deviceManager.getCurrentAudioDevice())
    {
        mode1Controller.setOutputLatencySeconds(
            device->getOutputLatencyInSamples() / sampleRate);
    }
    voiceRecorder.prepare(sampleRate);
    guitarTestRecorder.prepare(sampleRate);
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
}

void MainComponent::releaseResources()
{
    mode1Controller.reset();
    voiceRecorder.stop();
    guitarTestRecorder.stop();
    guitarReplayActive.store(false);
    guidedRecordingSession.stop();
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

juce::File MainComponent::getGuitarTestRecordingFile() const
{
    return findRepositoryRoot()
        .getChildFile("output")
        .getChildFile("audio")
        .getChildFile("guitar_tests")
        .getChildFile("bansanka_last_guitar_take.wav");
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

    {
        const juce::ScopedLock callbackLock(
            deviceManager.getAudioCallbackLock());
        guitarReplayActive.store(false);
        guitarReplayFinished.store(false);
        mode1Controller.restartPerformance();
    }
    if (!guitarTestRecorder.start())
    {
        statusLabel.setText(
            L"기타 테스트 녹음을 시작하지 못했습니다.",
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
        L"기타 입력 녹음 중: Input "
            + juce::String(guitarChannelIndex.load() + 1),
        juce::dontSendNotification);
    refreshTransportControls();
    refreshGuitarTestControls();
}

void MainComponent::stopGuitarTestRecording()
{
    if (!guitarTestRecorder.isRecording())
        return;

    guitarTestRecorder.stop();
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

    lastGuitarTestRecording = destination;
    const bool loaded = loadGuitarTestReplay(destination);
    statusLabel.setText(
        loaded
            ? L"기타 입력 저장 완료: " + destination.getFullPathName()
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
    const auto relativePath = juce::String("build/mode1/bansanka/song_package.json");
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
            .getChildFile("mode1/bansanka/song_package.json");
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
                mode1Controller.getRecoveredSkippedChordCount() > 0
                    ? L"  재동기화 "
                        + juce::String(
                            mode1Controller
                                .getRecoveredSkippedChordCount())
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
    loadSongButton.setBounds(songRow.removeFromLeft(260).reduced(4, 0));
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
    area.removeFromTop(7);

    auto virtualHeader = area.removeFromTop(32);
    virtualChordLabel.setBounds(
        virtualHeader.removeFromLeft(
            virtualHeader.getWidth() - 190).reduced(4, 0));
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
