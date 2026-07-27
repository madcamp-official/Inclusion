#include "FeedbackNotchSuppressor.h"

#include <algorithm>
#include <cmath>
#include <cstring>

void FeedbackNotchSuppressor::prepare(double sampleRateIn, int maxBlockSize)
{
    sampleRate = sampleRateIn;

    fftScratch.assign(static_cast<size_t>(fftSize) * 2, 0.0f);
    analysisWindow.assign(static_cast<size_t>(fftSize), 0.0f);
    sortedMagnitudes.assign(static_cast<size_t>(spectrumBins), 0.0f);
    analysisFilled = 0;

    hannWindow.resize(static_cast<size_t>(fftSize));
    for (int i = 0; i < fftSize; ++i)
        hannWindow[static_cast<size_t>(i)] = 0.5f - 0.5f * std::cos(juce::MathConstants<float>::twoPi
                                                                    * static_cast<float>(i)
                                                                    / static_cast<float>(fftSize - 1));

    juce::dsp::ProcessSpec spec;
    spec.sampleRate = sampleRateIn;
    spec.maximumBlockSize = static_cast<juce::uint32>(juce::jmax(1, maxBlockSize));
    spec.numChannels = 1;

    for (auto& notch : notches)
        notch.filter.prepare(spec);

    highPass.prepare(spec);
    highPass.coefficients = juce::dsp::IIR::Coefficients<float>::makeHighPass(sampleRateIn, highPassHz);

    reset();
}

void FeedbackNotchSuppressor::reset()
{
    std::fill(analysisWindow.begin(), analysisWindow.end(), 0.0f);
    analysisFilled = 0;
    candidateBin = -1;
    candidateFrameCount = 0;

    for (auto& notch : notches)
    {
        notch.active = false;
        notch.frequencyHz = 0.0f;
        notch.holdBlocksRemaining = 0;
        notch.filter.reset();
    }

    highPass.reset();
}

int FeedbackNotchSuppressor::getActiveNotchCount() const
{
    int count = 0;
    for (const auto& notch : notches)
        if (notch.active)
            ++count;
    return count;
}

float FeedbackNotchSuppressor::getActiveNotchFrequency(int index) const
{
    int seen = 0;
    for (const auto& notch : notches)
    {
        if (! notch.active)
            continue;
        if (seen == index)
            return notch.frequencyHz;
        ++seen;
    }
    return 0.0f;
}

void FeedbackNotchSuppressor::pushForAnalysis(const float* samples, int numSamples)
{
    if (numSamples >= fftSize)
    {
        std::memcpy(analysisWindow.data(), samples + (numSamples - fftSize),
                    sizeof(float) * static_cast<size_t>(fftSize));
        analysisFilled = fftSize;
        return;
    }

    const int keep = fftSize - numSamples;
    std::memmove(analysisWindow.data(), analysisWindow.data() + numSamples,
                 sizeof(float) * static_cast<size_t>(keep));
    std::memcpy(analysisWindow.data() + keep, samples, sizeof(float) * static_cast<size_t>(numSamples));
    analysisFilled = std::min(fftSize, analysisFilled + numSamples);
}

bool FeedbackNotchSuppressor::hasNearbyNotch(float frequencyHz) const
{
    for (const auto& notch : notches)
    {
        if (! notch.active)
            continue;
        // 반음(약 6%) 이내면 같은 하울링으로 본다.
        if (std::abs(notch.frequencyHz - frequencyHz) < frequencyHz * 0.06f)
            return true;
    }
    return false;
}

void FeedbackNotchSuppressor::addNotch(float frequencyHz)
{
    const int holdBlocks = static_cast<int>(notchHoldSeconds * sampleRate / 512.0);

    // 빈 슬롯을 먼저 쓰고, 없으면 가장 오래된(남은 유지 시간이 가장 짧은) 것을 재활용한다.
    Notch* slot = nullptr;
    for (auto& notch : notches)
    {
        if (! notch.active)
        {
            slot = &notch;
            break;
        }
    }

    if (slot == nullptr)
    {
        slot = &notches[0];
        for (auto& notch : notches)
            if (notch.holdBlocksRemaining < slot->holdBlocksRemaining)
                slot = &notch;
    }

    const float gainLinear = juce::Decibels::decibelsToGain(notchGainDb);
    slot->filter.coefficients = juce::dsp::IIR::Coefficients<float>::makePeakFilter(sampleRate,
                                                                                    frequencyHz,
                                                                                    notchQ,
                                                                                    gainLinear);
    slot->filter.reset();
    slot->active = true;
    slot->frequencyHz = frequencyHz;
    slot->holdBlocksRemaining = holdBlocks;
}

void FeedbackNotchSuppressor::analyseSpectrum()
{
    if (analysisFilled < fftSize)
        return;

    std::fill(fftScratch.begin(), fftScratch.end(), 0.0f);
    for (int i = 0; i < fftSize; ++i)
        fftScratch[static_cast<size_t>(i)] = analysisWindow[static_cast<size_t>(i)]
                                             * hannWindow[static_cast<size_t>(i)];

    fft.performFrequencyOnlyForwardTransform(fftScratch.data());

    const float binHz = static_cast<float>(sampleRate) / static_cast<float>(fftSize);
    const int minBin = std::max(1, static_cast<int>(searchMinHz / binHz));
    const int maxBin = std::min(spectrumBins - 1, static_cast<int>(searchMaxHz / binHz));
    if (minBin >= maxBin)
        return;

    // 중앙값을 노이즈 플로어로 쓴다. 평균은 강한 피크 하나에 끌려가므로 부적절하다.
    sortedMagnitudes.assign(fftScratch.begin() + minBin, fftScratch.begin() + maxBin + 1);
    std::nth_element(sortedMagnitudes.begin(),
                     sortedMagnitudes.begin() + sortedMagnitudes.size() / 2,
                     sortedMagnitudes.end());
    const float floorMagnitude = sortedMagnitudes[sortedMagnitudes.size() / 2];

    if (floorMagnitude <= 1.0e-7f)
    {
        candidateBin = -1;
        candidateFrameCount = 0;
        return;
    }

    int peakBin = minBin;
    float peakMagnitude = 0.0f;
    for (int bin = minBin; bin <= maxBin; ++bin)
    {
        const float magnitude = fftScratch[static_cast<size_t>(bin)];
        if (magnitude > peakMagnitude)
        {
            peakMagnitude = magnitude;
            peakBin = bin;
        }
    }

    if (peakMagnitude < floorMagnitude * peakToFloorRatio)
    {
        candidateBin = -1;
        candidateFrameCount = 0;
        return;
    }

    if (candidateBin >= 0 && std::abs(peakBin - candidateBin) <= 1)
    {
        ++candidateFrameCount;
    }
    else
    {
        candidateBin = peakBin;
        candidateFrameCount = 1;
    }

    if (candidateFrameCount >= framesToConfirm)
    {
        const float frequencyHz = static_cast<float>(peakBin) * binHz;
        if (! hasNearbyNotch(frequencyHz))
            addNotch(frequencyHz);
        candidateFrameCount = 0;
    }
}

void FeedbackNotchSuppressor::tickHold(int /*numSamples*/)
{
    for (auto& notch : notches)
    {
        if (! notch.active)
            continue;
        if (--notch.holdBlocksRemaining <= 0)
        {
            notch.active = false;
            notch.frequencyHz = 0.0f;
            notch.filter.reset();
        }
    }
}

void FeedbackNotchSuppressor::processBlock(float* samples, int numSamples)
{
    if (! enabled || numSamples <= 0)
        return;

    // 노치를 걸기 전 신호로 분석해야, 하울링이 계속되는지 여부를 판단할 수 있다.
    pushForAnalysis(samples, numSamples);
    analyseSpectrum();
    tickHold(numSamples);

    for (int i = 0; i < numSamples; ++i)
    {
        float sample = highPass.processSample(samples[i]);
        for (auto& notch : notches)
            if (notch.active)
                sample = notch.filter.processSample(sample);
        samples[i] = sample;
    }
}
