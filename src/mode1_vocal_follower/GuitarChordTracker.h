#pragma once

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <array>
#include <cstdint>

namespace mode1
{

enum class ChordQuality
{
    major,
    minor,
    diminished,
    suspended2,
    suspended4,
    dominant7,
    major7,
    minor7,
    unknown,
};

struct ChordDetection
{
    bool valid = false;
    int rootPitchClass = 0;
    int bassPitchClass = -1;
    ChordQuality quality = ChordQuality::unknown;
    bool minor = false;
    float confidence = 0.0f;
    int pitchClassMask = 0;
    std::array<float, 12> chroma {};
};

struct ChromaFrame
{
    std::array<float, 12> values {};
    std::uint64_t sequence = 0;
    bool valid = false;
};

class GuitarChordTracker
{
public:
    void prepare(double sampleRate) noexcept;
    void reset() noexcept;

    // Returns true once a delayed chord estimate is ready after an onset.
    bool processBlock(
        const float* guitarInput,
        int numSamples,
        bool onset,
        ChordDetection& detection) noexcept;

    // A low-rate continuous feature stream for score alignment. Unlike the
    // chord result above, this is not gated by onset confirmation.
    [[nodiscard]] ChromaFrame getLatestChromaFrame() const noexcept
    {
        return latestChromaFrame;
    }

    // Restricts chord classification to only the (root, quality)
    // combinations that actually appear in the loaded song, instead of
    // searching all 12 roots x 8 qualities. A guitar's real overtone/pick-
    // noise content can score higher than the true chord against an
    // off-script template, and that spurious detection can feed into
    // PhraseScheduler::applyChordEvidence and jump the score cursor ahead
    // incorrectly. An empty set (the default) disables the restriction and
    // reproduces the original unrestricted search exactly.
    void setAllowedChords(
        const std::array<bool, 12 * 9>& allowed) noexcept
    {
        allowedChords = allowed;
        anyAllowedChordSet = std::any_of(
            allowed.begin(), allowed.end(), [](bool value) { return value; });
    }
    void clearAllowedChords() noexcept
    {
        allowedChords.fill(false);
        anyAllowedChordSet = false;
    }

private:
    ChordDetection analyse() noexcept;

    static constexpr int fftOrder = 12;
    static constexpr int fftSize = 1 << fftOrder;

    juce::dsp::FFT fft { fftOrder };
    juce::dsp::WindowingFunction<float> window {
        fftSize,
        juce::dsp::WindowingFunction<float>::hann,
    };
    std::array<float, fftSize> ring {};
    std::array<float, fftSize * 2> fftData {};
    int writePosition = 0;
    int validSamples = 0;
    int pendingAnalysisSamples = -1;
    int samplesUntilContinuousAnalysis = 0;
    std::uint64_t chromaSequence = 0;
    ChromaFrame latestChromaFrame;
    double sampleRate = 48'000.0;
    std::array<bool, 12 * 9> allowedChords {};
    bool anyAllowedChordSet = false;
};

} // namespace mode1
