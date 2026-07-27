#include "PitchAccuracyMetrics.h"

#include <world/harvest.h>
#include <world/stonemask.h>

#include <algorithm>
#include <cmath>

namespace
{
    constexpr double framePeriodMs = 5.0;
    // 옥타브 -1 설정에서 기타 6번줄 개방현(E2 82Hz)의 목표는 41Hz까지 내려간다. StoneMask의
    // 하한이 40Hz이므로 그보다 낮게 잡을 이유는 없다.
    constexpr double f0Floor = 40.0;
    constexpr double f0Ceil = 1000.0;

    // 지연 탐색 간격. 1ms보다 촘촘히 볼 필요는 없다 — 1ms 어긋남이 만드는 음정 오차는
    // 아래 지표의 분해능보다 훨씬 작다.
    constexpr double lagSearchStepMs = 1.0;

    float percentileOfSorted(const std::vector<float>& sorted, double p)
    {
        if (sorted.empty())
            return 0.0f;
        const double index = p * static_cast<double>(sorted.size() - 1);
        return sorted[static_cast<size_t>(std::clamp(index, 0.0, static_cast<double>(sorted.size() - 1)))];
    }
}

PitchAccuracyMetrics::Result PitchAccuracyMetrics::measure(const float* output,
                                                           int numSamples,
                                                           double sampleRate,
                                                           const std::vector<float>& absoluteTargetF0Trace,
                                                           int nominalLagSamples,
                                                           int searchRadiusSamples)
{
    Result result;
    result.nominalLagSamples = nominalLagSamples;

    if (output == nullptr || numSamples <= 0 || sampleRate <= 0.0
        || static_cast<int>(absoluteTargetF0Trace.size()) < numSamples)
        return result;

    std::vector<double> signal(static_cast<size_t>(numSamples));
    for (int i = 0; i < numSamples; ++i)
        signal[static_cast<size_t>(i)] = static_cast<double>(output[i]);

    const int fs = static_cast<int>(std::lround(sampleRate));

    HarvestOption option;
    InitializeHarvestOption(&option);
    option.f0_floor = f0Floor;
    option.f0_ceil = f0Ceil;
    option.frame_period = framePeriodMs;

    const int frameCount = GetSamplesForHarvest(fs, numSamples, framePeriodMs);
    if (frameCount <= 0)
        return result;

    std::vector<double> timeAxis(static_cast<size_t>(frameCount), 0.0);
    std::vector<double> rawF0(static_cast<size_t>(frameCount), 0.0);
    std::vector<double> f0(static_cast<size_t>(frameCount), 0.0);
    Harvest(signal.data(), numSamples, fs, &option, timeAxis.data(), rawF0.data());
    // Harvest만으로도 옥타브는 잘 맞지만, cents 단위로 비교하려면 추정기 자신의 오차를
    // 최대한 줄여야 한다. StoneMask는 그 목적의 정제 단계다.
    StoneMask(signal.data(), numSamples, fs, timeAxis.data(), rawF0.data(), frameCount, f0.data());

    result.totalFrames = frameCount;
    for (int frame = 0; frame < frameCount; ++frame)
        if (f0[static_cast<size_t>(frame)] > 0.0)
            ++result.voicedFrames;

    // 출력은 입력보다 늦다(제어 lookahead + 시프터 고유 지연). 목표 궤적은 입력 시간축이므로
    // 그만큼 되돌려 조회해야 같은 순간끼리 비교된다. 이론값을 그대로 쓰지 않고 주변을
    // 탐색하는 이유는 두 가지다. 백엔드마다 지연이 다르고, 탐색 결과 자체가 "출력이 실제로
    // 얼마나 늦는가"의 실측치가 되기 때문이다.
    //
    // F0 분석은 지연과 무관하므로 한 번만 하고, 목표를 조회하는 인덱스만 옮겨가며 비교한다.
    auto collectSignedCents = [&](int lagSamples, std::vector<float>& centsOut)
    {
        centsOut.clear();
        for (int frame = 0; frame < frameCount; ++frame)
        {
            const double measured = f0[static_cast<size_t>(frame)];
            if (measured <= 0.0)
                continue;

            const int sampleIndex =
                static_cast<int>(std::lround(timeAxis[static_cast<size_t>(frame)] * sampleRate))
                - lagSamples;
            if (sampleIndex < 0 || sampleIndex >= numSamples)
                continue;

            const float target = absoluteTargetF0Trace[static_cast<size_t>(sampleIndex)];
            if (target <= 0.0f)
                continue;

            centsOut.push_back(static_cast<float>(
                1200.0 * std::log2(measured / static_cast<double>(target))));
        }
    };

    const int lagStep = std::max(1, static_cast<int>(std::lround(sampleRate * lagSearchStepMs / 1000.0)));
    const int lagLow = nominalLagSamples - std::max(0, searchRadiusSamples);
    const int lagHigh = nominalLagSamples + std::max(0, searchRadiusSamples);

    struct Candidate
    {
        int lag = 0;
        int count = 0;
        float medianAbs = 0.0f;
        // 지연을 고를 때 쓰는 기준값. 중앙값을 쓰면 안 된다 — 정렬이 어긋났을 때 실제로
        // 망가지는 것은 음이 바뀌는 순간의 프레임들뿐이고, 중앙값은 정의상 그 소수를
        // 무시한다(그래서 지연을 수십 ms 어긋내도 값이 거의 그대로다). 반대로 평균은
        // 옥타브 오검출 한 프레임에 통째로 끌려간다. 그래서 600 cents에서 자른 평균을 쓴다.
        float clippedMeanAbs = 0.0f;
    };

    std::vector<Candidate> candidates;
    std::vector<float> signedCents;
    std::vector<float> absCents;
    int maxCount = 0;

    for (int lag = lagLow; lag <= lagHigh; lag += lagStep)
    {
        collectSignedCents(lag, signedCents);
        if (signedCents.size() < 8)
            continue;

        absCents.clear();
        double clippedSum = 0.0;
        for (float cents : signedCents)
        {
            const float magnitude = std::abs(cents);
            absCents.push_back(magnitude);
            clippedSum += std::min(magnitude, 600.0f);
        }
        std::sort(absCents.begin(), absCents.end());

        candidates.push_back({ lag,
                               static_cast<int>(absCents.size()),
                               percentileOfSorted(absCents, 0.5),
                               static_cast<float>(clippedSum / static_cast<double>(absCents.size())) });
        maxCount = std::max(maxCount, static_cast<int>(absCents.size()));
    }

    if (candidates.empty())
        return result;

    // 지연을 크게 어긋내면 비교 가능한 프레임 자체가 줄어, 남은 소수가 우연히 잘 맞는 것만으로
    // 중앙값이 낮아질 수 있다. 표본이 충분한 후보만 승자 후보로 본다.
    const int minimumCount = std::max(8, static_cast<int>(0.8 * maxCount));
    const Candidate* lowest = nullptr;
    for (const auto& candidate : candidates)
    {
        if (candidate.count < minimumCount)
            continue;
        if (lowest == nullptr || candidate.clippedMeanAbs < lowest->clippedMeanAbs)
            lowest = &candidate;
    }
    if (lowest == nullptr)
        return result;

    // 최소값 하나만 보고 지연을 정하면 안 된다. 기타가 같은 음을 오래 끌면 지연을 수십 ms
    // 어긋내도 비교 결과가 거의 변하지 않아서, 사실상 동점인 후보가 넓게 깔린다. 그때의
    // "최소값"은 잡음이 고른 값일 뿐이다.
    //
    // 그래서 동점 구간(plateau)을 먼저 구하고, 그 안에서는 이론값에 가장 가까운 지연을 고른다.
    // 데이터가 실제로 구분해 줄 때만 이론값을 뒤집는다는 뜻이다.
    const float tieTolerance = std::max(1.0f, lowest->clippedMeanAbs * 0.05f);
    const Candidate* best = nullptr;
    int plateauLow = 0;
    int plateauHigh = 0;
    bool havePlateau = false;
    for (const auto& candidate : candidates)
    {
        if (candidate.count < minimumCount || candidate.clippedMeanAbs > lowest->clippedMeanAbs + tieTolerance)
            continue;

        plateauLow = havePlateau ? std::min(plateauLow, candidate.lag) : candidate.lag;
        plateauHigh = havePlateau ? std::max(plateauHigh, candidate.lag) : candidate.lag;
        havePlateau = true;

        if (best == nullptr
            || std::abs(candidate.lag - nominalLagSamples) < std::abs(best->lag - nominalLagSamples))
            best = &candidate;
    }
    if (best == nullptr)
        return result;

    result.lagPlateauSamples = plateauHigh - plateauLow;
    // 20ms보다 넓게 퍼져 있으면 이 녹음으로는 지연을 잴 수 없다고 본다. 음이 자주 바뀌는
    // 실제 연주에서는 이 폭이 수 ms로 좁아진다.
    result.lagIdentifiable =
        static_cast<double>(result.lagPlateauSamples) <= 0.020 * sampleRate;

    for (const auto& candidate : candidates)
        if (candidate.lag == nominalLagSamples)
            result.medianAbsCentsAtNominalLag = candidate.medianAbs;

    collectSignedCents(best->lag, signedCents);
    if (signedCents.empty())
        return result;

    absCents.clear();
    for (float cents : signedCents)
        absCents.push_back(std::abs(cents));
    std::sort(absCents.begin(), absCents.end());

    std::vector<float> sortedSigned = signedCents;
    std::sort(sortedSigned.begin(), sortedSigned.end());

    int withinFifty = 0;
    int withinTwenty = 0;
    int octaveErrors = 0;
    for (float cents : absCents)
    {
        if (cents < 50.0f)  ++withinFifty;
        if (cents < 20.0f)  ++withinTwenty;
        if (cents > 600.0f) ++octaveErrors;
    }

    const float total = static_cast<float>(absCents.size());
    result.valid = true;
    result.comparedFrames = static_cast<int>(absCents.size());
    result.withinFiftyCentsPercent = 100.0f * withinFifty / total;
    result.withinTwentyCentsPercent = 100.0f * withinTwenty / total;
    result.octaveErrorPercent = 100.0f * octaveErrors / total;
    result.medianAbsCents = percentileOfSorted(absCents, 0.5);
    result.p90AbsCents = percentileOfSorted(absCents, 0.9);
    result.medianSignedCents = percentileOfSorted(sortedSigned, 0.5);
    result.bestLagSamples = best->lag;
    return result;
}
