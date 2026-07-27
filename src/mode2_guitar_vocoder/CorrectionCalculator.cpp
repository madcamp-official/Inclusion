#include "CorrectionCalculator.h"
#include "params/Mode2Params.h"

#include <cmath>

float CorrectionCalculator::computeCorrection(std::optional<float> vocalMidi, std::optional<float> targetMidi, float strength)
{
    if (!targetMidi.has_value() || !vocalMidi.has_value())
        return 0.0f;

    float diffSemitones = *targetMidi - *vocalMidi;

    // 사양서 4절은 옥타브 차이를 검출 오류로 보고 보정을 포기하지만, 실제로는 기타를 치는
    // 음역과 노래하는 음역이 옥타브 단위로 다른 경우가 많아 그러면 보정이 걸리지 않는다.
    // 목표를 부르는 사람의 옥타브로 접으면 같은 음이름에 붙고, 잔차는 ±6반음 이내가 된다.
    if (mode2::params::foldCorrectionToNearestOctave)
        diffSemitones -= 12.0f * std::round(diffSemitones / 12.0f);

    if (std::abs(diffSemitones) > mode2::params::maxCorrectionSemitones)
        return 0.0f;

    return strength * diffSemitones;
}
