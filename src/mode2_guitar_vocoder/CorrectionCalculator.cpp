#include "CorrectionCalculator.h"
float CorrectionCalculator::computeCorrection(std::optional<float> vocalMidi, std::optional<float> targetMidi, float strength)
{
    if (!targetMidi.has_value() || !vocalMidi.has_value())
        return 0.0f;

    // targetMidi는 Mode2Controller가 만든 기타의 절대 목표 MIDI다. 여기서 옥타브를
    // 접거나 보컬 근처 분기를 고르면 같은 기타 음의 출력 옥타브가 보컬에 따라 달라진다.
    return strength * (*targetMidi - *vocalMidi);
}
