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

    bool processBlock(const float* guitarInput, int numSamples) noexcept;

    [[nodiscard]] float getCurrentRms() const noexcept { return currentRms; }
    [[nodiscard]] bool isActive() const noexcept { return currentRms >= activityThreshold; }

private:
    double sampleRate = 48'000.0;
    double samplesSinceOnset = 0.0;
    float energyBaseline = 1.0e-4f;
    float currentRms = 0.0f;

    static constexpr float onsetRatio = 1.6f;
    static constexpr float minimumOnsetRms = 0.01f;
    static constexpr float activityThreshold = 0.003f;
    static constexpr double refractorySeconds = 0.050;
};

} // namespace mode1
