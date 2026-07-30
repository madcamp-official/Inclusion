#include "AudioLatencyCalibrator.h"

#include <algorithm>
#include <cmath>
#include <numeric>

void AudioLatencyCalibrator::prepare(double sampleRate)
{
    cancel();
    preparedSampleRate = sampleRate > 0.0 ? sampleRate : 48'000.0;
    preRollSamples = juce::roundToInt(preparedSampleRate * 0.20);
    probeSpacingSamples = juce::roundToInt(preparedSampleRate * 0.35);
    captureLengthSamples =
        getEmissionSample(probeCount - 1)
        + juce::roundToInt(preparedSampleRate * 0.65);
    capturedInput.assign(
        static_cast<size_t>(captureLengthSamples), 0.0f);
    buildProbe();
}

bool AudioLatencyCalibrator::start()
{
    if (state.load() != State::idle || capturedInput.empty())
        return false;

    std::fill(capturedInput.begin(), capturedInput.end(), 0.0f);
    writePosition = 0;
    state.store(State::measuring);
    return true;
}

void AudioLatencyCalibrator::cancel()
{
    state.store(State::idle);
    writePosition = 0;
}

void AudioLatencyCalibrator::processBlock(
    const float* input,
    float* outputLeft,
    float* outputRight,
    int numSamples) noexcept
{
    if (state.load() != State::measuring || numSamples <= 0)
        return;

    for (int sample = 0; sample < numSamples; ++sample)
    {
        const int timelineSample = writePosition + sample;
        if (timelineSample >= captureLengthSamples)
            break;

        capturedInput[static_cast<size_t>(timelineSample)] =
            input != nullptr ? input[sample] : 0.0f;

        float probeSample = 0.0f;
        for (int probeIndex = 0; probeIndex < probeCount; ++probeIndex)
        {
            const int offset =
                timelineSample - getEmissionSample(probeIndex);
            if (offset >= 0 && offset < probeLength)
            {
                probeSample = probe[static_cast<size_t>(offset)] * probeLevel;
                break;
            }
        }

        if (outputLeft != nullptr)
            outputLeft[sample] += probeSample;
        if (outputRight != nullptr)
            outputRight[sample] += probeSample;
    }

    writePosition += numSamples;
    if (writePosition >= captureLengthSamples)
        state.store(State::analysisPending);
}

bool AudioLatencyCalibrator::isMeasuring() const noexcept
{
    return state.load() == State::measuring;
}

bool AudioLatencyCalibrator::isAnalysisPending() const noexcept
{
    return state.load() == State::analysisPending;
}

AudioLatencyCalibrator::Result
AudioLatencyCalibrator::analyseCompletedCapture()
{
    Result result;
    result.totalProbes = probeCount;
    if (state.load() != State::analysisPending)
    {
        result.message = L"분석할 레이턴시 측정 데이터가 없습니다.";
        return result;
    }

    const int maximumDelay =
        juce::roundToInt(preparedSampleRate * 0.30);
    double probeEnergy = 0.0;
    for (const float sample : probe)
        probeEnergy += static_cast<double>(sample) * sample;

    std::vector<double> delays;
    std::vector<float> correlations;
    delays.reserve(probeCount);
    correlations.reserve(probeCount);

    for (int probeIndex = 0; probeIndex < probeCount; ++probeIndex)
    {
        const int emission = getEmissionSample(probeIndex);
        double bestCorrelation = 0.0;
        int bestDelay = 0;

        for (int delay = 0; delay <= maximumDelay; ++delay)
        {
            const int start = emission + delay;
            if (start + probeLength > captureLengthSamples)
                break;

            double dot = 0.0;
            double inputEnergy = 0.0;
            for (int sample = 0; sample < probeLength; ++sample)
            {
                const double input =
                    capturedInput[static_cast<size_t>(start + sample)];
                const double reference =
                    probe[static_cast<size_t>(sample)];
                dot += input * reference;
                inputEnergy += input * input;
            }

            const double denominator =
                std::sqrt(std::max(1.0e-15, inputEnergy * probeEnergy));
            const double correlation = std::abs(dot / denominator);
            if (correlation > bestCorrelation)
            {
                bestCorrelation = correlation;
                bestDelay = delay;
            }
        }

        if (bestCorrelation >= 0.22)
        {
            delays.push_back(static_cast<double>(bestDelay));
            correlations.push_back(static_cast<float>(bestCorrelation));
        }
    }

    result.successfulProbes = static_cast<int>(delays.size());
    if (delays.size() < 3)
    {
        result.message =
            L"테스트 신호를 충분히 찾지 못했습니다. 케이블, 출력 볼륨, "
            L"선택한 기타 입력 채널을 확인하세요.";
        state.store(State::idle);
        return result;
    }

    std::sort(delays.begin(), delays.end());
    const double median = delays[delays.size() / 2];
    std::vector<double> absoluteDeviations;
    absoluteDeviations.reserve(delays.size());
    for (const double delay : delays)
        absoluteDeviations.push_back(std::abs(delay - median));
    std::sort(absoluteDeviations.begin(), absoluteDeviations.end());

    result.roundTripSamples = median;
    result.jitterSamples =
        absoluteDeviations[absoluteDeviations.size() / 2];
    result.correlation = std::accumulate(
        correlations.begin(), correlations.end(), 0.0f)
        / static_cast<float>(correlations.size());
    result.valid =
        result.roundTripSamples > 0.0
        && result.jitterSamples <= preparedSampleRate * 0.003;
    result.message = result.valid
        ? L"레이턴시 왕복 측정이 완료되었습니다."
        : L"측정 편차가 큽니다. 모니터링/이펙트를 끄고 다시 측정하세요.";

    state.store(State::idle);
    return result;
}

void AudioLatencyCalibrator::buildProbe()
{
    uint32_t randomState = 0x91e10da5u;
    float mean = 0.0f;
    for (auto& sample : probe)
    {
        randomState ^= randomState << 13;
        randomState ^= randomState >> 17;
        randomState ^= randomState << 5;
        sample = (randomState & 1u) != 0u ? 1.0f : -1.0f;
        mean += sample;
    }

    mean /= static_cast<float>(probe.size());
    for (auto& sample : probe)
        sample -= mean;
}

int AudioLatencyCalibrator::getEmissionSample(int probeIndex) const noexcept
{
    return preRollSamples + probeIndex * probeSpacingSamples;
}
