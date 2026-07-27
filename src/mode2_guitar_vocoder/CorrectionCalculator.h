#pragma once

#include <optional>

// 사양서 PART 1 4절 compute_correction 그대로.
class CorrectionCalculator
{
public:
    static float computeCorrection(std::optional<float> vocalMidi, std::optional<float> targetMidi, float strength);
};
