#pragma once

#include "GuitarOnsetTracker.h"
#include "GuitarChordTracker.h"
#include "PhrasePlayer.h"
#include "PhraseScheduler.h"
#include "SongPackage.h"

#include <atomic>

namespace mode1
{

class Mode1Controller
{
public:
    void prepare(double sampleRate, int maximumBlockSize);
    bool loadSongPackage(const juce::File& packageFile, juce::String& error);
    void reset() noexcept;

    void processBlock(
        const float* guitarInput,
        float* outputLeft,
        float* outputRight,
        int numSamples) noexcept;

    void triggerNextPhrase() noexcept { manualTrigger.store(true); }

    [[nodiscard]] bool hasSong() const noexcept { return songPackage.isLoaded(); }
    [[nodiscard]] bool isGuitarActive() const noexcept
    {
        return guitarActive.load();
    }
    [[nodiscard]] bool consumedOnset() const noexcept { return lastOnset.load(); }
    [[nodiscard]] float getGuitarRms() const noexcept { return guitarRms.load(); }
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
    [[nodiscard]] int getPitchShiftSemitones() const noexcept
    {
        return stablePitchShift.load();
    }
    [[nodiscard]] juce::String getDetectedChordName() const;

private:
    int expectedRootForPhrase(int phraseIndex) const noexcept;
    void updateTransposition(const ChordDetection& detection) noexcept;

    SongPackage songPackage;
    PhrasePlayer phrasePlayer;
    PhraseScheduler scheduler;
    GuitarOnsetTracker onsetTracker;
    GuitarChordTracker chordTracker;

    std::atomic<bool> manualTrigger { false };
    std::atomic<bool> guitarActive { false };
    std::atomic<bool> lastOnset { false };
    std::atomic<float> guitarRms { 0.0f };
    std::atomic<int> lastStartedPhrase { -1 };
    std::atomic<int> detectedChordRoot { -1 };
    std::atomic<int> stablePitchShift { 0 };
    int pendingPitchShift = 0;
    int pendingPitchShiftCount = 0;
};

} // namespace mode1
