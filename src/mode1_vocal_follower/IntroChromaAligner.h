#pragma once

#include "GuitarChordTracker.h"
#include "SongPackage.h"

#include <array>
#include <cstdint>

namespace mode1
{

struct IntroAlignment
{
    bool locked = false;
    double performanceSecondsPerScoreSecond = 1.0;
    double offsetSeconds = 0.0;
    double confidence = 0.0;
    double harmonicMargin = 0.0;
    int evidenceFrames = 0;
};

// Fixed-allocation hypothesis bank used only while the score is in its intro.
// It aligns continuous guitar chroma to the known song chord sequence, thereby
// avoiding the arpeggio/onset ambiguity of the reactive chord cursor.
class IntroChromaAligner
{
public:
    void prepare(double sampleRate) noexcept;
    void setSong(const SongPackage* package) noexcept;
    void reset() noexcept;
    void noteFirstOnset(std::int64_t sample) noexcept;
    void processFrame(
        std::int64_t sample,
        const ChromaFrame& frame,
        bool guitarActive) noexcept;

    [[nodiscard]] IntroAlignment getAlignment() const noexcept
    {
        return result;
    }

private:
    struct Hypothesis
    {
        double scale = 1.0;
        double offsetSeconds = 0.0;
        double score = 0.0;
        int frames = 0;
    };

    static std::array<float, 12> makeChordTemplate(
        const juce::String& chord) noexcept;
    const std::array<float, 12>& templateAt(double scoreSeconds) const noexcept;
    void initialiseHypotheses() noexcept;
    void accumulateFrame(
        std::int64_t sample,
        const std::array<float, 12>& values) noexcept;
    void updateResult(double performanceSeconds) noexcept;

    static constexpr int maximumChordTemplates = 256;
    static constexpr int maximumHypotheses = 512;
    static constexpr int historyCapacity = 64;
    static constexpr int smoothingCapacity = 25;

    const SongPackage* song = nullptr;
    double sampleRate = 48'000.0;
    std::int64_t firstOnsetSample = -1;
    std::uint64_t lastFrameSequence = 0;
    int hypothesisCount = 0;
    int evidenceFrames = 0;
    int stableWinnerFrames = 0;
    double previousWinnerScale = 0.0;
    double previousWinnerOffsetSeconds = 0.0;
    std::array<Hypothesis, maximumHypotheses> hypotheses {};
    std::array<std::array<float, 12>, maximumChordTemplates> chordTemplates {};
    std::array<ChromaFrame, historyCapacity> frameHistory {};
    std::array<std::int64_t, historyCapacity> frameHistorySamples {};
    int historyWriteIndex = 0;
    int historySize = 0;
    std::array<std::array<float, 12>, smoothingCapacity> smoothingHistory {};
    std::array<float, 12> smoothingSum {};
    int smoothingWriteIndex = 0;
    int smoothingSize = 0;
    IntroAlignment result;
};

} // namespace mode1
