#pragma once

#include "SongPackage.h"

namespace mode1
{

class PhraseScheduler
{
public:
    void prepare(double sampleRate);
    void setSong(const SongPackage* package);
    void setOutputLatencySeconds(double seconds) noexcept
    {
        guitarVocalLeadSeconds = juce::jlimit(0.0, 0.08, seconds);
    }
    void reset() noexcept;

    // Returns the phrase index to start, or -1 when no phrase starts.
    int processBlock(
        int numSamples,
        bool guitarOnset,
        bool manualTrigger,
        bool automaticPlayback = false,
        bool guitarActive = false,
        float onsetStrength = 1.0f) noexcept;

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

private:
    [[nodiscard]] double normalizedScoreStartTime(int phraseIndex) const noexcept;
    [[nodiscard]] int firstPlayableChordEvent() const noexcept;
    [[nodiscard]] int nextPlayableChordEvent(int afterIndex) const noexcept;
    [[nodiscard]] int chooseChordEventForOnset(
        float onsetStrength) const noexcept;
    [[nodiscard]] double scoreBeatSeconds() const noexcept;
    void updateTempoEstimate(int matchedEventIndex) noexcept;
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
    double guitarVocalLeadSeconds = 0.10;
};

} // namespace mode1
