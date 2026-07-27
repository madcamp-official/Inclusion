#include "WorldRealtimePitchShifter.h"

#include <world/cheaptrick.h>
#include <world/d4c.h>
#include <world/harvest.h>
#include <world/synthesis.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numbers>

namespace
{
    constexpr double framePeriodMs = 5.0;
    constexpr double windowDurationSeconds = 0.160;
    constexpr double hopDurationSeconds = 0.080;
    constexpr double overlapDurationSeconds = 0.040;
    constexpr double f0Floor = 70.0;
    constexpr double f0Ceil = 700.0;
    constexpr double synthesisF0Floor = 40.0;
}

WorldRealtimePitchShifter::~WorldRealtimePitchShifter()
{
    stopWorker();
}

void WorldRealtimePitchShifter::prepare(double sampleRateIn, int maxBlockSize)
{
    stopWorker();

    sampleRate = sampleRateIn;
    windowSamples = std::max(1, static_cast<int>(std::lround(sampleRate * windowDurationSeconds)));
    hopSamples = std::max(1, static_cast<int>(std::lround(sampleRate * hopDurationSeconds)));
    const int overlapSamples =
        std::max(1, static_cast<int>(std::lround(sampleRate * overlapDurationSeconds)));
    preRollSamples = (windowSamples - hopSamples - overlapSamples) / 2;

    // 120 ms 합성 구간을 80 ms마다 만들고 인접 구간의 40 ms를 겹쳐 잇는다.
    // 첫 창은 왼쪽에 20 ms 무음을 붙여 실제 입력 140 ms가 모였을 때 첫 80 ms를 낸다.
    latencySamples = windowSamples - preRollSamples;

    const size_t ringCapacity = static_cast<size_t>(
        std::max(static_cast<int>(sampleRate * 2.0), windowSamples + maxBlockSize * 8)) + 1;
    inputRing.assign(ringCapacity, {});
    outputRing.assign(ringCapacity, 0.0f);
    analysisAudio.assign(static_cast<size_t>(windowSamples), 0.0);
    analysisTargetF0.assign(static_cast<size_t>(windowSamples), 0.0f);
    analysisFallbackShift.assign(static_cast<size_t>(windowSamples), 0.0f);
    previousOverlap.assign(static_cast<size_t>(overlapSamples), 0.0f);
    analysisFill = preRollSamples;
    havePreviousOverlap = false;

    inputWrite.store(0, std::memory_order_relaxed);
    inputRead.store(0, std::memory_order_relaxed);
    outputWrite.store(0, std::memory_order_relaxed);
    outputRead.store(0, std::memory_order_relaxed);
    startWorker();
}

void WorldRealtimePitchShifter::reset()
{
    if (inputRing.empty())
        return;

    stopWorker();
    std::fill(analysisAudio.begin(), analysisAudio.end(), 0.0);
    std::fill(analysisTargetF0.begin(), analysisTargetF0.end(), 0.0f);
    std::fill(analysisFallbackShift.begin(), analysisFallbackShift.end(), 0.0f);
    std::fill(previousOverlap.begin(), previousOverlap.end(), 0.0f);
    analysisFill = preRollSamples;
    havePreviousOverlap = false;
    inputWrite.store(0, std::memory_order_relaxed);
    inputRead.store(0, std::memory_order_relaxed);
    outputWrite.store(0, std::memory_order_relaxed);
    outputRead.store(0, std::memory_order_relaxed);
    startWorker();
}

void WorldRealtimePitchShifter::processBlock(const float* input, float* output, int numSamples,
                                             float absoluteTargetF0Hz,
                                             float fallbackSemitones)
{
    for (int i = 0; i < numSamples; ++i)
    {
        pushInput({ input[i], absoluteTargetF0Hz, fallbackSemitones });

        float ready = 0.0f;
        output[i] = popOutput(ready) ? ready : 0.0f;
    }
}

void WorldRealtimePitchShifter::startWorker()
{
    shouldStop.store(false, std::memory_order_release);
    worker = std::thread([this] { workerLoop(); });
}

void WorldRealtimePitchShifter::stopWorker()
{
    shouldStop.store(true, std::memory_order_release);
    if (worker.joinable())
        worker.join();
}

void WorldRealtimePitchShifter::workerLoop()
{
    while (! shouldStop.load(std::memory_order_acquire))
    {
        InputSample sample;
        bool consumedAny = false;
        while (analysisFill < windowSamples && popInput(sample))
        {
            const size_t index = static_cast<size_t>(analysisFill++);
            analysisAudio[index] = sample.audio;
            analysisTargetF0[index] = sample.targetF0Hz;
            analysisFallbackShift[index] = sample.fallbackSemitones;
            consumedAny = true;
        }

        if (analysisFill == windowSamples)
        {
            processWindow();

            const int retained = windowSamples - hopSamples;
            std::move(analysisAudio.begin() + hopSamples, analysisAudio.end(), analysisAudio.begin());
            std::move(analysisTargetF0.begin() + hopSamples, analysisTargetF0.end(),
                      analysisTargetF0.begin());
            std::move(analysisFallbackShift.begin() + hopSamples, analysisFallbackShift.end(),
                      analysisFallbackShift.begin());
            analysisFill = retained;
            consumedAny = true;
        }

        if (! consumedAny)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void WorldRealtimePitchShifter::processWindow()
{
    const int fs = static_cast<int>(std::lround(sampleRate));

    HarvestOption harvestOption;
    InitializeHarvestOption(&harvestOption);
    harvestOption.f0_floor = f0Floor;
    harvestOption.f0_ceil = f0Ceil;
    harvestOption.frame_period = framePeriodMs;

    const int frameCount = GetSamplesForHarvest(fs, windowSamples, framePeriodMs);
    std::vector<double> timeAxis(static_cast<size_t>(frameCount));
    std::vector<double> sourceF0(static_cast<size_t>(frameCount));
    Harvest(analysisAudio.data(), windowSamples, fs, &harvestOption,
            timeAxis.data(), sourceF0.data());

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

    CheapTrick(analysisAudio.data(), windowSamples, fs, timeAxis.data(), sourceF0.data(),
               frameCount, &spectralOption, spectralPointers.data());

    D4COption d4cOption;
    InitializeD4COption(&d4cOption);
    D4C(analysisAudio.data(), windowSamples, fs, timeAxis.data(), sourceF0.data(),
        frameCount, spectralOption.fft_size, &d4cOption, aperiodicityPointers.data());

    std::vector<double> targetF0 = sourceF0;
    std::vector<float> frameVoicing(static_cast<size_t>(frameCount), 0.0f);
    for (int frame = 0; frame < frameCount; ++frame)
    {
        const size_t frameIndex = static_cast<size_t>(frame);
        if (sourceF0[frameIndex] <= 0.0)
            continue;

        frameVoicing[frameIndex] = 1.0f;
        const int sampleIndex = std::clamp(
            static_cast<int>(std::lround(timeAxis[frameIndex] * sampleRate)),
            0, windowSamples - 1);
        const float absoluteTarget = analysisTargetF0[static_cast<size_t>(sampleIndex)];
        if (absoluteTarget > 0.0f)
        {
            targetF0[frameIndex] = std::clamp(
                static_cast<double>(absoluteTarget), synthesisF0Floor, 1000.0);
        }
        else
        {
            const float semitones =
                analysisFallbackShift[static_cast<size_t>(sampleIndex)];
            targetF0[frameIndex] = std::clamp(
                sourceF0[frameIndex] * std::pow(2.0, static_cast<double>(semitones) / 12.0),
                synthesisF0Floor, 1000.0);
        }
    }

    std::vector<double> synthesized(static_cast<size_t>(windowSamples), 0.0);
    Synthesis(targetF0.data(), frameCount, spectralPointers.data(),
              aperiodicityPointers.data(), spectralOption.fft_size,
              framePeriodMs, fs, windowSamples, synthesized.data());

    // 창 가장자리는 분석 오차가 크므로 중앙의 hop + overlap 구간만 쓴다. WORLD 합성은
    // 창마다 여기 위상이 새로 시작되므로, 인접 창의 같은 40 ms를 equal-power
    // crossfade하지 않으면 80 ms 주기의 클릭이 저주파 "두두두"로 들린다.
    const int overlapSamples = static_cast<int>(previousOverlap.size());
    const int segmentSamples = hopSamples + overlapSamples;
    const int outputBegin = (windowSamples - segmentSamples) / 2;
    const double framesPerSample = 1000.0 / (framePeriodMs * sampleRate);
    const float mixCoeff = 1.0f - std::exp(-1.0f / static_cast<float>(0.006 * sampleRate));
    float voicedMix = frameVoicing.empty()
        ? 0.0f
        : frameVoicing[static_cast<size_t>(std::clamp(
              static_cast<int>(outputBegin * framesPerSample), 0, frameCount - 1))];

    std::vector<float> segment(static_cast<size_t>(segmentSamples), 0.0f);
    for (int offset = 0; offset < segmentSamples; ++offset)
    {
        const int sampleIndex = outputBegin + offset;
        const double framePosition = static_cast<double>(sampleIndex) * framesPerSample;
        const int frame0 = std::clamp(static_cast<int>(framePosition), 0, frameCount - 1);
        const int frame1 = std::min(frame0 + 1, frameCount - 1);
        const float fraction = static_cast<float>(framePosition - frame0);
        const float targetMix = frameVoicing[static_cast<size_t>(frame0)] * (1.0f - fraction)
                              + frameVoicing[static_cast<size_t>(frame1)] * fraction;
        voicedMix += mixCoeff * (targetMix - voicedMix);

        const float original = static_cast<float>(analysisAudio[static_cast<size_t>(sampleIndex)]);
        const float world = static_cast<float>(synthesized[static_cast<size_t>(sampleIndex)]);
        segment[static_cast<size_t>(offset)] =
            world * voicedMix + original * (1.0f - voicedMix);
    }

    if (! havePreviousOverlap)
    {
        // 첫 구간의 앞 80 ms는 아직 다음 창과 겹치지 않으므로 바로 낼 수 있다.
        for (int i = 0; i < hopSamples; ++i)
            pushOutput(segment[static_cast<size_t>(i)]);
        havePreviousOverlap = true;
    }
    else
    {
        // 이전 창의 꼬리와 현재 창의 머리는 시간상 같은 40 ms다. sin/cos 곡선은
        // 중간에서 에너지가 꺼지는 현상 없이 서로 다른 합성 위상을 부드럽게 교체한다.
        for (int i = 0; i < overlapSamples; ++i)
        {
            const float phase = static_cast<float>(i + 1)
                              / static_cast<float>(overlapSamples + 1)
                              * (std::numbers::pi_v<float> * 0.5f);
            pushOutput(previousOverlap[static_cast<size_t>(i)] * std::cos(phase)
                       + segment[static_cast<size_t>(i)] * std::sin(phase));
        }

        const int uniqueSamples = hopSamples - overlapSamples;
        for (int i = 0; i < uniqueSamples; ++i)
            pushOutput(segment[static_cast<size_t>(overlapSamples + i)]);
    }

    std::copy(segment.end() - overlapSamples, segment.end(), previousOverlap.begin());
}

bool WorldRealtimePitchShifter::pushInput(const InputSample& sample)
{
    const size_t write = inputWrite.load(std::memory_order_relaxed);
    const size_t next = (write + 1) % inputRing.size();
    if (next == inputRead.load(std::memory_order_acquire))
        return false;
    inputRing[write] = sample;
    inputWrite.store(next, std::memory_order_release);
    return true;
}

bool WorldRealtimePitchShifter::popInput(InputSample& sample)
{
    const size_t read = inputRead.load(std::memory_order_relaxed);
    if (read == inputWrite.load(std::memory_order_acquire))
        return false;
    sample = inputRing[read];
    inputRead.store((read + 1) % inputRing.size(), std::memory_order_release);
    return true;
}

bool WorldRealtimePitchShifter::pushOutput(float sample)
{
    const size_t write = outputWrite.load(std::memory_order_relaxed);
    const size_t next = (write + 1) % outputRing.size();
    if (next == outputRead.load(std::memory_order_acquire))
        return false;
    outputRing[write] = sample;
    outputWrite.store(next, std::memory_order_release);
    return true;
}

bool WorldRealtimePitchShifter::popOutput(float& sample)
{
    const size_t read = outputRead.load(std::memory_order_relaxed);
    if (read == outputWrite.load(std::memory_order_acquire))
        return false;
    sample = outputRing[read];
    outputRead.store((read + 1) % outputRing.size(), std::memory_order_release);
    return true;
}
