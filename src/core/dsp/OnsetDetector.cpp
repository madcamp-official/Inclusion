#include "OnsetDetector.h"
#include "params/Mode2Params.h"

#include <cmath>
#include <limits>

void OnsetDetector::prepare(double sampleRateIn)
{
    sampleRate = sampleRateIn;
    minIntervalSamples = static_cast<long long>(mode2::params::onsetMinIntervalSeconds * sampleRate);
    reset();
}

void OnsetDetector::reset()
{
    runningEnergy = 0.0f;
    samplesSinceLastOnset = std::numeric_limits<long long>::max() / 2;
}

float OnsetDetector::computeRms(const float* samples, int numSamples)
{
    if (numSamples <= 0)
        return 0.0f;

    double sumSquares = 0.0;
    for (int i = 0; i < numSamples; ++i)
        sumSquares += static_cast<double>(samples[i]) * static_cast<double>(samples[i]);

    return static_cast<float>(std::sqrt(sumSquares / static_cast<double>(numSamples)));
}

bool OnsetDetector::processBlock(const float* samples, int numSamples)
{
    const float rms = computeRms(samples, numSamples);

    bool isOnset = false;
    if (rms > mode2::params::onsetEnergyFloor
        && rms > runningEnergy * mode2::params::onsetAdaptiveThresholdMultiplier
        && samplesSinceLastOnset >= minIntervalSamples)
    {
        isOnset = true;
        samplesSinceLastOnset = 0;
    }
    else
    {
        samplesSinceLastOnset += numSamples;
    }

    runningEnergy += baselineEmaCoeff * (rms - runningEnergy);

    return isOnset;
}
