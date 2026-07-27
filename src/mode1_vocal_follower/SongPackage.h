#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <vector>

namespace mode1
{

struct ChordEvent
{
    double startSeconds = 0.0;
    juce::String chord;
    juce::String rawChord;
};

struct Phrase
{
    juce::String id;
    juce::String lyrics;
    double scoreStartSeconds = 0.0;
    double scoreEndSeconds = 0.0;
    double sourceStartSeconds = 0.0;
    double sourceEndSeconds = 0.0;
    double contentOffsetSeconds = 0.0;
    juce::File vocalFile;
    std::vector<ChordEvent> chords;
};

class SongPackage
{
public:
    bool loadFromFile(const juce::File& packageFile, juce::String& error);
    void clear();

    [[nodiscard]] bool isLoaded() const noexcept { return !phrases.empty(); }
    [[nodiscard]] const std::vector<Phrase>& getPhrases() const noexcept { return phrases; }
    [[nodiscard]] const juce::String& getSongName() const noexcept { return songName; }
    [[nodiscard]] double getScoreBpm() const noexcept { return scoreBpm; }
    [[nodiscard]] int getBaseKeyShift() const noexcept { return baseKeyShift; }
    [[nodiscard]] const juce::String& getRangeWarning() const noexcept
    {
        return rangeWarning;
    }
    [[nodiscard]] const juce::File& getSourceFile() const noexcept { return sourceFile; }

private:
    juce::File sourceFile;
    juce::String songName;
    double scoreBpm = 0.0;
    int baseKeyShift = 0;
    juce::String rangeWarning;
    std::vector<Phrase> phrases;
};

} // namespace mode1
