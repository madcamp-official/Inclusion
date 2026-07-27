#pragma once

#include <atomic>

// 하울링(스피커→마이크 되먹임) 캘리브레이션. 테스트 게인을 서서히 올리며 입력 RMS가
// 급격히 튀는 지점을 하울링 조짐으로 보고, 그 직전 게인에 안전마진을 곱해
// 마스터 게인 상한으로 기록한다. UI 스레드가 startCalibration()을 호출하고,
// 오디오 스레드가 매 블록 processBlock()으로 진행시킨다(공용 인프라).
class FeedbackCalibrator
{
public:
    void prepare(double sampleRateIn);
    void reset();

    void startCalibration();
    bool isCalibrating() const { return calibrating.load(); }

    // 오디오 스레드에서 호출. 캘리브레이션 중이 아니면 outputTestGain=1.0을 반환한다.
    void processBlock(const float* inputBlock, int numSamples, float& outputTestGain);

    float getMasterGainCeiling() const { return masterGainCeiling.load(); }

private:
    double sampleRate = 44100.0;
    std::atomic<bool> calibrating { false };
    float currentTestGain = 0.0f;
    float previousInputRms = 0.0f;
    int consecutiveGrowthBlocks = 0;
    std::atomic<float> masterGainCeiling { 1.0f };

    static constexpr float gainRampPerSecond = 0.15f;
    static constexpr float feedbackGrowthRatioThreshold = 1.5f;
    static constexpr float safetyMarginRatio = 0.7f;

    // 한 번의 레벨 상승(기타를 치거나 말을 시작하는 것만으로도 일어난다)을 하울링으로
    // 오판하면 출력이 영구히 눌리므로, 연속 상승을 여러 블록 요구하고 하한도 넉넉히 둔다.
    static constexpr int requiredConsecutiveGrowthBlocks = 8;
    static constexpr float armTestGainThreshold = 0.25f;
    static constexpr float minimumCeiling = 0.35f;

    static float computeRms(const float* samples, int numSamples);
};
