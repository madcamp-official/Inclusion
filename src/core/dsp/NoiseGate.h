#pragma once

// 사이드체인 노이즈 게이트.
//
// 판정 기준(key)과 게인을 적용할 신호를 분리해서 받는다. 모드 2에서는 "목소리 입력 레벨"로
// 판정하고 "출력 신호"에 게인을 걸기 때문이다.
//
// 단일 임계값만 쓰면 레벨이 임계 근처를 오갈 때 게이트가 떨리므로(chattering),
// 여는 임계와 닫는 임계를 다르게 두고(히스테리시스) 짧게 유지(hold)한 뒤 닫는다.
// 어택은 빠르게(말 시작을 놓치지 않게), 릴리스는 느리게(말꼬리가 잘리지 않게) 움직인다.
//
// 모드 1의 연주 활동 감지(8.2절)도 성질이 같으므로 core/dsp에 공용으로 둔다.
class NoiseGate
{
public:
    void prepare(double sampleRateIn, int maxBlockSize);
    void reset();

    void setEnabled(bool shouldEnable) { enabled = shouldEnable; }
    bool isEnabled() const { return enabled; }

    // 게이트를 여는 레벨(RMS). 닫는 레벨은 여기에 히스테리시스 비율을 곱한 값이다.
    void setThreshold(float openThresholdRms) { openThreshold = openThresholdRms; }
    float getThreshold() const { return openThreshold; }

    // keyLevelRms로 열림/닫힘을 판정하고, samples에 게인을 샘플 단위로 부드럽게 적용한다.
    void processBlock(float* samples, int numSamples, float keyLevelRms);

    float getCurrentGain() const { return currentGain; }
    bool isOpen() const { return open; }

private:
    double sampleRate = 44100.0;
    bool enabled = true;
    bool open = false;

    float openThreshold = 0.02f;
    float currentGain = 0.0f;

    float attackCoeff = 0.0f;
    float releaseCoeff = 0.0f;
    int holdBlocksTotal = 0;
    int holdBlocksRemaining = 0;
};
