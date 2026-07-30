#pragma once

#include "GuitarChordTracker.h"

#include <juce_dsp/juce_dsp.h>

#include <array>
#include <cstdint>

namespace mode1
{

// High-resolution continuous chroma used exclusively by Active v2 Shadow.
// Its FFT/window/frequency range mirrors tools/mode1_oracle/chroma_align.py.
class ContinuousChromaExtractor
{
public:
    void prepare(double sampleRate) noexcept;
    void reset() noexcept;
    void processBlock(const float* input, int numSamples) noexcept;

    [[nodiscard]] ChromaFrame getLatestFrame() const noexcept
    {
        return latestFrame;
    }

    // The feature describes a window beginning this many samples before the
    // callback position. Keeping this timestamp convention matches Oracle.
    [[nodiscard]] int getWindowSize() const noexcept { return fftSize; }
    [[nodiscard]] std::int64_t getLatestWindowStartSample() const noexcept
    {
        return latestWindowStartSample;
    }

private:
    void analyse() noexcept;

    static constexpr int fftOrder = 13;
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
    int samplesUntilAnalysis = 0;
    double sampleRate = 48'000.0;
    std::uint64_t sequence = 0;
    std::int64_t processedSamples = 0;
    std::int64_t latestWindowStartSample = 0;
    ChromaFrame latestFrame;
};

} // namespace mode1
