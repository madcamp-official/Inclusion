#include "PitchStabilizer.h"
#include "params/Mode2Params.h"

#include <cmath>

void PitchStabilizer::prepare(double sampleRateIn)
{
    sampleRate = sampleRateIn;
    attackSettleSamples = static_cast<long long>(mode2::params::attackSettleSeconds * sampleRate);
    coastHoldSamples = static_cast<long long>(mode2::params::coastHoldSeconds * sampleRate);
    coastFadeSamples = static_cast<long long>(mode2::params::coastFadeSeconds * sampleRate);
    reset();
}

void PitchStabilizer::reset()
{
    state = State::Idle;
    samplesInState = 0;
    lastValidMidi = 0.0f;
    collectedStableCount = 0;
    haveConfirmedTarget = false;
    confirmedTargetMidi = 0.0f;
}

void PitchStabilizer::enterState(State newState)
{
    state = newState;
    samplesInState = 0;
    if (newState == State::Collect)
        collectedStableCount = 0;
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
    if (onsetDetected && state != State::Attack)
        enterState(State::Attack);

    switch (state)
    {
        case State::Idle:
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
                    confirmedTargetMidi = pitch.midiFloat;
            }
            else
            {
                enterState(State::Coast);
            }
            break;

        case State::Coast:
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
