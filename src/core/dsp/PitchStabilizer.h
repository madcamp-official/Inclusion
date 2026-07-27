#pragma once

#include "PitchDetector.h"

// 7.5절 안정화 상태기계: IDLE → ATTACK → COLLECT → STABLE → COAST.
// 온셋 + 피치 추정치를 받아 "지금 확정된 목표 음정이 무엇인가"를 판단한다.
// 모드 2의 GuitarTargetTracker가 사용하지만, 온셋/피치 기반 안정화는 범용이라 core/dsp에 둔다.
class PitchStabilizer
{
public:
    enum class State
    {
        Idle,
        Attack,
        Collect,
        Stable,
        Coast
    };

    struct Output
    {
        State state = State::Idle;
        bool hasTarget = false;
        float targetMidi = 0.0f;
        // 1.0 = 이 타겟을 온전히 신뢰, COAST의 FADE 구간에서 1→0으로 감쇠, IDLE/신규 노트 확정 전에는 0.0
        float fadeGain = 0.0f;
    };

    void prepare(double sampleRateIn);
    void reset();

    // onsetDetected: 이 블록에 새 온셋이 있었는지. pitch: 같은 블록의 PitchDetector 결과.
    Output processBlock(bool onsetDetected, const PitchDetector::Result& pitch, int numSamples);

private:
    double sampleRate = 44100.0;
    State state = State::Idle;

    long long samplesInState = 0;
    float lastValidMidi = 0.0f;
    int collectedStableCount = 0;
    // STABLE에서 목표를 벗어난 피치가 연속으로 몇 블록 관측됐는지(온셋 없는 음 변화 감지용).
    int stableDeviationBlocks = 0;

    bool haveConfirmedTarget = false;
    float confirmedTargetMidi = 0.0f;

    long long attackSettleSamples = 0;
    long long coastHoldSamples = 0;
    long long coastFadeSamples = 0;

    void enterState(State newState);
    float computeFadeGain() const;
};
