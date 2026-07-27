#include "RecordingQualityChecker.h"

#include <cmath>

namespace voice_capture
{
RecordingQuality RecordingQualityChecker::analyse(const juce::AudioBuffer<float>& audio,
                                                   double sampleRate)
{
    RecordingQuality result;
    const auto sampleCount = audio.getNumSamples();

    if (sampleRate <= 0.0 || sampleCount == 0)
    {
        result.summary = "녹음된 오디오가 없습니다.";
        return result;
    }

    result.durationSeconds = static_cast<double>(sampleCount) / sampleRate;

    const auto* samples = audio.getReadPointer(0);
    double sumSquares = 0.0;
    float peak = 0.0f;
    int clippedSamples = 0;

    for (int i = 0; i < sampleCount; ++i)
    {
        const auto magnitude = std::abs(samples[i]);
        sumSquares += static_cast<double>(samples[i]) * samples[i];
        peak = juce::jmax(peak, magnitude);
        clippedSamples += magnitude >= 0.99f ? 1 : 0;
    }

    const auto rms = static_cast<float>(std::sqrt(sumSquares / sampleCount));
    result.rmsDb = juce::Decibels::gainToDecibels(rms, -100.0f);
    result.peakDb = juce::Decibels::gainToDecibels(peak, -100.0f);
    result.clippingRatio = static_cast<float>(clippedSamples) / sampleCount;

    const auto frameSize = juce::jmax(1, static_cast<int>(sampleRate * 0.02));
    int activeFrames = 0;
    int totalFrames = 0;

    for (int start = 0; start < sampleCount; start += frameSize)
    {
        const auto count = juce::jmin(frameSize, sampleCount - start);
        double frameSquares = 0.0;
        for (int i = 0; i < count; ++i)
            frameSquares += static_cast<double>(samples[start + i]) * samples[start + i];

        const auto frameRms = static_cast<float>(std::sqrt(frameSquares / count));
        activeFrames += juce::Decibels::gainToDecibels(frameRms, -100.0f) > -45.0f ? 1 : 0;
        ++totalFrames;
    }

    result.activeSpeechRatio = totalFrames > 0
        ? static_cast<float>(activeFrames) / totalFrames
        : 0.0f;

    juce::StringArray problems;
    if (result.durationSeconds < 8.0)
        problems.add("8초 이상 녹음해 주세요");
    if (result.rmsDb < -38.0f)
        problems.add("목소리가 너무 작습니다");
    if (result.clippingRatio > 0.002f)
        problems.add("입력 음량이 너무 커서 소리가 깨졌습니다");
    if (result.activeSpeechRatio < 0.35f)
        problems.add("무음 구간이 너무 많습니다");

    result.passed = problems.isEmpty();
    result.summary = result.passed
        ? "품질 검사 통과 — zero-shot 참조 음성으로 사용할 수 있습니다."
        : "재녹음 권장: " + problems.joinIntoString(", ");
    return result;
}
}
