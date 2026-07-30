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
            0.020, 0.08, seconds + 0.025);
    }
    void reset() noexcept;

    // When true, chooseChordEventForOnset() skips the tab_tracking
    // multi-candidate lookahead entirely and falls back to the same
    // conservative, purely-reactive matching a song without tab_tracking
    // already uses (match the immediate next expected event only, minimal
    // early tolerance). Off by default -- a song that never calls this
    // keeps its exact current behaviour.
    void setDisableBoundaryPrediction(bool disable) noexcept
    {
        disableBoundaryPrediction = disable;
    }

    // Returns the phrase index to start, or -1 when no phrase starts.
    //
    // activeVocalLeadEnabled asks every phrase target to move
    // activeMusicalAnticipationSeconds plus that phrase's own content offset
    // earlier, so the caller can start playback from sample 0 of the clip
    // (full natural consonant, Oracle C) instead of jumping straight to the
    // scored vowel, while the vowel still lands close to the intended beat.
    // It defaults to false, which reproduces the exact prior target-time
    // computation -- Baseline and Shadow never set it.
    int processBlock(
        int numSamples,
        bool guitarOnset,
        bool manualTrigger,
        bool automaticPlayback = false,
        bool guitarActive = false,
        float onsetStrength = 1.0f,
        const ChordEvidence* chordEvidence = nullptr,
        bool predictNextBoundary = false,
        double predictedScoreSeconds = -1.0,
        bool activeVocalLeadEnabled = false) noexcept;

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
    [[nodiscard]] int getScheduledPhraseDelaySamples() const noexcept
    {
        return juce::roundToInt(
            scheduledPhraseDelaySeconds * sampleRate);
    }
    // False for a phrase that fired the instant it became next in line at an
    // already (or just-)confirmed chord boundary -- that path has no
    // target-time gating, so the caller must not also ask PhrasePlayer to
    // start earlier in the clip, which would only delay the vowel.
    [[nodiscard]] bool lastPhraseUsedLeadTiming() const noexcept
    {
        return lastPhraseStartUsedLeadTiming;
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
        float onsetStrength = 1.0f,
        int* boundaryPhrase = nullptr) noexcept;
    int startBoundaryGracePhrase() noexcept;
    int startDueGuitarPhrase() noexcept;
    int startNextAutomaticPhrase() noexcept;
    [[nodiscard]] double activeVocalLeadSecondsForPhrase(
        int phraseIndex) const noexcept;

    const SongPackage* song = nullptr;
    bool disableBoundaryPrediction = false;
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
    int stablePredictionAnchorCount = 0;
    double lastTimingPredictionResidualSeconds = 0.0;
    double guitarVocalLeadSeconds = 0.025;
    double predictiveLookaheadSeconds = 0.120;
    double scheduledPhraseDelaySeconds = 0.0;
    double earlyBoundaryCandidateSeconds = -1.0;
    double learnedArpeggioPhaseLeadSeconds = 0.0;
    int arpeggioPhaseObservationCount = 0;
    bool predictionEnabledForBlock = false;
    double predictedScoreSecondsForBlock = -1.0;
    bool activeVocalLeadEnabledForBlock = false;
    bool lastPhraseStartUsedLeadTiming = false;
    // A deliberate, package-independent "sung ahead of the beat" amount,
    // confirmed by ear against real guitar takes (Oracle B leadin test:
    // nudging the whole utterance 100 ms earlier read as most accurate).
    // Kept separate from guitarVocalLeadSeconds, which compensates measured
    // output latency, not musical timing.
    static constexpr double activeMusicalAnticipationSeconds = 0.100;
    // The intro has already supplied several causal timing anchors before
    // phrase zero. Match the proven v2 look-ahead for that one entry only;
    // later phrases retain the conservative anticipation above.
    static constexpr double firstVocalIntroAnticipationSeconds = 0.250;
    // Upper bound on how far ahead a single phrase's own content-offset pad
    // can push its trigger time, independent of the package that produced it.
    static constexpr double activeVocalLeadCapSeconds = 0.30;
};

} // namespace mode1
