#include "PitchStabilizer.h"
#include "params/Mode2Params.h"

#include <algorithm>
#include <cmath>

void PitchStabilizer::prepare(double sampleRateIn)
{
    sampleRate = sampleRateIn;
    attackSettleSamples = static_cast<long long>(mode2::params::attackSettleSeconds * sampleRate);
    coastHoldSamples = static_cast<long long>(mode2::params::coastHoldSeconds * sampleRate);
    coastFadeSamples = static_cast<long long>(mode2::params::coastFadeSeconds * sampleRate);
    stableReacquireSamples = std::max(
        1LL, static_cast<long long>(mode2::params::stableReacquireSeconds * sampleRate));
    reset();
}

void PitchStabilizer::reset()
{
    state = State::Idle;
    samplesInState = 0;
    lastValidMidi = 0.0f;
    collectedStableCount = 0;
    resetStableDeviationCandidate();
    haveConfirmedTarget = false;
    confirmedTargetMidi = 0.0f;
}

void PitchStabilizer::enterState(State newState)
{
    state = newState;
    samplesInState = 0;
    if (newState == State::Collect)
        collectedStableCount = 0;
    if (newState == State::Stable)
        resetStableDeviationCandidate();
}

void PitchStabilizer::resetStableDeviationCandidate()
{
    stableDeviationSamples = 0;
    haveStableDeviationCandidate = false;
    stableDeviationCandidateMidi = 0.0f;
}

float PitchStabilizer::computeFadeGain() const
{
    switch (state)
    {
        case State::Idle:
            return 0.0f;

        case State::Attack:
        case State::Collect:
            // 새 노트로 재조준 중에도, 직전에 확정된 타겟이 있으면 끊기지 않도록 유지한다.
            return haveConfirmedTarget ? 1.0f : 0.0f;

        case State::Stable:
            return 1.0f;

        case State::Coast:
            if (samplesInState <= coastHoldSamples)
                return 1.0f;
            if (coastFadeSamples <= 0)
                return 0.0f;
            {
                const long long intoFade = samplesInState - coastHoldSamples;
                const float t = static_cast<float>(intoFade) / static_cast<float>(coastFadeSamples);
                return std::max(0.0f, 1.0f - t);
            }
    }
    return 0.0f;
}

PitchStabilizer::Output PitchStabilizer::processBlock(bool onsetDetected, const PitchDetector::Result& pitch, int numSamples)
{
    // 연주가 이미 안정된 상태에서 발생하는 온셋은 옆 줄 스침이나 프렛 잡음일 수 있다.
    // STABLE의 목표는 피치가 실제로 일정 시간 바뀐 경우에만 아래 분기에서 재조준한다.
    if (onsetDetected && (state == State::Idle || state == State::Coast))
        enterState(State::Attack);

    switch (state)
    {
        case State::Idle:
            // 온셋 없이도 유효한 피치가 들어오면 목표 수집을 시작한다. 온셋 검출은 적응 임계가
            // 이미 울리는 소리 위로 올라가 있어서 지속음·레가토·부드러운 코드 전환에서 자주
            // 놓친다. IDLE에서 나가는 길이 온셋뿐이면 그런 순간에 목표를 영구히 잡지 못하고
            // hasTarget=false로 남아 보정이 0이 되어 원본 목소리가 그대로 나간다.
            if (pitch.valid)
                enterState(State::Collect);
            break;

        case State::Attack:
            samplesInState += numSamples;
            if (samplesInState >= attackSettleSamples)
                enterState(State::Collect);
            break;

        case State::Collect:
            samplesInState += numSamples;
            if (pitch.valid)
            {
                if (collectedStableCount == 0
                    || std::abs(pitch.midiFloat - lastValidMidi) <= mode2::params::collectConvergenceToleranceSemitones)
                {
                    ++collectedStableCount;
                }
                else
                {
                    collectedStableCount = 1;
                }
                lastValidMidi = pitch.midiFloat;

                if (collectedStableCount >= mode2::params::collectRequiredStableBlocks)
                {
                    confirmedTargetMidi = lastValidMidi;
                    haveConfirmedTarget = true;
                    enterState(State::Stable);
                }
            }
            break;

        case State::Stable:
            if (pitch.valid)
            {
                if (std::abs(pitch.midiFloat - confirmedTargetMidi) <= mode2::params::stableToleranceSemitones)
                {
                    confirmedTargetMidi = pitch.midiFloat;
                    resetStableDeviationCandidate();
                }
                else
                {
                    const bool sameCandidate =
                        haveStableDeviationCandidate
                        && std::abs(pitch.midiFloat - stableDeviationCandidateMidi)
                               <= mode2::params::collectConvergenceToleranceSemitones;

                    if (!sameCandidate)
                    {
                        haveStableDeviationCandidate = true;
                        stableDeviationCandidateMidi = pitch.midiFloat;
                        stableDeviationSamples = numSamples;
                    }
                    else
                    {
                        stableDeviationCandidateMidi = pitch.midiFloat;
                        stableDeviationSamples += numSamples;
                    }

                    if (stableDeviationSamples >= stableReacquireSamples)
                    {
                        // 온셋 없이도 실제 음이 바뀔 수 있다. 같은 후보가 충분히 유지된 경우에만
                        // 재조준하고, 그동안은 직전 확정 목표를 유지해 스침 잡음에 흔들리지 않는다.
                        lastValidMidi = stableDeviationCandidateMidi;
                        enterState(State::Collect);
                    }
                }
            }
            else
            {
                enterState(State::Coast);
            }
            break;

        case State::Coast:
            if (pitch.valid)
            {
                // 한두 블록의 검출 누락은 흔하다. 신호가 돌아오면 긴 HOLD/FADE를 끝까지
                // 기다리지 말고 직전 목표를 유지한 채 즉시 정상 추적으로 복귀한다.
                enterState(State::Stable);
                break;
            }
            samplesInState += numSamples;
            if (samplesInState > coastHoldSamples + coastFadeSamples)
            {
                haveConfirmedTarget = false;
                enterState(State::Idle);
            }
            break;
    }

    Output out;
    out.state = state;
    out.hasTarget = haveConfirmedTarget;
    out.targetMidi = confirmedTargetMidi;
    out.fadeGain = computeFadeGain();
    return out;
}
