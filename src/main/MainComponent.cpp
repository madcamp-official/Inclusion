#include "MainComponent.h"

#include <algorithm>

MainComponent::MainComponent()
{
    setWantsKeyboardFocus(true);
    setAudioChannels(8, 2);

    statusLabel.setText("모드를 선택하세요.", juce::dontSendNotification);
    statusLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(statusLabel);

    mode1Button.onClick = [this] { selectMode1(); };
    mode2Button.onClick = [this]
    {
        if (voiceRecorder.isRecording())
            stopReferenceRecording();
        activeMode = ActiveMode::mode2Placeholder;
        mode1Controller.reset();
        statusLabel.setText(
            "Mode 2는 이 작업공간에 아직 연결되지 않았습니다.",
            juce::dontSendNotification);
    };
    addAndMakeVisible(mode1Button);
    addAndMakeVisible(mode2Button);

    loadSongButton.onClick = [this] { chooseSongPackage(); };
    nextPhraseButton.onClick = [this] { triggerNextPhrase(); };
    nextPhraseButton.setEnabled(false);
    addAndMakeVisible(loadSongButton);
    addAndMakeVisible(nextPhraseButton);

    guitarChannelSelector.setTextWhenNothingSelected("Guitar input channel");
    for (int channel = 0; channel < 8; ++channel)
        guitarChannelSelector.addItem("Guitar: Input " + juce::String(channel + 1), channel + 1);
    guitarChannelSelector.setSelectedId(1, juce::dontSendNotification);
    guitarChannelSelector.onChange = [this]
    {
        guitarChannelIndex.store(
            std::max(0, guitarChannelSelector.getSelectedId() - 1));
        grabKeyboardFocus();
    };
    addAndMakeVisible(guitarChannelSelector);

    songLabel.setText("곡 패키지: 미선택", juce::dontSendNotification);
    songLabel.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(songLabel);

    lyricLabel.setText("가사가 여기에 표시됩니다.", juce::dontSendNotification);
    lyricLabel.setFont(juce::FontOptions(24.0f, juce::Font::bold));
    lyricLabel.setJustificationType(juce::Justification::centred);
    lyricLabel.setColour(juce::Label::backgroundColourId, juce::Colours::black.withAlpha(0.25f));
    addAndMakeVisible(lyricLabel);

    guitarStatusLabel.setText("기타 입력 대기", juce::dontSendNotification);
    guitarStatusLabel.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(guitarStatusLabel);

    recordButton.onClick = [this]
    {
        if (voiceRecorder.isRecording())
            stopReferenceRecording();
        else
            startReferenceRecording();
    };
    addAndMakeVisible(recordButton);

    recordingStatusLabel.setText(
        "음성 프로필 녹음 기능은 기존 방식으로 유지됩니다.",
        juce::dontSendNotification);
    recordingStatusLabel.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(recordingStatusLabel);

    startTimerHz(15);
    setSize(820, 560);
}

MainComponent::~MainComponent()
{
    stopTimer();
    voiceRecorder.stop();
    shutdownAudio();
}

void MainComponent::prepareToPlay(int samplesPerBlockExpected, double sampleRate)
{
    currentSampleRate = sampleRate;
    guitarInputScratch.assign(
        static_cast<size_t>(std::max(1, samplesPerBlockExpected)),
        0.0f);
    mode1Controller.prepare(sampleRate, samplesPerBlockExpected);
    voiceRecorder.prepare(sampleRate);
}

void MainComponent::getNextAudioBlock(
    const juce::AudioSourceChannelInfo& bufferToFill)
{
    if (bufferToFill.buffer == nullptr)
        return;

    auto& buffer = *bufferToFill.buffer;
    const int numSamples = bufferToFill.numSamples;
    const int startSample = bufferToFill.startSample;

    if (voiceRecorder.isRecording())
        voiceRecorder.processBlock(buffer);

    if (static_cast<int>(guitarInputScratch.size()) < numSamples)
    {
        // The device is not expected to exceed the prepared block size.
        // Stay silent instead of allocating on the real-time audio thread.
        bufferToFill.clearActiveBufferRegion();
        return;
    }

    if (buffer.getNumChannels() > 0)
    {
        const int guitarChannel = juce::jlimit(
            0,
            buffer.getNumChannels() - 1,
            guitarChannelIndex.load());
        const auto* guitarInput =
            buffer.getReadPointer(guitarChannel, startSample);
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
}

void MainComponent::selectMode1()
{
    if (voiceRecorder.isRecording())
        stopReferenceRecording();

    activeMode = ActiveMode::mode1;
    statusLabel.setText(
        "Mode 1 활성화: 기타 스트로크가 프레이즈 시작을 보정합니다.",
        juce::dontSendNotification);

    if (!mode1Controller.hasSong())
    {
        const auto developmentPackage = findDevelopmentSongPackage();
        if (developmentPackage.existsAsFile())
            loadSongPackage(developmentPackage);
    }

    grabKeyboardFocus();
}

void MainComponent::chooseSongPackage()
{
    packageChooser = std::make_unique<juce::FileChooser>(
        "Mode 1 song_package.json 선택",
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
        statusLabel.setText("곡 로드 실패: " + error, juce::dontSendNotification);
        nextPhraseButton.setEnabled(false);
        return false;
    }

    activeMode = ActiveMode::mode1;
    songLabel.setText(
        "곡 패키지: " + mode1Controller.getSongName()
            + "  기준 키 "
            + (mode1Controller.getBaseKeyShift() >= 0 ? "+" : "")
            + juce::String(mode1Controller.getBaseKeyShift()) + " st"
            + (mode1Controller.getRangeWarning().isNotEmpty()
                ? "  ⚠ " + mode1Controller.getRangeWarning()
                : ""),
        juce::dontSendNotification);
    lyricLabel.setText("기타를 치거나 Space를 눌러 시작", juce::dontSendNotification);
    statusLabel.setText(
        "Mode 1 준비 완료: " + file.getFullPathName(),
        juce::dontSendNotification);
    nextPhraseButton.setEnabled(true);
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

void MainComponent::triggerNextPhrase()
{
    if (activeMode == ActiveMode::mode1 && mode1Controller.hasSong())
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

void MainComponent::startReferenceRecording()
{
    mode1Controller.reset();
    activeMode = ActiveMode::idle;

    if (!voiceRecorder.start())
    {
        recordingStatusLabel.setText(
            "오디오 장치가 준비되지 않아 녹음을 시작할 수 없습니다.",
            juce::dontSendNotification);
        return;
    }

    lastReferenceFile = {};
    recordButton.setButtonText("녹음 정지 및 검사");
    mode1Button.setEnabled(false);
    mode2Button.setEnabled(false);
    recordingStatusLabel.setColour(
        juce::Label::textColourId,
        juce::Colours::orangered);
    recordingStatusLabel.setText("녹음 중... 0.0초", juce::dontSendNotification);
}

void MainComponent::stopReferenceRecording()
{
    if (!voiceRecorder.isRecording())
        return;

    voiceRecorder.stop();
    recordButton.setButtonText("Record voice profile");
    mode1Button.setEnabled(true);
    mode2Button.setEnabled(true);

    const auto quality = voiceRecorder.getQuality();
    const auto details =
        juce::String(quality.durationSeconds, 1) + "초, 평균 "
        + juce::String(quality.rmsDb, 1) + " dBFS, 피크 "
        + juce::String(quality.peakDb, 1) + " dBFS\n";

    if (!quality.passed)
    {
        recordingStatusLabel.setColour(
            juce::Label::textColourId,
            juce::Colours::orange);
        recordingStatusLabel.setText(
            details + quality.summary,
            juce::dontSendNotification);
        return;
    }

    lastReferenceFile = makeReferenceFile();
    const auto directoryResult =
        lastReferenceFile.getParentDirectory().createDirectory();
    const auto saveResult = directoryResult.wasOk()
        ? voiceRecorder.saveAsWav(lastReferenceFile)
        : directoryResult;

    if (saveResult.failed())
    {
        recordingStatusLabel.setColour(
            juce::Label::textColourId,
            juce::Colours::orange);
        recordingStatusLabel.setText(
            details + "저장 실패: " + saveResult.getErrorMessage(),
            juce::dontSendNotification);
        return;
    }

    recordingStatusLabel.setColour(
        juce::Label::textColourId,
        juce::Colours::lightgreen);
    recordingStatusLabel.setText(
        details + quality.summary + "\n저장: " + lastReferenceFile.getFullPathName(),
        juce::dontSendNotification);
}

juce::File MainComponent::makeReferenceFile() const
{
    const auto timestamp =
        juce::Time::getCurrentTime().formatted("%Y%m%d_%H%M%S");
    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
        .getChildFile("VocalGuitarApp")
        .getChildFile("reference_voice_" + timestamp + ".wav");
}

void MainComponent::timerCallback()
{
    if (voiceRecorder.isRecording())
    {
        const auto seconds = voiceRecorder.getRecordedSeconds();
        recordingStatusLabel.setText(
            "녹음 중... " + juce::String(seconds, 1) + "초 / 최대 25초",
            juce::dontSendNotification);
        if (seconds >= voice_capture::VoiceRecorder::maximumRecordingSeconds)
            stopReferenceRecording();
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
    guitarStatusLabel.setText(
        juce::String(mode1Controller.isGuitarActive() ? "기타 입력 감지" : "기타 입력 대기")
            + "  RMS " + juce::String(rmsDb, 1) + " dBFS"
            + "  코드 " + mode1Controller.getDetectedChordName()
            + "  보컬 " + (mode1Controller.getPitchShiftSemitones() >= 0 ? "+" : "")
            + juce::String(mode1Controller.getPitchShiftSemitones()) + " st"
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
    loadSongButton.setBounds(songRow.removeFromLeft(190).reduced(4, 0));
    nextPhraseButton.setBounds(songRow.reduced(4, 0));
    area.removeFromTop(8);

    songLabel.setBounds(area.removeFromTop(30));
    area.removeFromTop(8);
    lyricLabel.setBounds(area.removeFromTop(115));
    area.removeFromTop(8);
    auto guitarRow = area.removeFromTop(34);
    guitarChannelSelector.setBounds(
        guitarRow.removeFromLeft(210).reduced(4, 0));
    guitarStatusLabel.setBounds(guitarRow.reduced(4, 0));
    area.removeFromTop(24);

    recordButton.setBounds(area.removeFromTop(42));
    area.removeFromTop(8);
    recordingStatusLabel.setBounds(area.removeFromTop(80));
}
