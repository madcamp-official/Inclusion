#pragma once

// 블록 단위 RMS 에너지 플럭스 기반 온셋 검출기.
// 이동평균(EMA) 베이스라인 대비 적응 임계값을 넘고, 최소 온셋 간격(refractory)이
// 지났을 때만 온셋으로 판정한다. 모드 1/모드 2 공용(work-split.md 공용 설계 방침).
class OnsetDetector
{
public:
    void prepare(double sampleRateIn);
    void reset();

    // samples: 모노 블록, numSamples > 0. 이 블록의 시작 지점에서 온셋이 검출되면 true.
    bool processBlock(const float* samples, int numSamples);

private:
    double sampleRate = 44100.0;
    long long minIntervalSamples = 0;
    long long samplesSinceLastOnset = 0;
    float runningEnergy = 0.0f;
    static constexpr float baselineEmaCoeff = 0.05f;

    static float computeRms(const float* samples, int numSamples);
};
