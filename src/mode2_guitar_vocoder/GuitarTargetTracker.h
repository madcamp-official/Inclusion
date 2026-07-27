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

private:
    OnsetDetector onsetDetector;
    PitchDetector pitchDetector;
    PitchStabilizer pitchStabilizer;
};
