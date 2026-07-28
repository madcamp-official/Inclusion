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
    onsetRmsBaseline = 0.02f;
    lastOnsetAccent = 0.5f;
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

    const double blockSeconds = numSamples / sampleRate;
    const double attackLogRise =
        arpeggioMode
            ? arpeggioAttackLogRisePerSecond
            : attackLogRisePerSecond;
    const float attackRatioForBlock = static_cast<float>(
        std::exp(attackLogRise * blockSeconds));
    const bool refractoryFinished =
        samplesSinceOnset >= refractorySeconds * sampleRate;
    const bool hasFreshAttack =
        previousRms <= 1.0e-5f
        || currentRms >= previousRms * attackRatioForBlock;
    const bool onset =
        refractoryFinished
        && hasFreshAttack
        && currentRms >= minimumOnsetRms
        && currentRms >= energyBaseline
            * (arpeggioMode ? arpeggioOnsetRatio : onsetRatio);

    // Time-based smoothing keeps onset sensitivity stable when the audio
    // device changes its callback buffer size.
    const double timeConstant = currentRms > energyBaseline
        ? risingBaselineTimeConstantSeconds
        : fallingBaselineTimeConstantSeconds;
    const float coefficient = static_cast<float>(
        1.0 - std::exp(-blockSeconds / timeConstant));
    energyBaseline += coefficient * (currentRms - energyBaseline);
    energyBaseline = std::max(energyBaseline, 1.0e-4f);

    if (onset)
    {
        lastOnsetStrength =
            currentRms / std::max(previousRms, 1.0e-5f);

        // Normalize this strum's loudness against a slow-moving average of
        // past onsets rather than a fixed threshold, so the accent tracks
        // the player's own dynamics instead of absolute input level.
        const float ratio = currentRms / std::max(onsetRmsBaseline, 1.0e-4f);
        lastOnsetAccent = std::clamp((ratio - 0.6f) / 1.0f, 0.0f, 1.0f);
        constexpr float baselineSmoothing = 0.12f;
        onsetRmsBaseline += baselineSmoothing * (currentRms - onsetRmsBaseline);

        samplesSinceOnset = 0.0;
    }
    previousRms = currentRms;
    return onset;
}

} // namespace mode1
