#pragma once

#include "GuitarOnsetTracker.h"
#include "GuitarChordTracker.h"
#include "ChordMismatchGate.h"
#include "BeatClock.h"
#include "PredictivePhraseScheduler.h"
#include "PredictiveTransport.h"
#include "IntroChromaAligner.h"
#include "ContinuousChromaExtractor.h"
#include "PhrasePlayer.h"
#include "PhraseScheduler.h"
#include "RealtimeTraceBuffer.h"
#include "SongPackage.h"

#include <algorithm>
#include <atomic>
#include <cmath>

namespace mode1
{

enum class FollowerMode : std::uint8_t
{
    baseline,
    predictiveShadow,
    predictiveActive,
    activeV2Shadow,
    activeV2
};

class Mode1Controller
{
public:
    void prepare(double sampleRate, int maximumBlockSize);
    void setOutputLatencySeconds(double seconds) noexcept
    {
        scheduler.setOutputLatencySeconds(seconds);
        predictiveScheduler.setOutputLatencySeconds(seconds);
        outputLatencySamplesV2 = static_cast<std::int64_t>(std::llround(
            std::max(0.0, seconds) * controllerSampleRate));
    }
    void setInputLatencySeconds(double seconds) noexcept;
    void setRealtimeTraceEnabled(bool enabled) noexcept
    {
        realtimeTraceEnabled.store(enabled);
    }
    bool popRealtimeTraceEvent(RealtimeTraceEvent& event) noexcept
    {
        return realtimeTrace.pop(event);
    }
    [[nodiscard]] std::uint64_t getDroppedRealtimeTraceCount() const noexcept
    {
        return realtimeTrace.getDroppedCount();
    }
    [[nodiscard]] ChromaFrame getDiagnosticContinuousChromaFrame() const noexcept
    {
        return continuousChromaExtractor.getLatestFrame();
    }
    [[nodiscard]] std::int64_t getDiagnosticChromaWindowStartSample() const noexcept
    {
        return continuousChromaExtractor.getLatestWindowStartSample();
    }
    bool loadSongPackage(const juce::File& packageFile, juce::String& error);
    void reset() noexcept;

    void processBlock(
        const float* guitarInput,
        float* outputLeft,
        float* outputRight,
        int numSamples) noexcept;

    void triggerNextPhrase() noexcept { manualTrigger.store(true); }
    void triggerVirtualChord(int rootPitchClass) noexcept
    {
        pendingVirtualChordRoot.store(
            juce::jlimit(0, 11, rootPitchClass));
    }
    void startAutomaticPlayback() noexcept;
    void stopAutomaticPlayback() noexcept;
    void startPerformance() noexcept;
    void restartPerformance() noexcept;
    void stopPerformance() noexcept;
    [[nodiscard]] bool isAutomaticPlaybackEnabled() const noexcept
    {
        return automaticPlayback.load();
    }
    [[nodiscard]] bool isPerformanceRunning() const noexcept
    {
        return performanceRunning.load();
    }
    bool setExpressionStrength(
        int strength,
        juce::String* error = nullptr);
    bool setManualKeyShift(
        int semitones,
        juce::String* error = nullptr);

    [[nodiscard]] bool hasSong() const noexcept { return songPackage.isLoaded(); }
    [[nodiscard]] bool isGuitarActive() const noexcept
    {
        return guitarActive.load();
    }
    [[nodiscard]] bool consumedOnset() const noexcept { return lastOnset.load(); }
    [[nodiscard]] bool isVocalRest() const noexcept { return vocalRest.load(); }
    [[nodiscard]] float getGuitarRms() const noexcept { return guitarRms.load(); }
    [[nodiscard]] double getPerformanceTempoScale() const noexcept
    {
        return scheduler.getTempoScale();
    }
    [[nodiscard]] int getRecoveredSkippedChordCount() const noexcept
    {
        return scheduler.getRecoveredSkippedChordCount();
    }
    [[nodiscard]] int getExpiredPhraseCount() const noexcept
    {
        return scheduler.getExpiredPhraseCount();
    }
    [[nodiscard]] int getEvidenceCorrectionCount() const noexcept
    {
        return scheduler.getEvidenceCorrectionCount();
    }
    [[nodiscard]] int getAcceptedTimingAnchorCount() const noexcept
    {
        return scheduler.getAcceptedTimingAnchorCount();
    }
    [[nodiscard]] int getRejectedTimingAnchorCount() const noexcept
    {
        return scheduler.getRejectedTimingAnchorCount();
    }
    [[nodiscard]] int getCurrentChordEventIndex() const noexcept
    {
        return scheduler.getCurrentChordEventIndex();
    }
    [[nodiscard]] int getCurrentPhraseIndex() const noexcept
    {
        const int playingIndex = phrasePlayer.getCurrentPhraseIndex();
        return playingIndex >= 0 ? playingIndex : lastStartedPhrase.load();
    }
    [[nodiscard]] juce::String getCurrentLyrics() const;
    [[nodiscard]] juce::String getSongName() const { return songPackage.getSongName(); }
    [[nodiscard]] int getBaseKeyShift() const noexcept
    {
        return songPackage.getBaseKeyShift();
    }
    [[nodiscard]] juce::String getRangeWarning() const
    {
        return songPackage.getRangeWarning();
    }
    [[nodiscard]] int getExpressionStrength() const noexcept
    {
        return expressionStrength.load();
    }
    [[nodiscard]] int getDefaultExpressionStrength() const noexcept
    {
        return songPackage.getDefaultExpressionStrength();
    }
    [[nodiscard]] int getPitchShiftSemitones() const noexcept
    {
        return juce::jlimit(-3, 3, getResidualKeyShift());
    }
    [[nodiscard]] int getManualKeyShift() const noexcept
    {
        return manualKeyShift.load();
    }
    [[nodiscard]] int getEffectiveBaseKeyShift() const noexcept
    {
        return getBaseKeyShift() + manualKeyShift.load();
    }
    [[nodiscard]] int getMaximumManualKeyShift() const noexcept
    {
        return songPackage.getMaximumManualKeyShift();
    }
    [[nodiscard]] int getMinimumManualKeyShift() const noexcept
    {
        return songPackage.getMinimumManualKeyShift();
    }
    [[nodiscard]] bool hasMultipleExpressionStrengths() const noexcept
    {
        return songPackage.getExpressionStrengths().size() > 1;
    }
    [[nodiscard]] int getSelectedKeyAnchor() const noexcept
    {
        return selectedKeyAnchor.load();
    }
    [[nodiscard]] int getResidualKeyShift() const noexcept
    {
        return getEffectiveBaseKeyShift() - selectedKeyAnchor.load();
    }
    [[nodiscard]] juce::String getDetectedChordName() const;
    [[nodiscard]] juce::String getRawDetectedChordName() const;
    [[nodiscard]] BeatClockSnapshot getBeatClockSnapshot() const noexcept
    {
        return beatClock.getSnapshot();
    }
    void setFollowerMode(FollowerMode mode) noexcept
    {
        followerMode.store(mode);
    }
    [[nodiscard]] FollowerMode getFollowerMode() const noexcept
    {
        return followerMode.load();
    }
    void setPauseOnChordMismatchEnabled(bool enabled) noexcept
    {
        pauseOnChordMismatchEnabled.store(enabled);
        if (!enabled)
            releaseChordMismatchPauseRequested.store(true);
    }
    [[nodiscard]] bool isPauseOnChordMismatchEnabled() const noexcept
    {
        return pauseOnChordMismatchEnabled.load();
    }
    [[nodiscard]] bool isPausedForChordMismatch() const noexcept
    {
        return chordMismatchPausedForUi.load();
    }
    void setFollowPerformanceTempoEnabled(bool enabled) noexcept
    {
        followPerformanceTempoEnabled.store(enabled);
        if (!enabled)
            vocalDurationScale.store(1.0);
    }
    [[nodiscard]] bool isFollowPerformanceTempoEnabled() const noexcept
    {
        return followPerformanceTempoEnabled.load();
    }
    [[nodiscard]] double getVocalDurationScale() const noexcept
    {
        return vocalDurationScale.load();
    }

private:
    void processSubBlock(
        const float* guitarInput,
        float* outputLeft,
        float* outputRight,
        int numSamples) noexcept;
    int expectedRootForPhrase(int phraseIndex) const noexcept;
    int expectedRootForChordEvent(int eventIndex) const noexcept;
    float expectedChordSimilarity(
        int eventIndex,
        const std::array<float, 12>& inputChroma) const noexcept;
    void releaseChordMismatchPause(
        std::int64_t decisionSample,
        int eventIndex) noexcept;
    void updateTransposition(const ChordDetection& detection) noexcept;
    [[nodiscard]] juce::String formatRawDetectedChordName() const;
    [[nodiscard]] juce::String guidedChordName() const;
    void startPhraseImmediately(int phraseIndex) noexcept;
    void scheduleActiveV2Phrase(
        int phraseIndex,
        std::int64_t targetSample,
        std::int64_t blockStartSample) noexcept;
    [[nodiscard]] double calculateVocalDurationScale() const noexcept;

    SongPackage songPackage;
    PhrasePlayer phrasePlayer;
    PhraseScheduler scheduler;
    GuitarOnsetTracker onsetTracker;
    GuitarChordTracker chordTracker;
    BeatClock beatClock;
    PredictivePhraseScheduler predictiveScheduler;
    PredictiveTransport predictiveTransport;
    IntroChromaAligner introChromaAligner;
    ContinuousChromaExtractor continuousChromaExtractor;
    ChordMismatchGate chordMismatchGate;

    std::atomic<bool> manualTrigger { false };
    std::atomic<bool> automaticPlayback { false };
    std::atomic<bool> performanceRunning { false };
    std::atomic<int> pendingVirtualChordRoot { -1 };
    std::atomic<bool> guitarActive { false };
    std::atomic<bool> lastOnset { false };
    std::atomic<bool> vocalRest { false };
    std::atomic<float> guitarRms { 0.0f };
    std::atomic<int> lastStartedPhrase { -1 };
    std::atomic<int> detectedChordRoot { -1 };
    std::atomic<int> detectedChordBass { -1 };
    std::atomic<int> detectedChordQuality {
        static_cast<int>(ChordQuality::unknown)
    };
    std::atomic<int> performanceKeyOffset { 0 };
    std::atomic<bool> performanceKeyOffsetLocked { false };
    std::atomic<int> currentChordEventForUi { -1 };
    std::atomic<int> expressionStrength { 25 };
    std::atomic<int> manualKeyShift { 0 };
    std::atomic<int> selectedKeyAnchor { 0 };
    std::atomic<unsigned int> manualKeyRevision { 0 };
    unsigned int observedManualKeyRevision = 0;
    int pendingPitchShift = 0;
    int pendingPitchShiftCount = 0;
    int pendingPitchShiftLastEventIndex = -1;
    int pendingPitchShiftDistinctEventCount = 0;
    int pendingCommittedPhrase = -1;
    int pendingCommitBlocks = 0;
    int maximumInternalBlockSize = 128;
    double controllerSampleRate = 48'000.0;
    std::int64_t processedSamples = 0;
    std::int64_t inputLatencySamples = 0;
    std::int64_t outputLatencySamplesV2 = 0;
    int tracedChordEventIndex = -1;
    int beatClockObservedScoreEventIndex = -1;
    BeatClockState tracedBeatClockState = BeatClockState::disarmed;
    BeatClockState tracedPredictiveTransportState =
        BeatClockState::disarmed;
    bool introAlignmentApplied = false;
    std::int64_t lastPhysicalOnsetForChordAnalysis = -1;
    std::atomic<bool> realtimeTraceEnabled { false };
    RealtimeTraceBuffer realtimeTrace;
    std::atomic<FollowerMode> followerMode { FollowerMode::predictiveShadow };
    std::atomic<bool> pauseOnChordMismatchEnabled { false };
    std::atomic<bool> releaseChordMismatchPauseRequested { false };
    std::atomic<bool> chordMismatchPausedForUi { false };
    std::atomic<bool> followPerformanceTempoEnabled { false };
    std::atomic<double> vocalDurationScale { 1.0 };
    int lastChordOnsetScoreEventForAnalysis = -1;
};

} // namespace mode1
