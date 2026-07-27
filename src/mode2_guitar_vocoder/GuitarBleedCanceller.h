#pragma once

#include "params/Mode2Params.h"

#include <atomic>
#include <vector>

// 목소리 마이크로 새어 들어온 기타 소리를 지운다.
//
// 왜 필요한가: 합성 신호로 측정하면, 목소리만 깨끗하게 들어올 때 이 파이프라인의 출력은
// 배음 비율 96.8% / 명료도 1.00으로 사실상 완벽하다. 그런데 목소리 채널에 기타가 섞이는
// 순간 무너진다(유입 -5dB에서 66.8%/0.71, -2dB에서 49.7%/0.57). 세 경로로 망가진다:
//   1. 유입된 기타가 목소리와 함께 시프트되어, 아무도 요청하지 않은 유령 음이 생긴다.
//   2. 목소리 피치 검출기가 기타 음을 목소리로 오인해 보정량이 통째로 틀어진다.
//   3. 그 탓에 신뢰도가 게이트 아래로 떨어져, 보정 안 된 원본이 출력으로 샌다.
// 즉 파라미터 튜닝으로 이길 수 있는 문제가 아니라 입력 단계에서 지워야 한다.
//
// 기타는 이미 별도 채널(ch0)로 들어오므로 그 신호를 참조로 쓸 수 있다. 기타 → 목소리 마이크
// 경로는 (지연 + 방 울림 + 마이크 특성)의 선형 시스템에 가까우므로, NLMS 적응 FIR로 그
// 경로를 추정해 목소리에서 빼면 된다. 음향 에코 제거(AEC)와 같은 구조다.
//
// 한계: 목소리 자체는 참조와 무관하므로 NLMS 입장에서는 잡음이다. 그래서 스텝을 작게 두고
// 여러 블록에 걸쳐 평균이 수렴하도록 한다. 완전 제거는 불가능하고, 물리적 분리(기타 DI 입력 +
// 근접 마이크)를 대체하지 못한다. 유입을 몇 dB 낮춰 붕괴 구간에서 빠져나오는 것이 목적이다.
class GuitarBleedCanceller
{
public:
    void prepare(double sampleRateIn);
    void reset();

    void setEnabled(bool shouldEnable) { enabled.store(shouldEnable); }
    bool isEnabled() const { return enabled.load(); }

    // vocalIn에서 guitarRef와 상관된 성분을 빼서 vocalOut에 쓴다.
    // vocalIn과 vocalOut은 같은 버퍼를 가리켜도 된다.
    //
    // 부스트를 적용하기 "전"의 원시 목소리에 걸어야 한다. 부스트 뒤에 걸면 사용자가
    // 슬라이더를 움직일 때마다 학습한 경로 이득이 어긋나 필터가 다시 수렴해야 한다.
    void processBlock(const float* vocalIn, const float* guitarRef, float* vocalOut, int numSamples);

    // 이 블록들에서 입력 대비 출력이 몇 dB 줄었는지(양수 = 제거되고 있음).
    // 노래를 하는 동안에는 목소리가 분모에 그대로 남아 0에 가깝게 보인다.
    // 기타만 치고 노래하지 않는 구간에서 읽어야 실제 제거량이 드러난다.
    float getCancelledDb() const { return cancelledDb.load(); }

    // 필터가 실제로 학습 중인지(기타 신호가 참조 문턱을 넘는지).
    bool isAdapting() const { return adapting.load(); }

private:
    std::atomic<bool> enabled { mode2::params::defaultBleedCancelEnabled };
    std::atomic<float> cancelledDb { 0.0f };
    std::atomic<bool> adapting { false };

    int taps = mode2::params::bleedCancelFilterTaps;

    // 참조 지연선. 길이 2*taps로 두고 같은 샘플을 pos와 pos+taps 두 곳에 써서,
    // 최근 taps개가 항상 [pos+1, pos+taps] 구간에 "오래된 것 → 최신" 순으로 연속하게 만든다.
    // 원형 인덱스를 매 탭마다 접지 않아도 되므로 내적과 갱신이 단순한 선형 루프가 된다.
    std::vector<float> weights;
    std::vector<float> referenceLine;
    int writePos = 0;

    // 슬라이딩 윈도우의 ||x||^2. 블록마다 새로 계산해 누적 오차가 쌓이지 않게 하고,
    // 블록 안에서는 증분 갱신한다.
    float referenceEnergy = 0.0f;

    // 직전 블록에서 추정한 유입 레벨. 가변 스텝의 분자로 쓴다(자세한 이유는
    // Mode2Params.h의 bleedCancelAdaptFloor 주석).
    float previousEstimateRms = 0.0f;

    float levelEmaIn = 0.0f;
    float levelEmaOut = 0.0f;
};
