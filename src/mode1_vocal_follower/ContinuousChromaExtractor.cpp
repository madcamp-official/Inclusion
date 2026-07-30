#include "ContinuousChromaExtractor.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace mode1
{

void ContinuousChromaExtractor::prepare(double newSampleRate) noexcept
{
    sampleRate = std::max(1.0, newSampleRate);
    reset();
}

void ContinuousChromaExtractor::reset() noexcept
{
    ring.fill(0.0f);
    fftData.fill(0.0f);
    writePosition = 0;
    validSamples = 0;
    samplesUntilAnalysis = 0;
    sequence = 0;
    processedSamples = 0;
    latestWindowStartSample = 0;
    latestFrame = {};
}

void ContinuousChromaExtractor::processBlock(
    const float* input,
    int numSamples) noexcept
{
    if (input == nullptr || numSamples <= 0)
        return;
    for (int sample = 0; sample < numSamples; ++sample)
    {
        ring[static_cast<size_t>(writePosition)] = input[sample];
        writePosition = (writePosition + 1) % fftSize;
        ++processedSamples;
        validSamples = std::min(fftSize, validSamples + 1);

        if (validSamples < fftSize)
            continue;
        if (sequence == 0)
        {
            analyse();
            latestWindowStartSample = processedSamples - fftSize;
            samplesUntilAnalysis =
                std::max(1, juce::roundToInt(0.020 * sampleRate));
            continue;
        }
        --samplesUntilAnalysis;
        if (samplesUntilAnalysis <= 0)
        {
            analyse();
            latestWindowStartSample = processedSamples - fftSize;
            samplesUntilAnalysis =
                std::max(1, juce::roundToInt(0.020 * sampleRate));
        }
    }
}

void ContinuousChromaExtractor::analyse() noexcept
{
    for (int index = 0; index < fftSize; ++index)
    {
        const int ringIndex = (writePosition + index) % fftSize;
        fftData[static_cast<size_t>(index)] =
            ring[static_cast<size_t>(ringIndex)];
    }
    std::fill(fftData.begin() + fftSize, fftData.end(), 0.0f);
    window.multiplyWithWindowingTable(fftData.data(), fftSize);
    fft.performFrequencyOnlyForwardTransform(fftData.data());

    std::array<float, 12> chroma {};
    const int firstBin = std::max(
        1,
        static_cast<int>(70.0 * fftSize / sampleRate));
    const int lastBin = std::min(
        fftSize / 2,
        static_cast<int>(2'000.0 * fftSize / sampleRate));
    for (int bin = firstBin; bin <= lastBin; ++bin)
    {
        const double frequency = bin * sampleRate / fftSize;
        const double midi = 69.0 + 12.0 * std::log2(frequency / 440.0);
        const int pitchClass =
            ((juce::roundToInt(midi) % 12) + 12) % 12;
        chroma[static_cast<size_t>(pitchClass)] +=
            std::max(0.0f, fftData[static_cast<size_t>(bin)]);
    }
    const float total =
        std::accumulate(chroma.begin(), chroma.end(), 0.0f);
    latestFrame = {};
    latestFrame.sequence = ++sequence;
    latestFrame.valid = total > 1.0e-5f;
    if (!latestFrame.valid)
        return;
    for (size_t pitch = 0; pitch < chroma.size(); ++pitch)
        latestFrame.values[pitch] = chroma[pitch] / total;
}

} // namespace mode1
