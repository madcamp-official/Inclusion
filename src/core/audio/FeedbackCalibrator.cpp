#include "FeedbackCalibrator.h"

#include <algorithm>
#include <cmath>

void FeedbackCalibrator::prepare(double sampleRateIn)
{
    sampleRate = sampleRateIn;
    reset();
}

void FeedbackCalibrator::reset()
{
    calibrating.store(false);
    currentTestGain = 0.0f;
    previousInputRms = 0.0f;
    consecutiveGrowthBlocks = 0;
    masterGainCeiling.store(1.0f);
}

void FeedbackCalibrator::startCalibration()
{
    currentTestGain = 0.0f;
    previousInputRms = 0.0f;
    consecutiveGrowthBlocks = 0;
    calibrating.store(true);
}

float FeedbackCalibrator::computeRms(const float* samples, int numSamples)
{
    if (numSamples <= 0)
        return 0.0f;

    double sumSquares = 0.0;
    for (int i = 0; i < numSamples; ++i)
        sumSquares += static_cast<double>(samples[i]) * static_cast<double>(samples[i]);

    return static_cast<float>(std::sqrt(sumSquares / static_cast<double>(numSamples)));
}

void FeedbackCalibrator::processBlock(const float* inputBlock, int numSamples, float& outputTestGain)
{
    if (!calibrating.load())
    {
        outputTestGain = 1.0f;
        return;
    }

    const float rms = computeRms(inputBlock, numSamples);

    // 하울링은 "계속 커지는" 현상이다. 단발성 상승은 연주·발성으로도 생기므로 무시한다.
    // 또 게인이 충분히 올라가기 전(armTestGainThreshold 이하)에는 판정하지 않는다.
    if (previousInputRms > 1.0e-6f && rms > previousInputRms * feedbackGrowthRatioThreshold)
        ++consecutiveGrowthBlocks;
    else
        consecutiveGrowthBlocks = 0;

    if (currentTestGain >= armTestGainThreshold && consecutiveGrowthBlocks >= requiredConsecutiveGrowthBlocks)
    {
        masterGainCeiling.store(std::max(minimumCeiling, currentTestGain * safetyMarginRatio));
        calibrating.store(false);
        outputTestGain = masterGainCeiling.load();
        return;
    }

    previousInputRms = rms;
    const float increment = gainRampPerSecond * static_cast<float>(numSamples) / static_cast<float>(sampleRate);
    currentTestGain = std::min(1.0f, currentTestGain + increment);
    outputTestGain = currentTestGain;

    if (currentTestGain >= 1.0f)
    {
        masterGainCeiling.store(1.0f);
        calibrating.store(false);
    }
}
