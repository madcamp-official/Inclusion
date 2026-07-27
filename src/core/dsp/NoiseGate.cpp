#include "NoiseGate.h"
#include "params/Mode2Params.h"

#include <algorithm>
#include <cmath>

namespace
{
    // 목표값까지 지정 시간(초)에 도달하는 1차 스무딩 계수. 시간이 0이면 즉시 이동.
    float timeToCoeff(float seconds, double sampleRate)
    {
        if (seconds <= 0.0f)
            return 1.0f;
        return 1.0f - std::exp(-1.0f / (seconds * static_cast<float>(sampleRate)));
    }
}

void NoiseGate::prepare(double sampleRateIn, int maxBlockSize)
{
    sampleRate = sampleRateIn;
    attackCoeff = timeToCoeff(mode2::params::noiseGateAttackSeconds, sampleRate);
    releaseCoeff = timeToCoeff(mode2::params::noiseGateReleaseSeconds, sampleRate);

    const int blockSize = std::max(1, maxBlockSize);
    holdBlocksTotal = std::max(1, static_cast<int>(mode2::params::noiseGateHoldSeconds * sampleRate / blockSize));

    openThreshold = mode2::params::vocalNoiseGateThreshold;
    reset();
}

void NoiseGate::reset()
{
    open = false;
    currentGain = 0.0f;
    holdBlocksRemaining = 0;
}

void NoiseGate::processBlock(float* samples, int numSamples, float keyLevelRms)
{
    if (! enabled)
    {
        currentGain = 1.0f;
        open = true;
        return;
    }

    const float closeThreshold = openThreshold * mode2::params::noiseGateHysteresisRatio;

    if (keyLevelRms >= openThreshold)
    {
        open = true;
        holdBlocksRemaining = holdBlocksTotal;
    }
    else if (open && keyLevelRms < closeThreshold)
    {
        // 바로 닫지 않고 유지 시간을 소진한 뒤 닫는다.
        if (--holdBlocksRemaining <= 0)
            open = false;
    }

    const float targetGain = open ? 1.0f : 0.0f;
    const float coeff = (targetGain > currentGain) ? attackCoeff : releaseCoeff;

    for (int i = 0; i < numSamples; ++i)
    {
        currentGain += coeff * (targetGain - currentGain);
        samples[i] *= currentGain;
    }
}
