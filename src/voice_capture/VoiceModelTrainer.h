#pragma once

#include <juce_core/juce_core.h>

#include <atomic>

namespace voice_capture
{

// Total accepted audio the prepare step demands before a retrain is allowed
// to start. Kept in step with GuidedRecordingSession's 120s song-stage floor:
// a user who sings the full song stage (or finishes it early) plus the
// speaking and vowel takes must end up above this, or the guided flow would
// send them to a prepare-step failure it had already let them walk into.
constexpr double minimumTrainingSeconds = 120.0;

// Runs the local prepare_voice_training_dataset.py + GPU
// train_rvc_voice.py scripts (see tools/mode1_song_package/) on a
// background thread so the guided recording flow can offer one-click
// retraining without freezing the UI for the several minutes the GPU job
// takes. This class only shells out to those existing, already-verified
// scripts — it does not reimplement the SSH/training orchestration itself.
class VoiceModelTrainer : private juce::Thread
{
public:
    struct Config
    {
        juce::File pythonExecutable;
        juce::File repositoryRoot;
        juce::File profileDirectory;
        juce::File songPackage;
        juce::String sshHost;
        juce::String experimentName;
    };

    VoiceModelTrainer();
    ~VoiceModelTrainer() override;

    // Starts the pipeline if one isn't already running. Safe to call from
    // the message thread.
    void start(Config newConfig);
    void cancel();

    [[nodiscard]] bool isBusy() const noexcept { return isThreadRunning(); }
    [[nodiscard]] bool isFinished() const noexcept { return finished.load(); }
    [[nodiscard]] bool didSucceed() const noexcept { return succeeded.load(); }
    [[nodiscard]] juce::String getLatestStatusLine() const;

private:
    void run() override;
    bool runStep(const juce::String& label, const juce::StringArray& commandLine);
    void setStatus(const juce::String& text);

    Config config;
    mutable juce::SpinLock statusLock;
    juce::String statusLine;
    std::atomic<bool> finished { true };
    std::atomic<bool> succeeded { false };
    std::atomic<bool> cancelRequested { false };
    juce::ChildProcess* activeProcess = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VoiceModelTrainer)
};

} // namespace voice_capture
