#pragma once

#include <juce_dsp/juce_dsp.h>

#include <array>

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
    double sampleRate = 48'000.0;
};

} // namespace mode1
