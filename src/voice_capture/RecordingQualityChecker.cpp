#include "RecordingQualityChecker.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace voice_capture
{
RecordingQuality RecordingQualityChecker::analyse(
    const juce::AudioBuffer<float>& audio,
    double sampleRate,
    float calibratedNoiseFloorDb)
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

    const auto frameSize =
        juce::jmax(1, static_cast<int>(sampleRate * 0.02));
    std::vector<float> frameLevels;
    frameLevels.reserve(
        static_cast<size_t>((sampleCount + frameSize - 1) / frameSize));
    for (int start = 0; start < sampleCount; start += frameSize)
    {
        const auto count = juce::jmin(frameSize, sampleCount - start);
        double frameSquares = 0.0;
        for (int i = 0; i < count; ++i)
            frameSquares +=
                static_cast<double>(samples[start + i]) * samples[start + i];
        const auto frameRms =
            static_cast<float>(std::sqrt(frameSquares / count));
        frameLevels.push_back(
            juce::Decibels::gainToDecibels(frameRms, -100.0f));
    }

    auto sortedLevels = frameLevels;
    std::sort(sortedLevels.begin(), sortedLevels.end());
    const auto percentile = [&sortedLevels](double fraction)
    {
        if (sortedLevels.empty())
            return -100.0f;
        const auto index = static_cast<size_t>(
            fraction * static_cast<double>(sortedLevels.size() - 1));
        return sortedLevels[index];
    };

    // The UI asks the user to speak as capture starts, so the first second
    // cannot be treated as room tone. Use quiet frames, or the dedicated
    // preflight measurement when available.
    const auto estimatedNoiseFloor =
        juce::jlimit(-80.0f, -25.0f, percentile(0.10));
    result.noiseFloorDb = calibratedNoiseFloorDb > -95.0f
        ? calibratedNoiseFloorDb
        : estimatedNoiseFloor;
    const float activeThresholdDb = juce::jlimit(
        -48.0f, -30.0f, result.noiseFloorDb + 10.0f);

    int activeFrames = 0;
    int activeSamples = 0;
    double activeSquares = 0.0;
    for (int start = 0, frame = 0;
         start < sampleCount;
         start += frameSize, ++frame)
    {
        const auto count = juce::jmin(frameSize, sampleCount - start);
        if (frameLevels[static_cast<size_t>(frame)] <= activeThresholdDb)
            continue;
        ++activeFrames;
        activeSamples += count;
        for (int i = 0; i < count; ++i)
            activeSquares +=
                static_cast<double>(samples[start + i]) * samples[start + i];
    }

    const auto totalFrames = static_cast<int>(frameLevels.size());
    result.activeSpeechRatio = totalFrames > 0
        ? static_cast<float>(activeFrames) / totalFrames
        : 0.0f;
    result.activeSpeechSeconds =
        static_cast<double>(activeSamples) / sampleRate;
    const auto activeRms = activeSamples > 0
        ? static_cast<float>(std::sqrt(activeSquares / activeSamples))
        : 0.0f;
    const auto activeRmsDb =
        juce::Decibels::gainToDecibels(activeRms, -100.0f);
    result.snrDb =
        juce::jmax(0.0f, activeRmsDb - result.noiseFloorDb);

    juce::StringArray problems;
    juce::StringArray warnings;
    if (result.durationSeconds < 2.0)
        problems.add(L"2초 이상 녹음해 주세요");
    if (result.activeSpeechSeconds < 1.0
        || result.activeSpeechRatio < 0.08f)
        problems.add(L"목소리가 충분히 들어오지 않았습니다");
    if (result.peakDb < -30.0f || activeRmsDb < -42.0f)
        problems.add(L"마이크 입력이 너무 작습니다");
    if (result.clippingRatio > 0.01f)
        problems.add(L"입력 음량이 너무 커서 소리가 심하게 깨집니다");

    if (result.peakDb >= -1.0f && result.clippingRatio <= 0.01f)
        warnings.add(L"일부 순간의 음량이 큽니다");
    if (result.snrDb < 12.0f)
        warnings.add(L"주변 소음이 다소 큽니다");
    if (result.activeSpeechRatio < 0.18f
        && result.activeSpeechSeconds >= 1.0)
        warnings.add(L"긴 무음 구간이 포함됐습니다");

    result.passed = problems.isEmpty();
    result.hasWarning = result.passed && !warnings.isEmpty();
    result.summary = !result.passed
        ? L"다시 녹음해 주세요: " + problems.joinIntoString(", ")
        : result.hasWarning
            ? L"사용 가능 · 참고: " + warnings.joinIntoString(", ")
            : L"품질 검사 통과";
    return result;
}
} // namespace voice_capture
