#pragma once

#include "core/dsp/OnsetDetector.h"
#include "core/dsp/PitchDetector.h"
#include "core/dsp/PitchStabilizer.h"

// 기타 채널 전용: 온셋 검출 → 피치 검출 → 안정화를 묶어 "지금의 목표 음정"을 노출한다.
class GuitarTargetTracker
{
public:
    struct Output
    {
        bool hasTarget = false;
        float targetMidi = 0.0f;
        float fadeGain = 0.0f;
        PitchStabilizer::State state = PitchStabilizer::State::Idle;
    };

    void prepare(double sampleRateIn);
    void reset();

    Output processBlock(const float* guitarSamples, int numSamples);

    // 기타 목표는 "검출된 순간 주파수"가 아니라 연주한 음표여야 한다. 반음 경계에서
    // 검출 지터로 두 음이 왕복하지 않도록 직전 음표 중심의 히스테리시스를 적용한다.
    static float quantizeMidiWithHysteresis(float midi, bool havePreviousNote, float previousNote);

private:
    OnsetDetector onsetDetector;
    PitchDetector pitchDetector;
    PitchStabilizer pitchStabilizer;
    bool haveQuantizedNote = false;
    float quantizedNoteMidi = 0.0f;
};
