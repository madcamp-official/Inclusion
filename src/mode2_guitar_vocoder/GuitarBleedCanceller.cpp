#include "GuitarBleedCanceller.h"

#include <algorithm>
#include <cmath>

void GuitarBleedCanceller::prepare(double)
{
    taps = std::max(16, mode2::params::bleedCancelFilterTaps);
    weights.assign(static_cast<size_t>(taps), 0.0f);
    referenceLine.assign(static_cast<size_t>(taps) * 2, 0.0f);
    reset();
}

void GuitarBleedCanceller::reset()
{
    std::fill(weights.begin(), weights.end(), 0.0f);
    std::fill(referenceLine.begin(), referenceLine.end(), 0.0f);
    writePos = 0;
    referenceEnergy = 0.0f;
    previousEstimateRms = 0.0f;
    levelEmaIn = 0.0f;
    levelEmaOut = 0.0f;
    cancelledDb.store(0.0f);
    adapting.store(false);
}

void GuitarBleedCanceller::processBlock(const float* vocalIn, const float* guitarRef, float* vocalOut, int numSamples)
{
    if (! enabled.load() || weights.empty())
    {
        if (vocalOut != vocalIn)
            std::copy(vocalIn, vocalIn + numSamples, vocalOut);
        adapting.store(false);
        return;
    }

    // 참조(기타)가 사실상 없으면 학습하지 않는다. 무신호 구간에서 계수가 목소리 쪽으로
    // 끌려가면, 정작 기타가 들어올 때 엉뚱한 것을 빼게 된다.
    double refSumSquares = 0.0;
    for (int i = 0; i < numSamples; ++i)
        refSumSquares += static_cast<double>(guitarRef[i]) * guitarRef[i];
    const float refRms = numSamples > 0 ? static_cast<float>(std::sqrt(refSumSquares / numSamples)) : 0.0f;
    const bool shouldAdapt = refRms > mode2::params::bleedCancelReferenceRmsFloor;
    adapting.store(shouldAdapt);

    // 누적 오차가 쌓이지 않게 윈도우 에너지를 블록마다 다시 계산한다(O(taps), 블록당 1회).
    referenceEnergy = 0.0f;
    for (int k = 1; k <= taps; ++k)
    {
        const float v = referenceLine[static_cast<size_t>(writePos + k)];
        referenceEnergy += v * v;
    }

    // 가변 스텝: 목소리 채널 중 "참조로 설명되는 몫"의 비율만큼만 학습한다. 직전 블록의
    // 추정 유입 레벨을 쓰는 이유는, 이번 블록의 추정이 갱신 결과에 다시 의존하기 때문이다.
    // 경로는 천천히 변하므로 한 블록 지연은 문제가 되지 않는다.
    double desiredSumSquares = 0.0;
    for (int i = 0; i < numSamples; ++i)
        desiredSumSquares += static_cast<double>(vocalIn[i]) * vocalIn[i];
    const float desiredRms = numSamples > 0 ? static_cast<float>(std::sqrt(desiredSumSquares / numSamples)) : 0.0f;
    const float explained = std::clamp(previousEstimateRms / (desiredRms + 1.0e-9f),
                                       mode2::params::bleedCancelAdaptFloor, 1.0f);

    const float mu = mode2::params::bleedCancelStepSize * explained;
    const float leak = mode2::params::bleedCancelLeakage;
    double inSumSquares = 0.0, outSumSquares = 0.0, estimateSumSquares = 0.0;

    for (int n = 0; n < numSamples; ++n)
    {
        // 가장 오래된 샘플이 빠지고 새 샘플이 들어온다.
        const float dropped = referenceLine[static_cast<size_t>(writePos)];
        const float x = guitarRef[n];
        referenceLine[static_cast<size_t>(writePos)] = x;
        referenceLine[static_cast<size_t>(writePos + taps)] = x;
        referenceEnergy += x * x - dropped * dropped;
        if (referenceEnergy < 0.0f)
            referenceEnergy = 0.0f;

        // 윈도우는 [writePos+1, writePos+taps]에 오래된 것 → 최신 순으로 연속해 있다.
        const float* window = referenceLine.data() + writePos + 1;

        float estimate = 0.0f;
        for (int k = 0; k < taps; ++k)
            estimate += weights[static_cast<size_t>(k)] * window[k];

        const float d = vocalIn[n];
        const float e = d - estimate;   // 기타 성분을 뺀 목소리

        if (shouldAdapt)
        {
            // NLMS: 참조 에너지로 정규화해 입력 레벨과 무관하게 같은 속도로 수렴한다.
            const float norm = mu / (referenceEnergy + mode2::params::bleedCancelRegularization);
            const float step = norm * e;
            const float decay = 1.0f - leak;
            for (int k = 0; k < taps; ++k)
                weights[static_cast<size_t>(k)] = weights[static_cast<size_t>(k)] * decay + step * window[k];
        }

        vocalOut[n] = e;

        inSumSquares += static_cast<double>(d) * d;
        outSumSquares += static_cast<double>(e) * e;
        estimateSumSquares += static_cast<double>(estimate) * estimate;

        writePos = writePos + 1 < taps ? writePos + 1 : 0;
    }

    if (numSamples > 0)
    {
        previousEstimateRms = static_cast<float>(std::sqrt(estimateSumSquares / numSamples));

        const float inRms = static_cast<float>(std::sqrt(inSumSquares / numSamples));
        const float outRms = static_cast<float>(std::sqrt(outSumSquares / numSamples));
        const float coeff = mode2::params::bleedCancelLevelEmaCoeff;
        levelEmaIn += coeff * (inRms - levelEmaIn);
        levelEmaOut += coeff * (outRms - levelEmaOut);

        if (levelEmaIn > 1.0e-6f && levelEmaOut > 1.0e-9f)
            cancelledDb.store(20.0f * std::log10(levelEmaIn / levelEmaOut));
        else
            cancelledDb.store(0.0f);
    }
}
