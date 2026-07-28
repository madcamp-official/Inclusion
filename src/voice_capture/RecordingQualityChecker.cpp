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
        result.summary = L"녹음된 오디오가 없습니다.";
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
    const auto roomToneCount =
        juce::jmin(sampleCount, static_cast<int>(sampleRate));
    double roomToneSquares = 0.0;
    for (int i = 0; i < roomToneCount; ++i)
        roomToneSquares += static_cast<double>(samples[i]) * samples[i];
    const auto noiseRms = static_cast<float>(
        std::sqrt(roomToneSquares / juce::jmax(1, roomToneCount)));
    result.noiseFloorDb = juce::Decibels::gainToDecibels(noiseRms, -100.0f);
    result.snrDb = juce::jmax(0.0f, result.rmsDb - result.noiseFloorDb);
    const float activeThresholdDb =
        juce::jmax(-45.0f, result.noiseFloorDb + 12.0f);

    int activeFrames = 0;
    int totalFrames = 0;

    for (int start = 0; start < sampleCount; start += frameSize)
    {
        const auto count = juce::jmin(frameSize, sampleCount - start);
        double frameSquares = 0.0;
        for (int i = 0; i < count; ++i)
            frameSquares += static_cast<double>(samples[start + i]) * samples[start + i];

        const auto frameRms = static_cast<float>(std::sqrt(frameSquares / count));
        activeFrames +=
            juce::Decibels::gainToDecibels(frameRms, -100.0f) > activeThresholdDb
                ? 1
                : 0;
        ++totalFrames;
    }

    result.activeSpeechRatio = totalFrames > 0
        ? static_cast<float>(activeFrames) / totalFrames
        : 0.0f;

    juce::StringArray problems;
    if (result.durationSeconds < 3.0)
        problems.add(L"3초 이상 녹음해 주세요");
    if (result.peakDb < -18.0f || result.rmsDb < -38.0f)
        problems.add(L"마이크를 조금 가까이 하거나 입력 음량을 높여주세요");
    if (result.peakDb >= -1.0f || result.clippingRatio > 0.0005f)
        problems.add(L"입력 음량이 너무 커서 소리가 깨졌습니다");
    if (result.snrDb < 15.0f)
        problems.add(L"주변 잡음이 큽니다");
    if (result.activeSpeechRatio < 0.5f)
        problems.add(L"무음 구간이 너무 많습니다");

    result.passed = problems.isEmpty();
    result.summary = result.passed
        ? L"품질 검사 통과 — zero-shot 참조 음성으로 사용할 수 있습니다."
        : L"재녹음 권장: " + problems.joinIntoString(", ");
    return result;
}
}
