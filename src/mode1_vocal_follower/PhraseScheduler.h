#pragma once

#include "SongPackage.h"

#include <array>

namespace mode1
{

struct ChordEvidence
{
    bool valid = false;
    int rootPitchClass = -1;
    int bassPitchClass = -1;
    int quality = 8;
    int pitchClassMask = 0;
    float confidence = 0.0f;
    double analysisDelaySeconds = 0.075;
};

class PhraseScheduler
{
public:
    void prepare(double sampleRate);
    void setSong(const SongPackage* package);
    void setOutputLatencySeconds(double seconds) noexcept
    {
        // Start only the inaudible/consonant lead slightly early so the
        // vowel lands on the predicted guitar beat. This is compensation for
        // the output block and scheduler uncertainty, not musical look-ahead.
        guitarVocalLeadSeconds = juce::jlimit(
            0.035, 0.10, seconds + 0.050);
    }
    void reset() noexcept;

    // Returns the phrase index to start, or -1 when no phrase starts.
    int processBlock(
        int numSamples,
        bool guitarOnset,
        bool manualTrigger,
        bool automaticPlayback = false,
        bool guitarActive = false,
        float onsetStrength = 1.0f,
        const ChordEvidence* chordEvidence = nullptr) noexcept;

    [[nodiscard]] int getNextPhraseIndex() const noexcept { return nextPhraseIndex; }
    [[nodiscard]] int getCurrentChordEventIndex() const noexcept
    {
        return currentChordEventIndex;
    }
    [[nodiscard]] double getSongTimeSeconds() const noexcept { return songTimeSeconds; }
    [[nodiscard]] bool isRunning() const noexcept { return running; }
    [[nodiscard]] double getTempoScale() const noexcept { return tempoScale; }
    [[nodiscard]] int getRecoveredSkippedChordCount() const noexcept
    {
        return recoveredSkippedChordCount;
    }
    [[nodiscard]] int getExpiredPhraseCount() const noexcept
    {
        return expiredPhraseCount;
    }
    [[nodiscard]] int getEvidenceCorrectionCount() const noexcept
    {
        return evidenceCorrectionCount;
    }
    [[nodiscard]] int getAcceptedTimingAnchorCount() const noexcept
    {
        return acceptedTimingAnchorCount;
    }
    [[nodiscard]] int getRejectedTimingAnchorCount() const noexcept
    {
        return rejectedTimingAnchorCount;
    }

private:
    struct TimingAnchor
    {
        int eventIndex = -1;
        double scoreSeconds = 0.0;
        double performanceSeconds = 0.0;
    };

    [[nodiscard]] double normalizedScoreStartTime(int phraseIndex) const noexcept;
    [[nodiscard]] int firstPlayableChordEvent() const noexcept;
    [[nodiscard]] int nextPlayableChordEvent(int afterIndex) const noexcept;
    [[nodiscard]] int chooseChordEventForOnset(
        float onsetStrength) noexcept;
    [[nodiscard]] double scoreBeatSeconds() const noexcept;
    void updateTempoEstimate(
        int matchedEventIndex,
        double matchedPerformanceSeconds = -1.0) noexcept;
    [[nodiscard]] double estimatedChordBoundaryPerformanceSeconds(
        int eventIndex,
        double onsetPerformanceSeconds) const noexcept;
    [[nodiscard]] bool hasReliableTimingSlope() const noexcept;
    [[nodiscard]] double performanceSecondsPerScoreSecond() const noexcept;
    [[nodiscard]] double mapScoreTimeToPerformanceSeconds(
        double scoreSeconds) const noexcept;
    bool recordTimingAnchor(
        int eventIndex,
        double boundaryPerformanceSeconds,
        bool allowPauseRebase) noexcept;
    void clearTimingAnchors() noexcept;
    void applyChordEvidence(const ChordEvidence& evidence) noexcept;
    int advanceChordCursor(
        bool force,
        float onsetStrength = 1.0f) noexcept;
    int startDueGuitarPhrase() noexcept;
    int startNextAutomaticPhrase() noexcept;

    const SongPackage* song = nullptr;
    double sampleRate = 48'000.0;
    double songTimeSeconds = 0.0;
    int nextPhraseIndex = 0;
    int pendingDuePhraseIndex = -1;
    int lastStartedPhraseIndex = -1;
    double lastPhraseTriggerPerformanceSeconds = 0.0;
    int nextChordEventIndex = -1;
    int currentChordEventIndex = -1;
    double currentChordPerformanceStartSeconds = 0.0;
    bool running = false;
    double secondsSinceChordMatch = 0.0;
    double activeSecondsSinceChordMatch = 0.0;
    double inactiveTailSeconds = 0.0;
    double performanceTimeSeconds = 0.0;
    double tempoScale = 1.0;
    int tempoObservationCount = 0;
    int tempoAnchorEventIndex = -1;
    double tempoAnchorPerformanceSeconds = 0.0;
    int performanceOriginEventIndex = -1;
    double performanceOriginSeconds = 0.0;
    int recoveredSkippedChordCount = 0;
    int expiredPhraseCount = 0;
    int evidenceCorrectionCount = 0;
    int pendingEvidenceCandidate = -1;
    int pendingEvidenceCount = 0;
    static constexpr int maximumTimingAnchors = 8;
    std::array<TimingAnchor, maximumTimingAnchors> timingAnchors {};
    int timingAnchorCount = 0;
    int acceptedTimingAnchorCount = 0;
    int rejectedTimingAnchorCount = 0;
    double guitarVocalLeadSeconds = 0.050;
};

} // namespace mode1
