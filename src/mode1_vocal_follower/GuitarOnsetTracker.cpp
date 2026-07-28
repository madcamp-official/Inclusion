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
    previousRms = 0.0f;
    lastOnsetStrength = 1.0f;
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
    const bool hasFreshAttack =
        previousRms <= 1.0e-5f
        || currentRms >= previousRms * attackRiseRatio;
    const bool onset =
        refractoryFinished
        && hasFreshAttack
        && currentRms >= minimumOnsetRms
        && currentRms >= energyBaseline * onsetRatio;

    // Use a slower baseline while the input rises so the attack is not
    // immediately absorbed into the average.
    const float coefficient = currentRms > energyBaseline ? 0.01f : 0.08f;
    energyBaseline += coefficient * (currentRms - energyBaseline);
    energyBaseline = std::max(energyBaseline, 1.0e-4f);

    if (onset)
    {
        lastOnsetStrength = currentRms
            / std::max(previousRms, 1.0e-5f);
        samplesSinceOnset = 0.0;
    }
    previousRms = currentRms;
    return onset;
}

} // namespace mode1
