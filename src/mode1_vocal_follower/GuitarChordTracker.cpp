#include "GuitarChordTracker.h"

#include <algorithm>
#include <cmath>

namespace mode1
{

void GuitarChordTracker::prepare(double newSampleRate) noexcept
{
    sampleRate = std::max(1.0, newSampleRate);
    reset();
}

void GuitarChordTracker::reset() noexcept
{
    ring.fill(0.0f);
    fftData.fill(0.0f);
    writePosition = 0;
    validSamples = 0;
    pendingAnalysisSamples = -1;
}

bool GuitarChordTracker::processBlock(
    const float* guitarInput,
    int numSamples,
    bool onset,
    ChordDetection& detection) noexcept
{
    detection = {};
    if (guitarInput == nullptr || numSamples <= 0)
        return false;

    for (int sample = 0; sample < numSamples; ++sample)
    {
        ring[static_cast<size_t>(writePosition)] = guitarInput[sample];
        writePosition = (writePosition + 1) % fftSize;
    }
    validSamples = std::min(fftSize, validSamples + numSamples);

    if (onset)
        pendingAnalysisSamples = juce::roundToInt(0.075 * sampleRate);
    if (pendingAnalysisSamples < 0)
        return false;

    pendingAnalysisSamples -= numSamples;
    if (pendingAnalysisSamples > 0 || validSamples < fftSize / 2)
        return false;

    pendingAnalysisSamples = -1;
    detection = analyse();
    return true;
}

ChordDetection GuitarChordTracker::analyse() noexcept
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
        static_cast<int>(1'200.0 * fftSize / sampleRate));

    for (int bin = firstBin; bin <= lastBin; ++bin)
    {
        const double frequency = bin * sampleRate / fftSize;
        const double midi = 69.0 + 12.0 * std::log2(frequency / 440.0);
        const int roundedMidi = juce::roundToInt(midi);
        const int pitchClass = ((roundedMidi % 12) + 12) % 12;
        const float magnitude =
            std::sqrt(std::max(0.0f, fftData[static_cast<size_t>(bin)]));
        chroma[static_cast<size_t>(pitchClass)] += magnitude;
    }

    const float total =
        std::accumulate(chroma.begin(), chroma.end(), 0.0f);
    if (total <= 1.0e-5f)
        return {};
    for (auto& value : chroma)
        value /= total;

    float bestScore = -1.0f;
    float secondScore = -1.0f;
    int bestRoot = 0;
    bool bestMinor = false;

    for (int root = 0; root < 12; ++root)
    {
        for (bool minor : { false, true })
        {
            const int third = (root + (minor ? 3 : 4)) % 12;
            const int fifth = (root + 7) % 12;
            const float chordEnergy =
                chroma[static_cast<size_t>(root)]
                + 0.85f * chroma[static_cast<size_t>(third)]
                + 0.90f * chroma[static_cast<size_t>(fifth)];
            const float score = chordEnergy;

            if (score > bestScore)
            {
                secondScore = bestScore;
                bestScore = score;
                bestRoot = root;
                bestMinor = minor;
            }
            else if (score > secondScore)
            {
                secondScore = score;
            }
        }
    }

    const float confidence =
        bestScore > 0.0f ? (bestScore - secondScore) / bestScore : 0.0f;
    return {
        true,
        bestRoot,
        bestMinor,
        confidence,
    };
}

} // namespace mode1
