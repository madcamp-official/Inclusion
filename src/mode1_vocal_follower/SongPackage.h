#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <algorithm>
#include <vector>

namespace mode1
{

struct ChordEvent
{
    double startSeconds = 0.0;
    juce::String chord;
    juce::String rawChord;
};

struct TabChordHint
{
    int chordEventIndex = -1;
    int pitchClassMask = 0;
    int bassPitchClassMask = 0;
    int noteGroupCount = 0;
    double arpeggioLikelihood = 0.0;
};

struct VocalVariant
{
    int strength = 25;
    int keyShift = 0;
    double contentOffsetSeconds = 0.0;
    juce::File vocalFile;
    double playbackStartSeconds = -1.0;
    double playbackEndSeconds = -1.0;
};

struct Phrase
{
    juce::String id;
    juce::String lyrics;
    double scoreStartSeconds = 0.0;
    double scoreEndSeconds = 0.0;
    double sourceStartSeconds = 0.0;
    double sourceEndSeconds = 0.0;
    int anchorChordEventIndex = -1;
    double chordRelativeStartSeconds = 0.0;
    double contentOffsetSeconds = 0.0;
    juce::File vocalFile;
    std::vector<VocalVariant> vocalVariants;
    std::vector<ChordEvent> chords;
};

class SongPackage
{
public:
    bool loadFromFile(const juce::File& packageFile, juce::String& error);
    void clear();

    [[nodiscard]] bool isLoaded() const noexcept { return !phrases.empty(); }
    [[nodiscard]] const std::vector<Phrase>& getPhrases() const noexcept { return phrases; }
    [[nodiscard]] const std::vector<ChordEvent>& getChordTimeline() const noexcept
    {
        return chordTimeline;
    }
    [[nodiscard]] const std::vector<TabChordHint>& getTabChordHints() const noexcept
    {
        return tabChordHints;
    }
    [[nodiscard]] const TabChordHint* getTabChordHint(
        int chordEventIndex) const noexcept;
    [[nodiscard]] bool hasTabTracking() const noexcept
    {
        return !tabChordHints.empty();
    }
    [[nodiscard]] const juce::String& getSongName() const noexcept { return songName; }
    [[nodiscard]] double getScoreBpm() const noexcept { return scoreBpm; }
    [[nodiscard]] int getBaseKeyShift() const noexcept { return baseKeyShift; }
    [[nodiscard]] const juce::String& getRangeWarning() const noexcept
    {
        return rangeWarning;
    }
    [[nodiscard]] const juce::File& getSourceFile() const noexcept { return sourceFile; }
    [[nodiscard]] int getDefaultExpressionStrength() const noexcept
    {
        return defaultExpressionStrength;
    }
    [[nodiscard]] const std::vector<int>& getExpressionStrengths() const noexcept
    {
        return expressionStrengths;
    }
    [[nodiscard]] const std::vector<int>& getKeyAnchors() const noexcept
    {
        return keyAnchors;
    }
    [[nodiscard]] int getNearestKeyAnchor(int targetKeyShift) const noexcept;
    [[nodiscard]] int getMinimumManualKeyShift() const noexcept
    {
        return hasManualKeyShiftRange ? manualKeyShiftMinimum : -6;
    }
    [[nodiscard]] int getMaximumManualKeyShift() const noexcept
    {
        return hasManualKeyShiftRange
            ? manualKeyShiftMaximum
            : std::max(6, -baseKeyShift);
    }

private:
    juce::File sourceFile;
    juce::String songName;
    double scoreBpm = 0.0;
    int baseKeyShift = 0;
    juce::String rangeWarning;
    int defaultExpressionStrength = 25;
    std::vector<int> expressionStrengths { 25 };
    std::vector<int> keyAnchors { 0 };
    bool hasManualKeyShiftRange = false;
    int manualKeyShiftMinimum = -6;
    int manualKeyShiftMaximum = 6;
    std::vector<ChordEvent> chordTimeline;
    std::vector<TabChordHint> tabChordHints;
    std::vector<Phrase> phrases;
};

} // namespace mode1
