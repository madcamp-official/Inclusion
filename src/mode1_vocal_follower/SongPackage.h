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
    double firstOnsetOffsetSeconds = 0.0;
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
    // Offset from the configured playback/content start to the first
    // perceptually useful vocal energy.  The scheduler may start the clip
    // this much early so the audible anchor, rather than the WAV boundary,
    // lands on the written score time.
    double audibleOnsetOffsetSeconds = 0.0;
    double vowelOnsetOffsetSeconds = -1.0;
    double vocalAnchorConfidence = 0.0;
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
    [[nodiscard]] double getIntroAlignmentOffsetSeconds() const noexcept
    {
        return introAlignmentOffsetSeconds;
    }
    // Negative means "no override": IntroChromaAligner uses its normal
    // best-scoring hypothesis untouched. A repetitive intro (e.g. a 2-bar
    // chord vamp) can make several hypotheses with very different tempo
    // scales score almost identically, since harmonic content alone can't
    // tell which repetition of the pattern is being heard -- a song whose
    // intro-lock is known (by measurement) to land on the wrong one can set
    // this to bias the near-tied-hypothesis tie-break toward its real
    // measured tempo instead.
    [[nodiscard]] double getIntroAlignmentPreferredScale() const noexcept
    {
        return introAlignmentPreferredScale;
    }
    // The hypothesis bank IntroChromaAligner searches spans
    // [center - halfRange, center + halfRange]. Defaults reproduce the
    // original hardcoded 0.92-1.06 range exactly, so a song without a
    // measured tempo is untouched. A song whose intro is known (by
    // measurement) to repeat too regularly for chroma alone to pick the
    // right repetition can narrow this so a wrong-repetition candidate
    // is never even in the search space, rather than hoping a tie-break
    // finds the right one among candidates that happened to score close.
    [[nodiscard]] double getIntroAlignmentScaleCenter() const noexcept
    {
        return introAlignmentScaleCenter;
    }
    [[nodiscard]] double getIntroAlignmentScaleHalfRange() const noexcept
    {
        return introAlignmentScaleHalfRange;
    }
    // Multiplies the rendered vocal level in PhrasePlayer. Defaults to 1.0
    // (no change) so a package without this field sounds exactly as before.
    [[nodiscard]] double getVocalOutputGain() const noexcept
    {
        return vocalOutputGain;
    }
    // Pure output-side delay applied to the rendered vocal audio, after all
    // score tracking. Unlike intro_alignment_offset_sec (which feeds back
    // into the tracking math and can shift which onset-match candidate
    // wins downstream), this only shifts samples in time -- a linear,
    // predictable "push everything N seconds later" knob. Defaults to 0.0
    // (no change).
    [[nodiscard]] double getVocalOutputDelaySeconds() const noexcept
    {
        return vocalOutputDelaySeconds;
    }
    // See PhraseScheduler::setDisableBoundaryPrediction(). Defaults to
    // false, so a song without this field keeps its current chord-cursor
    // behaviour exactly.
    [[nodiscard]] bool getDisableBoundaryPrediction() const noexcept
    {
        return disableBoundaryPrediction;
    }
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
    // PredictiveTransport's IntroChromaAligner lock is empirically biased
    // early by a song-dependent amount (its intro's own chord content and
    // duration change how ambiguous the chroma match is). This constant
    // was tuned against Bansanka; other songs override it via the
    // "intro_alignment_offset_sec" package field.
    double introAlignmentOffsetSeconds = 0.540;
    double introAlignmentPreferredScale = -1.0;
    double introAlignmentScaleCenter = 0.99;
    double introAlignmentScaleHalfRange = 0.07;
    double vocalOutputGain = 1.0;
    double vocalOutputDelaySeconds = 0.0;
    bool disableBoundaryPrediction = false;
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
