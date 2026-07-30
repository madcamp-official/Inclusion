#include "GuitarChordTracker.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

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
    samplesUntilContinuousAnalysis = 0;
    chromaSequence = 0;
    latestChromaFrame = {};
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

    samplesUntilContinuousAnalysis -= numSamples;
    if (samplesUntilContinuousAnalysis <= 0 && validSamples >= fftSize / 2)
    {
        const auto continuousDetection = analyse();
        latestChromaFrame.values = continuousDetection.chroma;
        latestChromaFrame.valid = std::accumulate(
            latestChromaFrame.values.begin(),
            latestChromaFrame.values.end(),
            0.0f) > 1.0e-5f;
        latestChromaFrame.sequence = ++chromaSequence;
        samplesUntilContinuousAnalysis =
            std::max(1, juce::roundToInt(0.020 * sampleRate));
    }

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
    std::array<float, 128> midiEnergy {};
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
            std::max(0.0f, fftData[static_cast<size_t>(bin)]);
        chroma[static_cast<size_t>(pitchClass)] += magnitude;
        if (roundedMidi >= 36 && roundedMidi < 128 && frequency <= 360.0)
            midiEnergy[static_cast<size_t>(roundedMidi)] += magnitude;
    }

    const float total =
        std::accumulate(chroma.begin(), chroma.end(), 0.0f);
    if (total <= 1.0e-5f)
        return {};
    for (auto& value : chroma)
        value /= total;
    const float strongestChroma =
        *std::max_element(chroma.begin(), chroma.end());
    int pitchClassMask = 0;
    for (int pitchClass = 0; pitchClass < 12; ++pitchClass)
    {
        const float value = chroma[static_cast<size_t>(pitchClass)];
        if (value >= std::max(0.055f, strongestChroma * 0.24f))
            pitchClassMask |= 1 << pitchClass;
    }

    float strongestBass = 0.0f;
    for (int midi = 36; midi <= 66; ++midi)
        strongestBass = std::max(
            strongestBass, midiEnergy[static_cast<size_t>(midi)]);
    int bassPitchClass = -1;
    if (strongestBass > 0.0f)
    {
        for (int midi = 36; midi <= 66; ++midi)
        {
            const float energy = midiEnergy[static_cast<size_t>(midi)];
            const float lower =
                midiEnergy[static_cast<size_t>(midi - 1)];
            const float upper =
                midiEnergy[static_cast<size_t>(midi + 1)];
            if (energy >= strongestBass * 0.35f
                && energy >= lower
                && energy >= upper)
            {
                bassPitchClass = midi % 12;
                break;
            }
        }
    }

    struct Template
    {
        ChordQuality quality;
        std::array<int, 4> intervals;
        int toneCount;
    };
    static constexpr std::array<Template, 8> templates {{
        { ChordQuality::major,      { 0, 4, 7, 0 }, 3 },
        { ChordQuality::minor,      { 0, 3, 7, 0 }, 3 },
        { ChordQuality::diminished, { 0, 3, 6, 0 }, 3 },
        { ChordQuality::suspended2, { 0, 2, 7, 0 }, 3 },
        { ChordQuality::suspended4, { 0, 5, 7, 0 }, 3 },
        { ChordQuality::dominant7,  { 0, 4, 7, 10 }, 4 },
        { ChordQuality::major7,     { 0, 4, 7, 11 }, 4 },
        { ChordQuality::minor7,     { 0, 3, 7, 10 }, 4 },
    }};
    static constexpr std::array<float, 4> toneWeights {
        1.0f, 0.92f, 0.82f, 0.72f,
    };

    float bestScore = -1.0f;
    float secondScore = -1.0f;
    int bestRoot = 0;
    ChordQuality bestQuality = ChordQuality::unknown;
    const float chromaNorm = std::sqrt(std::inner_product(
        chroma.begin(), chroma.end(), chroma.begin(), 0.0f));

    for (int root = 0; root < 12; ++root)
    {
        for (size_t templateIndex = 0; templateIndex < templates.size();
             ++templateIndex)
        {
            const auto& candidate = templates[templateIndex];
            if (anyAllowedChordSet
                && !allowedChords[static_cast<size_t>(root) * 9
                    + templateIndex])
                continue;
            float dot = 0.0f;
            float templateNormSquared = 0.0f;
            float weakestTone = 1.0f;
            for (int tone = 0; tone < candidate.toneCount; ++tone)
            {
                const float weight = toneWeights[static_cast<size_t>(tone)];
                const float energy = chroma[static_cast<size_t>(
                    (root + candidate.intervals[static_cast<size_t>(tone)]) % 12)];
                dot += weight * energy;
                templateNormSquared += weight * weight;
                weakestTone = std::min(weakestTone, energy);
            }

            // Cosine similarity keeps seventh templates from winning over a
            // plain triad when the seventh is absent. A small coverage bonus
            // breaks close guitar-voicing ties in favour of fully present
            // chord tones.
            const float score =
                dot / std::max(
                    1.0e-6f,
                    chromaNorm * std::sqrt(templateNormSquared))
                + 0.10f * weakestTone
                + 0.25f * chroma[static_cast<size_t>(root)]
                + (root == bassPitchClass ? 0.035f : 0.0f);

            if (score > bestScore)
            {
                secondScore = bestScore;
                bestScore = score;
                bestRoot = root;
                bestQuality = candidate.quality;
            }
            else if (score > secondScore)
            {
                secondScore = score;
            }
        }
    }

    const float confidence =
        bestScore > 0.0f ? (bestScore - secondScore) / bestScore : 0.0f;

    if (bassPitchClass < 0)
        bassPitchClass = bestRoot;

    ChordDetection detection;
    detection.valid = bestScore >= 0.42f;
    detection.rootPitchClass = bestRoot;
    detection.bassPitchClass = bassPitchClass;
    detection.quality = bestQuality;
    detection.minor =
        bestQuality == ChordQuality::minor
        || bestQuality == ChordQuality::minor7;
    detection.confidence = confidence;
    detection.pitchClassMask = pitchClassMask;
    detection.chroma = chroma;
    return detection;
}

} // namespace mode1
