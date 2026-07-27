#include "GuitarOnsetTracker.h"

namespace mode1
{

void GuitarOnsetTracker::prepare(double newSampleRate) noexcept
{
    sampleRate = std::max(1.0, newSampleRate);
    reset();
}

void GuitarOnsetTracker::reset() noexcept
{
    samplesSinceOnset = sampleRate;
    energyBaseline = 1.0e-4f;
    currentRms = 0.0f;
}

bool GuitarOnsetTracker::processBlock(
    const float* guitarInput,
    int numSamples) noexcept
{
    if (guitarInput == nullptr || numSamples <= 0)
        return false;

    double sumSquares = 0.0;
    for (int sample = 0; sample < numSamples; ++sample)
        sumSquares += static_cast<double>(guitarInput[sample]) * guitarInput[sample];

    currentRms = static_cast<float>(std::sqrt(sumSquares / numSamples));
    samplesSinceOnset += numSamples;

    const bool refractoryFinished =
        samplesSinceOnset >= refractorySeconds * sampleRate;
    const bool onset =
        refractoryFinished
        && currentRms >= minimumOnsetRms
        && currentRms >= energyBaseline * onsetRatio;

    // Use a slower baseline while the input rises so the attack is not
    // immediately absorbed into the average.
    const float coefficient = currentRms > energyBaseline ? 0.01f : 0.08f;
    energyBaseline += coefficient * (currentRms - energyBaseline);
    energyBaseline = std::max(energyBaseline, 1.0e-4f);

    if (onset)
        samplesSinceOnset = 0.0;
    return onset;
}

} // namespace mode1
