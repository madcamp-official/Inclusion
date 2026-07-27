#include "CorrectionCalculator.h"
#include "params/Mode2Params.h"

#include <cmath>

float CorrectionCalculator::computeCorrection(std::optional<float> vocalMidi, std::optional<float> targetMidi, float strength)
{
    if (!targetMidi.has_value() || !vocalMidi.has_value())
        return 0.0f;

    // 목표의 옥타브 정렬은 호출자(Mode2Controller)가 chooseTargetOctaves로 미리 해둔다.
    // 여기서 다시 접으면 매 블록 접기 경계에서 결과가 뒤집히므로 접지 않는다.
    // 정렬을 못 한 경우(목소리 신뢰도 없음)에는 애초에 이 함수가 호출되지 않는다.
    return strength * (*targetMidi - *vocalMidi);
}

float CorrectionCalculator::computePitchClassLockedCorrection(std::optional<float> vocalMidi,
                                                              std::optional<float> targetMidi,
                                                              std::optional<float> previousCorrection,
                                                              int preferredOctaves,
                                                              float strength)
{
    if (! targetMidi.has_value() || ! vocalMidi.has_value())
        return 0.0f;

    // remainder는 결과를 [-6, +6]에 둔다. vocalMidi에 12의 배수가 더해져도 이 값은
    // 바뀌지 않으므로 기본주기/반주기 혼동에 의한 옥타브 오검출이 보정량에 새지 않는다.
    const float pitchClassCorrection = std::remainder(*targetMidi - *vocalMidi, 12.0f);
    const float reference = previousCorrection.value_or(12.0f * static_cast<float>(preferredOctaves));
    float correction = pitchClassCorrection
                       + 12.0f * std::round((reference - pitchClassCorrection) / 12.0f);

    // 보컬이 아주 넓은 음역을 연속 이동하면 같은 출력 옥타브를 지키기 위해 시프트가 계속
    // 커진다. SoundTouch의 깨끗한 동작 범위를 넘을 때만 출력 옥타브를 하나 접는다.
    while (correction > mode2::params::maxUsableShiftSemitones)
        correction -= 12.0f;
    while (correction < -mode2::params::maxUsableShiftSemitones)
        correction += 12.0f;

    return strength * correction;
}

int CorrectionCalculator::chooseTargetOctaves(float targetMidi, float vocalMidi, int currentOctaves)
{
    // 직전 결정을 먼저 시험한다. 히스테리시스만큼 여유를 두고 판단해서, 경계 근처에서
    // 목소리가 조금 흔들리는 것만으로 옥타브가 바뀌지 않게 한다.
    const float heldShift = (targetMidi + 12.0f * static_cast<float>(currentOctaves)) - vocalMidi;
    if (std::abs(heldShift) <= mode2::params::maxUsableShiftSemitones
                               + mode2::params::octaveChoiceHysteresisSemitones)
        return currentOctaves;

    // 여유를 넘어섰으면 최근접 옥타브로 새로 고른다.
    const float rawShift = targetMidi - vocalMidi;
    return -static_cast<int>(std::round(rawShift / 12.0f));
}
