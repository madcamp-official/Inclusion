#pragma once

#include <algorithm>
#include <cmath>

namespace mode1
{

class GuitarOnsetTracker
{
public:
    void prepare(double sampleRate) noexcept;
    void reset() noexcept;
    void setArpeggioMode(bool enabled) noexcept
    {
        arpeggioMode = enabled;
        refractorySeconds = enabled ? 0.225 : 0.280;
    }

    bool processBlock(const float* guitarInput, int numSamples) noexcept;

    [[nodiscard]] float getCurrentRms() const noexcept { return currentRms; }
    [[nodiscard]] bool isActive() const noexcept { return currentRms >= activityThreshold; }
    [[nodiscard]] float getLastOnsetStrength() const noexcept
    {
        return lastOnsetStrength;
    }
    // Loudness of the most recent onset relative to the player's own recent
    // strums (self-calibrating, so it works regardless of input gain
    // staging). 0.5 is a typical strum, 0 is soft, 1 is hard.
    [[nodiscard]] float getLastOnsetAccent() const noexcept
    {
        return lastOnsetAccent;
    }

private:
    double sampleRate = 48'000.0;
    double samplesSinceOnset = 0.0;
    float energyBaseline = 1.0e-4f;
    float currentRms = 0.0f;
    float previousRms = 0.0f;
    float lastOnsetStrength = 1.0f;
    float onsetRmsBaseline = 0.02f;
    float lastOnsetAccent = 0.5f;
    double refractorySeconds = 0.280;
    bool arpeggioMode = false;

    static constexpr float onsetRatio = 1.6f;
    static constexpr float arpeggioOnsetRatio = 1.25f;
    static constexpr double attackLogRisePerSecond =
        18.652276197318684; // log(1.22) / (512 / 48000)
    static constexpr double arpeggioAttackLogRisePerSecond =
        7.215788606407547; // log(1.08) / (512 / 48000)
    static constexpr double risingBaselineTimeConstantSeconds = 1.0613;
    static constexpr double fallingBaselineTimeConstantSeconds = 0.1279;
    static constexpr float minimumOnsetRms = 0.01f;
    static constexpr float activityThreshold = 0.003f;
};

} // namespace mode1
