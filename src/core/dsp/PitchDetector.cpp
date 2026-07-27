#include "PitchDetector.h"

#include <algorithm>
#include <cmath>
#include <cstring>

void PitchDetector::prepare(double sampleRateIn)
{
    sampleRate = sampleRateIn;
    minLag = static_cast<int>(sampleRate / maxFrequencyHz);
    maxLag = static_cast<int>(sampleRate / minFrequencyHz);
    minLag = std::max(minLag, 1);
    maxLag = std::min(maxLag, analysisWindowSize / 2 - 1);

    window.assign(analysisWindowSize, 0.0f);
    filledSamples = 0;
}

void PitchDetector::reset()
{
    std::fill(window.begin(), window.end(), 0.0f);
    filledSamples = 0;
}

void PitchDetector::pushSamples(const float* samples, int numSamples)
{
    if (numSamples <= 0)
        return;

    if (numSamples >= analysisWindowSize)
    {
        std::memcpy(window.data(), samples + (numSamples - analysisWindowSize),
                    sizeof(float) * static_cast<size_t>(analysisWindowSize));
        filledSamples = analysisWindowSize;
        return;
    }

    const int keep = analysisWindowSize - numSamples;
    std::memmove(window.data(), window.data() + numSamples, sizeof(float) * static_cast<size_t>(keep));
    std::memcpy(window.data() + keep, samples, sizeof(float) * static_cast<size_t>(numSamples));

    filledSamples = std::min(analysisWindowSize, filledSamples + numSamples);
}

float PitchDetector::frequencyToMidi(float frequencyHz)
{
    return 69.0f + 12.0f * std::log2(frequencyHz / 440.0f);
}

PitchDetector::Result PitchDetector::analyze() const
{
    Result result;

    if (filledSamples < analysisWindowSize)
        return result;

    const float* x = window.data();
    const int n = analysisWindowSize;

    std::vector<float> nsdf(static_cast<size_t>(maxLag + 1), 0.0f);
    nsdf[0] = 1.0f;

    for (int tau = 1; tau <= maxLag; ++tau)
    {
        double acf = 0.0;
        double m = 0.0;
        const int count = n - tau;
        for (int i = 0; i < count; ++i)
        {
            const double a = x[i];
            const double b = x[i + tau];
            acf += a * b;
            m += a * a + b * b;
        }
        nsdf[static_cast<size_t>(tau)] = (m > 0.0) ? static_cast<float>(2.0 * acf / m) : 0.0f;
    }

    // 첫 양(+)의 제로크로싱 이후 등장하는 극대점들만 후보로 삼는다(잡음 억제).
    struct Peak { int lag; float value; };
    std::vector<Peak> peaks;

    int tau = 1;
    while (tau < maxLag && nsdf[static_cast<size_t>(tau)] > 0.0f)
        ++tau;
    while (tau < maxLag && nsdf[static_cast<size_t>(tau)] <= 0.0f)
        ++tau;

    for (; tau < maxLag; ++tau)
    {
        if (nsdf[static_cast<size_t>(tau)] > nsdf[static_cast<size_t>(tau - 1)]
            && nsdf[static_cast<size_t>(tau)] >= nsdf[static_cast<size_t>(tau + 1)])
        {
            peaks.push_back({ tau, nsdf[static_cast<size_t>(tau)] });
        }
    }

    if (peaks.empty() || minLag > maxLag)
        return result;

    float highest = 0.0f;
    for (const auto& p : peaks)
        highest = std::max(highest, p.value);

    if (highest <= 0.0f)
        return result;

    const Peak* chosen = nullptr;
    for (const auto& p : peaks)
    {
        if (p.lag < minLag)
            continue;
        if (p.value >= peakPickingThreshold * highest)
        {
            chosen = &p;
            break;
        }
    }

    if (chosen == nullptr)
        return result;

    // 초입/말단 보호 후 포물선 보간으로 서브샘플 정확도 확보.
    int lag = chosen->lag;
    float interpolatedLag = static_cast<float>(lag);
    if (lag > 0 && lag < maxLag)
    {
        const float y0 = nsdf[static_cast<size_t>(lag - 1)];
        const float y1 = nsdf[static_cast<size_t>(lag)];
        const float y2 = nsdf[static_cast<size_t>(lag + 1)];
        const float denom = (y0 - 2.0f * y1 + y2);
        if (std::abs(denom) > 1.0e-9f)
            interpolatedLag = static_cast<float>(lag) + 0.5f * (y0 - y2) / denom;
    }

    if (interpolatedLag <= 0.0f)
        return result;

    const float frequencyHz = static_cast<float>(sampleRate) / interpolatedLag;
    const float confidence = std::clamp(chosen->value, 0.0f, 1.0f);

    if (confidence < minClarityForValidPitch)
        return result;

    result.valid = true;
    result.frequencyHz = frequencyHz;
    result.midiFloat = frequencyToMidi(frequencyHz);
    result.confidence = confidence;
    return result;
}

PitchDetector::Result PitchDetector::processBlock(const float* samples, int numSamples)
{
    pushSamples(samples, numSamples);
    return analyze();
}
