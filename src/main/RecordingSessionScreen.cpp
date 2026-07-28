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
    cancelButton.setEnabled(true);
    cancelButton.setButtonText(state.sessionFinished ? L"완료 · 돌아가기" : L"세션 취소");

    repaint();
}

void RecordingSessionScreen::updateTrainingStatus(
    bool availableToStart,
    bool running,
    bool trainingFinished,
    bool trainingSucceeded,
    const juce::String& trainingStatusLine)
{
    startTrainingButton.setVisible(availableToStart && !running && !trainingFinished);
    startTrainingButton.setEnabled(availableToStart && !running && !trainingFinished);

    const bool showStatus = running || trainingFinished;
    trainingStatusLabel.setVisible(showStatus);
    if (showStatus)
    {
        trainingStatusLabel.setColour(
            juce::Label::textColourId,
            trainingFinished
                ? (trainingSucceeded ? juce::Colours::lightgreen : juce::Colours::orange)
                : juce::Colours::lightblue);
        trainingStatusLabel.setText(trainingStatusLine, juce::dontSendNotification);
    }
    repaint();
}

void RecordingSessionScreen::paint(juce::Graphics& g)
{
    g.fillAll(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));

    if (state.sessionFinished)
    {
        g.setColour(juce::Colours::white);
        g.setFont(juce::FontOptions(28.0f, juce::Font::bold));
        g.drawText(
            L"녹음 완료! 수고하셨습니다.",
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
            const bool isCurrent = i == state.highlightedWordIndex;
            const bool isSpoken = i < state.highlightedWordIndex;
            const auto colour = isCurrent
                ? juce::Colours::limegreen
                : isSpoken
                    ? juce::Colours::lightgreen.withAlpha(0.75f)
                    : juce::Colours::white.withAlpha(0.45f);
            attributed.append(
                perSyllable ? state.words[i] : state.words[i] + " ",
                juce::FontOptions(32.0f, isCurrent ? juce::Font::bold : juce::Font::plain),
                colour);
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
