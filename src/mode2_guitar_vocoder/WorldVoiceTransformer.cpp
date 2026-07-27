#include "WorldVoiceTransformer.h"

#include <world/cheaptrick.h>
#include <world/d4c.h>
#include <world/harvest.h>
#include <world/synthesis.h>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace
{
    constexpr double framePeriodMs = 5.0;

    float localRms(const std::vector<double>& signal, int centre, int radius)
    {
        const int begin = std::max(0, centre - radius);
        const int end = std::min(static_cast<int>(signal.size()), centre + radius + 1);
        if (end <= begin)
            return 0.0f;

        double sum = 0.0;
        for (int i = begin; i < end; ++i)
            sum += signal[static_cast<size_t>(i)] * signal[static_cast<size_t>(i)];
        return static_cast<float>(std::sqrt(sum / static_cast<double>(end - begin)));
    }
}

WorldVoiceTransformer::Result WorldVoiceTransformer::transform(
    const float* vocal,
    int numSamples,
    double sampleRate,
    const std::vector<float>& shiftSemitones,
    const std::vector<float>& absoluteTargetF0Hz,
    float inputGain,
    float outputGain,
    bool gateEnabled,
    float gateThreshold)
{
    Result result;
    if (vocal == nullptr || numSamples <= 0 || sampleRate < 8000.0)
        return result;

    const int fs = static_cast<int>(std::lround(sampleRate));
    std::vector<double> input(static_cast<size_t>(numSamples));
    for (int i = 0; i < numSamples; ++i)
        input[static_cast<size_t>(i)] = static_cast<double>(vocal[i] * inputGain);

    HarvestOption harvestOption;
    InitializeHarvestOption(&harvestOption);
    harvestOption.f0_floor = 80.0;
    harvestOption.f0_ceil = 500.0;
    harvestOption.frame_period = framePeriodMs;

    const int frameCount = GetSamplesForHarvest(fs, numSamples, framePeriodMs);
    std::vector<double> timeAxis(static_cast<size_t>(frameCount));
    std::vector<double> sourceF0(static_cast<size_t>(frameCount));
    Harvest(input.data(), numSamples, fs, &harvestOption, timeAxis.data(), sourceF0.data());

    CheapTrickOption spectralOption;
    InitializeCheapTrickOption(fs, &spectralOption);
    spectralOption.f0_floor = harvestOption.f0_floor;
    spectralOption.fft_size = GetFFTSizeForCheapTrick(fs, &spectralOption);
    const int bins = spectralOption.fft_size / 2 + 1;

    std::vector<std::vector<double>> spectralEnvelope(
        static_cast<size_t>(frameCount), std::vector<double>(static_cast<size_t>(bins)));
    std::vector<std::vector<double>> aperiodicity(
        static_cast<size_t>(frameCount), std::vector<double>(static_cast<size_t>(bins)));
    std::vector<double*> spectralPointers(static_cast<size_t>(frameCount));
    std::vector<double*> aperiodicityPointers(static_cast<size_t>(frameCount));
    for (int frame = 0; frame < frameCount; ++frame)
    {
        spectralPointers[static_cast<size_t>(frame)] =
            spectralEnvelope[static_cast<size_t>(frame)].data();
        aperiodicityPointers[static_cast<size_t>(frame)] =
            aperiodicity[static_cast<size_t>(frame)].data();
    }

    CheapTrick(input.data(), numSamples, fs, timeAxis.data(), sourceF0.data(),
               frameCount, &spectralOption, spectralPointers.data());

    D4COption d4cOption;
    InitializeD4COption(&d4cOption);
    D4C(input.data(), numSamples, fs, timeAxis.data(), sourceF0.data(),
        frameCount, spectralOption.fft_size, &d4cOption, aperiodicityPointers.data());

    std::vector<double> targetF0 = sourceF0;
    std::vector<float> frameVoicing(static_cast<size_t>(frameCount), 0.0f);
    int voicedFrames = 0;
    for (int frame = 0; frame < frameCount; ++frame)
    {
        if (sourceF0[static_cast<size_t>(frame)] <= 0.0)
            continue;

        ++voicedFrames;
        frameVoicing[static_cast<size_t>(frame)] = 1.0f;
        const int sample = std::clamp(
            static_cast<int>(std::lround(timeAxis[static_cast<size_t>(frame)] * sampleRate)),
            0, numSamples - 1);
        const float semitones = sample < static_cast<int>(shiftSemitones.size())
            ? shiftSemitones[static_cast<size_t>(sample)]
            : 0.0f;
        const float absoluteTarget =
            sample < static_cast<int>(absoluteTargetF0Hz.size())
                ? absoluteTargetF0Hz[static_cast<size_t>(sample)]
                : 0.0f;
        targetF0[static_cast<size_t>(frame)] =
            absoluteTarget > 0.0f
                ? std::clamp(static_cast<double>(absoluteTarget), 50.0, 1000.0)
                : std::clamp(sourceF0[static_cast<size_t>(frame)]
                                 * std::pow(2.0, static_cast<double>(semitones) / 12.0),
                             50.0, 1000.0);
    }

    std::vector<double> synthesized(static_cast<size_t>(numSamples), 0.0);
    Synthesis(targetF0.data(), frameCount, spectralPointers.data(),
              aperiodicityPointers.data(), spectralOption.fft_size,
              framePeriodMs, fs, numSamples, synthesized.data());

    // WORLD는 유성 모음의 spectral envelope 보존에 집중하고, 자음·호흡은 원음을 통과시킨다.
    // 5ms 프레임의 V/UV 결정을 샘플 단위로 보간한 뒤 8ms로 완만하게 만들어 경계 클릭을 막는다.
    const double framesPerSample = 1000.0 / (framePeriodMs * sampleRate);
    const float smoothing = 1.0f - std::exp(-1.0f / static_cast<float>(0.008 * sampleRate));
    const int rmsRadius = std::max(1, static_cast<int>(0.01 * sampleRate));
    float voicedMix = 0.0f;
    result.audio.resize(static_cast<size_t>(numSamples), 0.0f);
    for (int i = 0; i < numSamples; ++i)
    {
        const double framePosition = static_cast<double>(i) * framesPerSample;
        const int frame0 = std::clamp(static_cast<int>(framePosition), 0, frameCount - 1);
        const int frame1 = std::min(frame0 + 1, frameCount - 1);
        const float fraction = static_cast<float>(framePosition - frame0);
        const float targetMix = frameVoicing[static_cast<size_t>(frame0)] * (1.0f - fraction)
                              + frameVoicing[static_cast<size_t>(frame1)] * fraction;
        voicedMix += smoothing * (targetMix - voicedMix);

        const float original = static_cast<float>(input[static_cast<size_t>(i)]);
        const float world = static_cast<float>(synthesized[static_cast<size_t>(i)]);
        float sample = world * voicedMix + original * (1.0f - voicedMix);

        if (gateEnabled && localRms(input, i, rmsRadius) < gateThreshold)
            sample = 0.0f;

        result.audio[static_cast<size_t>(i)] = sample * outputGain;
    }

    // 하드 클리핑으로 자음이 거칠어지지 않도록 전체 파일에 하나의 안전 게인을 적용한다.
    float peak = 0.0f;
    for (float sample : result.audio)
        peak = std::max(peak, std::abs(sample));
    const float safetyGain = peak > 0.95f ? 0.95f / peak : 1.0f;
    for (float& sample : result.audio)
        sample *= safetyGain;

    result.voicedFrames = voicedFrames;
    result.totalFrames = frameCount;
    result.fftSize = spectralOption.fft_size;
    return result;
}
