#include "RecordingSessionScreen.h"

RecordingSessionScreen::RecordingSessionScreen()
{
    stageLabel.setJustificationType(juce::Justification::centred);
    stageLabel.setFont(juce::FontOptions(16.0f, juce::Font::bold));
    stageLabel.setColour(juce::Label::textColourId, juce::Colours::lightblue);
    addAndMakeVisible(stageLabel);

    headingLabel.setJustificationType(juce::Justification::centred);
    headingLabel.setFont(juce::FontOptions(15.0f));
    headingLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
    addAndMakeVisible(headingLabel);

    statusLabel.setJustificationType(juce::Justification::centred);
    statusLabel.setFont(juce::FontOptions(14.0f));
    addAndMakeVisible(statusLabel);

    overallProgressBar.setPercentageDisplay(false);
    addAndMakeVisible(overallProgressBar);

    inputLevelBar.setPercentageDisplay(false);
    addAndMakeVisible(inputLevelBar);

    retryButton.onClick = [this]
    {
        if (onRetryRequested)
            onRetryRequested();
    };
    addAndMakeVisible(retryButton);

    skipButton.onClick = [this]
    {
        if (onSkipRequested)
            onSkipRequested();
    };
    addAndMakeVisible(skipButton);

    cancelButton.onClick = [this]
    {
        if (onCancelRequested)
            onCancelRequested();
    };
    addAndMakeVisible(cancelButton);

    startTrainingButton.setVisible(false);
    startTrainingButton.onClick = [this]
    {
        if (onStartTrainingRequested)
            onStartTrainingRequested();
    };
    addAndMakeVisible(startTrainingButton);

    trainingStatusLabel.setJustificationType(juce::Justification::centredLeft);
    trainingStatusLabel.setFont(juce::FontOptions(13.0f));
    trainingStatusLabel.setVisible(false);
    addAndMakeVisible(trainingStatusLabel);
}

void RecordingSessionScreen::updateState(
    const voice_capture::GuidedRecordingState& newState,
    float inputLevelValue01,
    const juce::String& statusMessage,
    bool statusIsError)
{
    state = newState;
    calibrationMode = false;
    retryButton.setButtonText(L"전체 다시 녹음");
    overallProgressValue = state.overallProgress;
    inputLevelValue = juce::jlimit(0.0, 1.0, static_cast<double>(inputLevelValue01));

    if (state.stage == voice_capture::GuidedRecordingStage::song)
    {
        stageLabel.setText(
            state.stageLabel + "  "
                + juce::String(static_cast<int>(state.stageProgressSeconds)) + "s / "
                + juce::String(static_cast<int>(state.stageProgressTargetSeconds)) + "s",
            juce::dontSendNotification);
    }
    else
    {
        stageLabel.setText(
            state.stageLabel + "  ("
                + juce::String(state.itemIndexInStage + 1) + "/"
                + juce::String(juce::jmax(1, state.itemCountInStage)) + ")",
            juce::dontSendNotification);
    }

    headingLabel.setText(state.heading, juce::dontSendNotification);

    statusLabel.setColour(
        juce::Label::textColourId,
        statusIsError ? juce::Colours::orange : juce::Colours::lightgreen);
    statusLabel.setText(statusMessage, juce::dontSendNotification);

    retryButton.setEnabled(!state.sessionFinished);
    skipButton.setEnabled(!state.sessionFinished);
    skipButton.setButtonText(
        state.stage == voice_capture::GuidedRecordingStage::song
                && state.stageProgressSeconds >= state.stageProgressTargetSeconds
            ? L"전체 녹음 완료"
            : L"말했어요 · 다음");
    cancelButton.setEnabled(true);
    cancelButton.setButtonText(state.sessionFinished ? L"완료 · 돌아가기" : L"세션 취소");

    repaint();
}

void RecordingSessionScreen::updateCalibrationState(
    const juce::String& instruction,
    double progress,
    float inputLevelValue01,
    bool failed)
{
    calibrationMode = true;
    calibrationInstruction = instruction;
    calibrationFailed = failed;
    overallProgressValue = juce::jlimit(0.0, 1.0, progress);
    inputLevelValue =
        juce::jlimit(0.0, 1.0, static_cast<double>(inputLevelValue01));
    stageLabel.setText(L"마이크 자동 음량 맞춤", juce::dontSendNotification);
    headingLabel.setText(
        failed ? L"측정 실패" : L"녹음 전 약 5초만 확인할게요",
        juce::dontSendNotification);
    statusLabel.setColour(
        juce::Label::textColourId,
        failed ? juce::Colours::orange : juce::Colours::lightblue);
    statusLabel.setText(instruction, juce::dontSendNotification);
    retryButton.setButtonText(failed ? L"다시 측정" : L"측정 중...");
    retryButton.setEnabled(failed);
    skipButton.setEnabled(false);
    cancelButton.setEnabled(true);
    cancelButton.setButtonText(L"세션 취소");
    startTrainingButton.setVisible(false);
    trainingStatusLabel.setVisible(false);
    repaint();
}

void RecordingSessionScreen::updateTrainingStatus(
    bool availableToStart,
    bool running,
    bool trainingFinished,
    bool trainingSucceeded,
    const juce::String& trainingStatusLine)
{
    if (running)
    {
        trainingHeadline = L"모델 학습 및 노래 변환 중";
        stageLabel.setText(L"2/3 · 재학습 진행 중", juce::dontSendNotification);
        headingLabel.setText(
            L"완료까지 약 8분 걸립니다. 앱을 종료하지 마세요.",
            juce::dontSendNotification);
    }
    else if (trainingFinished && trainingSucceeded)
    {
        trainingHeadline = L"새 목소리 학습 및 적용 완료";
        stageLabel.setText(L"3/3 · 적용 완료", juce::dontSendNotification);
        headingLabel.setText(
            L"이제 Mode 1에서 새 목소리를 사용할 수 있습니다.",
            juce::dontSendNotification);
    }
    else if (trainingFinished)
    {
        trainingHeadline = L"재학습 실패";
        stageLabel.setText(L"학습 실패", juce::dontSendNotification);
        headingLabel.setText(
            L"아래 오류를 확인한 뒤 재시도할 수 있습니다.",
            juce::dontSendNotification);
    }
    else
    {
        trainingHeadline = L"녹음 저장 완료 · 학습 시작 대기";
        stageLabel.setText(L"1/3 · 녹음 저장 완료", juce::dontSendNotification);
    }
    cancelButton.setEnabled(!running);
    cancelButton.setButtonText(
        running ? L"학습 중 · 잠시 기다려 주세요" : L"완료 · 돌아가기");

    const bool canStartOrRetry =
        availableToStart && !running && trainingFinished && !trainingSucceeded;
    startTrainingButton.setVisible(canStartOrRetry);
    startTrainingButton.setEnabled(canStartOrRetry);
    startTrainingButton.setButtonText(
        L"재학습 다시 시도");

    const bool showStatus = running || trainingFinished || !availableToStart;
    trainingStatusLabel.setVisible(showStatus);
    if (showStatus)
    {
        trainingStatusLabel.setColour(
            juce::Label::textColourId,
            trainingFinished
                ? (trainingSucceeded ? juce::Colours::lightgreen : juce::Colours::orange)
                : (running ? juce::Colours::lightblue : juce::Colours::orange));
        trainingStatusLabel.setText(trainingStatusLine, juce::dontSendNotification);
    }
    repaint();
}

void RecordingSessionScreen::paint(juce::Graphics& g)
{
    g.fillAll(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));

    if (calibrationMode)
    {
        g.setColour(
            calibrationFailed ? juce::Colours::orange : juce::Colours::white);
        g.setFont(juce::FontOptions(27.0f, juce::Font::bold));
        g.drawFittedText(
            calibrationInstruction,
            promptArea,
            juce::Justification::centred,
            3);
        return;
    }

    if (state.sessionFinished)
    {
        g.setColour(juce::Colours::white);
        g.setFont(juce::FontOptions(28.0f, juce::Font::bold));
        g.drawText(
            trainingHeadline.isNotEmpty()
                ? trainingHeadline
                : L"녹음 저장 완료",
            promptArea,
            juce::Justification::centred);
        return;
    }

    juce::AttributedString attributed;
    attributed.setJustification(juce::Justification::centred);
    attributed.setWordWrap(juce::AttributedString::byWord);

    if (state.words.isEmpty())
    {
        attributed.append(
            state.fullPrompt,
            juce::FontOptions(30.0f, juce::Font::bold),
            juce::Colours::white);
    }
    else
    {
        // Song-stage tokens are single syllables, so they must not be
        // padded apart the way whitespace-delimited words are.
        const bool perSyllable =
            state.stage == voice_capture::GuidedRecordingStage::song;
        for (int i = 0; i < state.words.size(); ++i)
        {
            attributed.append(
                perSyllable ? state.words[i] : state.words[i] + " ",
                juce::FontOptions(32.0f, juce::Font::plain),
                juce::Colours::white);
        }
    }

    attributed.draw(g, promptArea.toFloat());

    auto captionArea = promptArea.withY(promptArea.getBottom() + 10).withHeight(28);
    if (state.countdownActive)
    {
        g.setColour(juce::Colours::orange);
        g.setFont(juce::FontOptions(20.0f, juce::Font::bold));
        g.drawText(
            L"준비하세요... " + juce::String(state.countdownRemainingSeconds, 1) + "s",
            captionArea,
            juce::Justification::centred);
    }
    else if (state.awaitingFinalize)
    {
        g.setColour(juce::Colours::yellow);
        g.setFont(juce::FontOptions(18.0f));
        g.drawText(L"확인 중...", captionArea, juce::Justification::centred);
    }
    else if (state.recordingActive)
    {
        g.setColour(juce::Colours::orangered);
        g.setFont(juce::FontOptions(18.0f));
        g.drawText(L"● 듣고 있어요", captionArea, juce::Justification::centred);
    }
}

void RecordingSessionScreen::resized()
{
    auto area = getLocalBounds().reduced(40);

    auto header = area.removeFromTop(64);
    stageLabel.setBounds(header.removeFromTop(26));
    headingLabel.setBounds(header);

    area.removeFromTop(8);
    overallProgressBar.setBounds(area.removeFromTop(20).reduced(4, 0));

    area.removeFromTop(16);
    auto footer = area.removeFromBottom(190);
    promptArea = area;

    inputLevelBar.setBounds(footer.removeFromTop(20).reduced(4, 0));
    footer.removeFromTop(8);
    statusLabel.setBounds(footer.removeFromTop(36));
    footer.removeFromTop(6);
    auto trainingRow = footer.removeFromTop(30);
    startTrainingButton.setBounds(trainingRow.removeFromLeft(220).reduced(4, 2));
    trainingStatusLabel.setBounds(trainingRow.reduced(4, 2));
    footer.removeFromTop(6);
    auto buttonRow = footer.removeFromTop(40);
    const int buttonWidth = buttonRow.getWidth() / 3;
    retryButton.setBounds(buttonRow.removeFromLeft(buttonWidth).reduced(6, 4));
    skipButton.setBounds(buttonRow.removeFromLeft(buttonWidth).reduced(6, 4));
    cancelButton.setBounds(buttonRow.reduced(6, 4));
}
