#pragma once

#include "VoiceActivityTracker.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <vector>

namespace voice_capture
{

enum class GuidedRecordingStage
{
    speaking,
    vowel,
    song,
    finished
};

struct GuidedRecordingState
{
    GuidedRecordingStage stage = GuidedRecordingStage::speaking;
    juce::String stageLabel;
    juce::String heading;
    juce::String fullPrompt;
    juce::StringArray words;
    int highlightedWordIndex = -1;
    int itemIndexInStage = 0;
    int itemCountInStage = 0;
    double itemElapsedSeconds = 0.0;
    double overallProgress = 0.0;
    // Only meaningful for the song stage, where progress is time-based
    // rather than item-count-based (0 for the other stages).
    double stageProgressSeconds = 0.0;
    double stageProgressTargetSeconds = 0.0;
    bool countdownActive = false;
    double countdownRemainingSeconds = 0.0;
    bool recordingActive = false;
    bool awaitingFinalize = false;
    bool sessionFinished = false;
};

// Drives the guided, auto-advancing profile-recording flow: a fixed
// sequence of speaking / sustained-vowel / song stages, using
// VoiceActivityTracker on live mic blocks to decide when the user has
// spoken far enough into a prompt to highlight the next word and when a
// prompt is "done" (last word reached + trailing silence, or a sustained
// vowel hold, or the 120s song-stage floor). This class never touches
// files — it only decides *when* an item is ready, via the
// awaitingFinalize/recordingActive flags in GuidedRecordingState; the
// caller is responsible for starting/stopping the actual VoiceRecorder
// capture and running it through the existing save/quality-check/manifest
// pipeline, then calling notifyItemFinalized().
class GuidedRecordingSession
{
public:
    void prepare(double sampleRate);
    void start() noexcept;
    void stop() noexcept;

    // Audio-thread: feed live mic blocks in while a session is active.
    void processAudioBlock(const float* input, int numSamples) noexcept;

    // Message-thread: UI actions.
    void requestSkip() noexcept;
    void requestRetry() noexcept;
    // Called by the caller once it has finished saving/grading the item
    // that was flagged via awaitingFinalize. Advances to the next item or
    // stage and restarts the get-ready countdown.
    void notifyItemFinalized() noexcept;
    // Keeps the user on the same required item after a hard quality failure.
    // The raw take remains available for diagnostics, but it is not silently
    // counted as training data.
    void notifyItemRejected() noexcept;

    [[nodiscard]] GuidedRecordingState getState() const;
    [[nodiscard]] bool isActive() const noexcept;
    // Cheap, allocation-free check safe to call every audio block: true
    // while the current item should be captured into the underlying
    // VoiceRecorder. The caller starts/stops that recorder to match — a
    // transition back to false (countdown/awaitingFinalize) with no
    // corresponding finalize means the caller should discard rather than
    // save, which is exactly what skip/retry rely on.
    [[nodiscard]] bool shouldBeCapturing() const noexcept;
    [[nodiscard]] juce::String getCurrentCategorySlug() const;

    // Overrides the built-in generic placeholder lines for the song stage
    // with real song lyric lines the caller already has locally (e.g. read
    // from the user's own song_package.json phrases) — this class and its
    // caller never hardcode actual song lyrics, only load them from
    // whatever local data the caller supplies at runtime. Call before
    // start(). Passing an empty array reverts to the built-in placeholders.
    void setSongLines(const juce::StringArray& lines);

private:
    enum class ItemPhase
    {
        countdown,
        listening,
        awaitingFinalize
    };

    struct Item
    {
        juce::String heading;
        juce::String prompt;
    };

    static const std::vector<Item>& speakingItems();
    static const std::vector<Item>& vowelItems();
    static const std::vector<Item>& defaultSongItems();
    int itemCountFor(GuidedRecordingStage stage) const;
    const Item& itemFor(GuidedRecordingStage stage, int index) const;
    const std::vector<Item>& songItemsInUse() const;

    void beginItemLocked() noexcept;
    void advanceToNextItemLocked() noexcept;
    void processSongBlockLocked(double blockSeconds) noexcept;
    static juce::StringArray tokenizePrompt(
        const juce::String& prompt,
        GuidedRecordingStage forStage);

    mutable juce::SpinLock lock;
    VoiceActivityTracker tracker;
    double sampleRate = 48'000.0;
    bool active = false;
    GuidedRecordingStage stage = GuidedRecordingStage::speaking;
    int itemIndex = 0;
    int currentItemWordCount = 1;
    double currentItemMaxSeconds = 20.0;
    ItemPhase phase = ItemPhase::countdown;
    int highlightedWordIndex = -1;
    double itemElapsedSeconds = 0.0;
    double countdownRemainingSeconds = 0.0;
    double secondsVoicedTotal = 0.0;
    double songStageElapsedSeconds = 0.0;
    double songSyllableProgress = 0.0;
    double continuousRecordingSeconds = 0.0;
    bool captureHasStarted = false;
    bool sessionFinished = false;
    std::vector<Item> customSongItems;

    static constexpr double countdownSeconds = 1.4;
    static constexpr double vowelItemMaxSeconds = 9.0;
    static constexpr double minimumSessionSeconds = 180.0;
    static constexpr double perWordMaxSecondsFactor = 2.4;
    static constexpr double perWordMaxSecondsBase = 5.0;
    // Sung syllables blend into each other, so per-syllable energy onsets
    // are not reliably detectable the way word-initial consonants are in
    // speech. The song stage therefore sweeps the syllable highlight by
    // accumulated *voiced* time instead: it advances while the singer is
    // actually producing sound and pauses while they rest, which tracks a
    // legato line far better than onset counting.
    static constexpr double songSecondsPerSyllable = 0.42;
};

} // namespace voice_capture
