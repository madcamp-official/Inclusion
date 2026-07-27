#pragma once

#include <optional>

// 사양서 PART 1 4절 compute_correction 그대로.
class CorrectionCalculator
{
public:
    static float computeCorrection(std::optional<float> vocalMidi, std::optional<float> targetMidi, float strength);

    // 보컬 검출의 절대 옥타브를 믿지 않고 음이름(pitch class) 차이만으로 보정량을 만든다.
    // 같은 음을 ±12/24반음으로 오검출해도 base correction은 완전히 같으며, 직전 보정량과
    // 가장 가까운 옥타브 분기를 골라 실제 보컬이 연속적으로 움직일 때도 출력 목표를 유지한다.
    static float computePitchClassLockedCorrection(std::optional<float> vocalMidi,
                                                   std::optional<float> targetMidi,
                                                   std::optional<float> previousCorrection,
                                                   int preferredOctaves,
                                                   float strength);

    // 목표를 목소리 근처 옥타브로 정렬하기 위해 더할 옥타브 수를 고른다.
    //
    // 매 블록 최근접 옥타브를 새로 고르면, 목표가 목소리에서 6반음의 홀수배만큼 떨어진
    // 경계에 걸터앉았을 때 목소리 지터가 경계를 넘나들며 결과가 12반음씩 뒤집힌다.
    // 그래서 호출자가 직전 결정(currentOctaves)을 보관해 넘겨주고, 그 값으로도 시프트가
    // 사용 가능 범위 + 히스테리시스 안에 들어오면 그대로 유지한다.
    static int chooseTargetOctaves(float targetMidi, float vocalMidi, int currentOctaves);
};
