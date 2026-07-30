#include "InputLevelCalibrator.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <cmath>

namespace voice_capture
{

void InputLevelCalibrator::prepare(double newSampleRate) noexcept
{
    sampleRate = std::max(1.0, newSampleRate);
    cancel();
}

void InputLevelCalibrator::start() noexcept
{
    samplesInPhase.store(0);
    roomToneSquares.store(0.0);
    roomToneSamples.store(0);
    voiceSquares.store(0.0);
    voiceSamples.store(0);
    voicePeak.store(0.0f);
    inputPeak.store(0.0f);
    phase.store(Phase::roomTone);
}

void InputLevelCalibrator::cancel() noexcept
{
    phase.store(Phase::idle);
    inputPeak.store(0.0f);
}

bool InputLevelCalibrator::isRunning() const noexcept
{
    const auto current = phase.load();
    return current == Phase::roomTone || current == Phase::voice;
}

void InputLevelCalibrator::processBlock(
    const float* input,
    int numSamples) noexcept
{
    if (input == nullptr || numSamples <= 0 || !isRunning())
        return;

    double squares = 0.0;
    float peak = 0.0f;
    for (int sample = 0; sample < numSamples; ++sample)
    {
        const auto value = input[sample];
        squares += static_cast<double>(value) * value;
        peak = std::max(peak, std::abs(value));
    }
    inputPeak.store(std::max(peak, inputPeak.load() * 0.82f));

    const auto current = phase.load();
    if (current == Phase::roomTone)
    {
        roomToneSquares.store(roomToneSquares.load() + squares);
        roomToneSamples.fetch_add(numSamples);
        const auto elapsed = samplesInPhase.fetch_add(numSamples) + numSamples;
        if (elapsed >= static_cast<int64_t>(sampleRate * roomToneSeconds))
        {
            samplesInPhase.store(0);
            phase.store(Phase::voice);
        }
        return;
    }

    voiceSquares.store(voiceSquares.load() + squares);
    voiceSamples.fetch_add(numSamples);
    voicePeak.store(std::max(voicePeak.load(), peak));
    const auto elapsed = samplesInPhase.fetch_add(numSamples) + numSamples;
    if (elapsed < static_cast<int64_t>(sampleRate * voiceSeconds))
        return;

    const auto roomCount = std::max<int64_t>(1, roomToneSamples.load());
    const auto speechCount = std::max<int64_t>(1, voiceSamples.load());
    const auto roomRms =
        static_cast<float>(std::sqrt(roomToneSquares.load() / roomCount));
    const auto speechRms =
        static_cast<float>(std::sqrt(voiceSquares.load() / speechCount));
    const bool voiceDetected =
        speechRms >= 0.003f
        && speechRms >= std::max(0.006f, roomRms * 2.0f);
    phase.store(voiceDetected ? Phase::complete : Phase::failed);
}

InputLevelCalibrator::Result InputLevelCalibrator::getResult() const
{
    Result result;
    result.phase = phase.load();

    const auto roomCount = std::max<int64_t>(1, roomToneSamples.load());
    const auto speechCount = std::max<int64_t>(1, voiceSamples.load());
    const auto roomRms =
        static_cast<float>(std::sqrt(roomToneSquares.load() / roomCount));
    const auto speechRms =
        static_cast<float>(std::sqrt(voiceSquares.load() / speechCount));
    const auto peak = voicePeak.load();
    result.roomToneDb =
        juce::Decibels::gainToDecibels(roomRms, -100.0f);
    result.voiceRmsDb =
        juce::Decibels::gainToDecibels(speechRms, -100.0f);
    result.voicePeakDb =
        juce::Decibels::gainToDecibels(peak, -100.0f);

    if (result.phase == Phase::roomTone)
    {
        result.progress = juce::jlimit(
            0.0,
            1.0,
            static_cast<double>(samplesInPhase.load())
                / (sampleRate * roomToneSeconds));
        result.message = L"1단계 · 말하지 말고 주변 소음을 측정합니다.";
    }
    else if (result.phase == Phase::voice)
    {
        result.progress = juce::jlimit(
            0.0,
            1.0,
            static_cast<double>(samplesInPhase.load())
                / (sampleRate * voiceSeconds));
        result.message =
            L"2단계 · 평소 음량으로 “안녕하세요, 제 목소리를 확인합니다”라고 말해 주세요.";
    }
    else if (result.phase == Phase::failed)
    {
        result.progress = 1.0;
        result.message =
            L"목소리를 충분히 감지하지 못했습니다. 다시 측정해 주세요.";
    }

    // Protect roughly -6 dBFS of peak headroom and -20 dBFS average.
    // The requested behaviour is attenuation only, so quiet microphones
    // are never digitally boosted along with their noise floor.
    const float peakGain = peak > 0.0f
        ? juce::Decibels::decibelsToGain(-6.0f) / peak
        : 1.0f;
    const float rmsGain = speechRms > 0.0f
        ? juce::Decibels::decibelsToGain(-20.0f) / speechRms
        : 1.0f;
    result.recommendedGain = juce::jlimit(
        0.15f, 1.0f, std::min(peakGain, rmsGain));
    if (result.phase == Phase::complete)
    {
        result.progress = 1.0;
        const auto attenuationDb = juce::Decibels::gainToDecibels(
            result.recommendedGain, -100.0f);
        result.message = result.recommendedGain < 0.98f
            ? L"입력이 커서 녹음 음량을 "
                + juce::String(attenuationDb, 1) + L" dB 자동 조절했습니다."
            : L"입력 음량이 적절합니다. 현재 음량을 유지합니다.";
    }
    return result;
}

} // namespace voice_capture
