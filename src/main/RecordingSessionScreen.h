#pragma once

#include "voice_capture/GuidedRecordingSession.h"

#include <juce_gui_extra/juce_gui_extra.h>

#include <functional>

// Full-screen guided-recording view: shows the current prompt with
// word-by-word highlighting driven by GuidedRecordingState, an overall
// session progress bar, a mic level meter, and retry/skip escape hatches.
// Purely a state-driven view — MainComponent pushes a fresh
// GuidedRecordingState into updateState() every timer tick; this class
// owns no timer of its own.
class RecordingSessionScreen : public juce::Component
{
public:
    RecordingSessionScreen();

    void updateState(
        const voice_capture::GuidedRecordingState& newState,
        float inputLevelValue01,
        const juce::String& statusMessage,
        bool statusIsError);
    void updateCalibrationState(
        const juce::String& instruction,
        double progress,
        float inputLevelValue01,
        bool failed);

    // Only meaningful once the session has finished. availableToStart is
    // true before the user has requested training; once requested, running
    // stays true until the background pipeline reports finished.
    void updateTrainingStatus(
        bool availableToStart,
        bool running,
        bool trainingFinished,
        bool trainingSucceeded,
        const juce::String& trainingStatusLine);

    std::function<void()> onRetryRequested;
    std::function<void()> onSkipRequested;
    std::function<void()> onCancelRequested;
    std::function<void()> onStartTrainingRequested;

    void paint(juce::Graphics& graphics) override;
    void resized() override;

private:
    voice_capture::GuidedRecordingState state;

    juce::Label stageLabel;
    juce::Label headingLabel;
    juce::Label statusLabel;
    double overallProgressValue = 0.0;
    juce::ProgressBar overallProgressBar { overallProgressValue };
    double inputLevelValue = 0.0;
    juce::ProgressBar inputLevelBar { inputLevelValue };
    juce::TextButton retryButton { L"다시 말하기" };
    juce::TextButton skipButton { L"건너뛰기" };
    juce::TextButton cancelButton { L"세션 취소" };
    juce::TextButton startTrainingButton { L"목소리 재학습 시작" };
    juce::Label trainingStatusLabel;
    juce::Rectangle<int> promptArea;
    bool calibrationMode = false;
    juce::String calibrationInstruction;
    bool calibrationFailed = false;
    juce::String trainingHeadline;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RecordingSessionScreen)
};
