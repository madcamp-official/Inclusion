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
    //
    // octaveChangeConfirmed: 직전 목표에서 정확히 ±12·24반음 떨어진 검출을 되접지 않고
    // 그대로 받아들일지. 오검출과 진짜 옥타브 이동을 구분할 방법이 이 함수 안에는 없으므로,
    // "같은 옥타브 후보가 충분히 오래 유지됐는가"는 호출자(processBlock)가 판단해서 넘긴다.
    static float quantizeMidiWithHysteresis(float midi, bool havePreviousNote, float previousNote,
                                            bool octaveChangeConfirmed = false);

    // 이 검출이 위의 옥타브 되접기 대상인가. 지속 시간을 세는 쪽과 되접는 쪽이 같은 조건을
    // 봐야 하므로 한 곳에 둔다.
    static bool isOctaveFoldCandidate(float midi, float previousNote);

private:
    OnsetDetector onsetDetector;
    PitchDetector pitchDetector;
    PitchStabilizer pitchStabilizer;
    bool haveQuantizedNote = false;
    float quantizedNoteMidi = 0.0f;

    double sampleRate = 48000.0;

    // 뮤트 판정용 상대 게이트: 최근 피크를 느리게 따라가며 현재 RMS와의 비율을 본다.
    float guitarLevelPeak = 0.0f;

    // 옥타브 되접기 후보가 유지된 시간. 서로 다른 옥타브 후보는 누적하지 않는다.
    long long octaveCandidateSamples = 0;
    long long octaveConfirmSamples = 0;
    bool haveOctaveCandidate = false;
    float octaveCandidateMidi = 0.0f;

    void resetOctaveCandidate();
};
