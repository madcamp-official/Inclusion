#pragma once

#include "GuitarOnsetTracker.h"
#include "GuitarChordTracker.h"
#include "PhrasePlayer.h"
#include "PhraseScheduler.h"
#include "SongPackage.h"

#include <algorithm>
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
    void triggerVirtualChord(int rootPitchClass) noexcept
    {
        pendingVirtualChordRoot.store(
            juce::jlimit(0, 11, rootPitchClass));
    }
    void startAutomaticPlayback() noexcept;
    void stopAutomaticPlayback() noexcept;
    [[nodiscard]] bool isAutomaticPlaybackEnabled() const noexcept
    {
        return automaticPlayback.load();
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
        return getResidualKeyShift() + stablePitchShift.load();
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

private:
    int expectedRootForPhrase(int phraseIndex) const noexcept;
    void updateTransposition(const ChordDetection& detection) noexcept;

    SongPackage songPackage;
    PhrasePlayer phrasePlayer;
    PhraseScheduler scheduler;
    GuitarOnsetTracker onsetTracker;
    GuitarChordTracker chordTracker;

    std::atomic<bool> manualTrigger { false };
    std::atomic<bool> automaticPlayback { false };
    std::atomic<int> pendingVirtualChordRoot { -1 };
    std::atomic<bool> guitarActive { false };
    std::atomic<bool> lastOnset { false };
    std::atomic<float> guitarRms { 0.0f };
    std::atomic<int> lastStartedPhrase { -1 };
    std::atomic<int> detectedChordRoot { -1 };
    std::atomic<int> stablePitchShift { 0 };
    std::atomic<int> expressionStrength { 25 };
    std::atomic<int> manualKeyShift { 0 };
    std::atomic<int> selectedKeyAnchor { 0 };
    std::atomic<unsigned int> manualKeyRevision { 0 };
    unsigned int observedManualKeyRevision = 0;
    int pendingPitchShift = 0;
    int pendingPitchShiftCount = 0;
};

} // namespace mode1
