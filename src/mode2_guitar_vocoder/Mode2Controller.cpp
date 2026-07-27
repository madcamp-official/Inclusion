#include "Mode2Controller.h"
#include "CorrectionCalculator.h"
#include "params/Mode2Params.h"

#include <algorithm>
#include <cmath>

namespace
{
    float computeRms(const float* samples, int numSamples)
    {
        if (numSamples <= 0)
            return 0.0f;

        double sumSquares = 0.0;
        for (int i = 0; i < numSamples; ++i)
            sumSquares += static_cast<double>(samples[i]) * static_cast<double>(samples[i]);

        return static_cast<float>(std::sqrt(sumSquares / static_cast<double>(numSamples)));
    }
}

void Mode2Controller::prepare(double sampleRateIn, int maxBlockSize)
{
    sampleRate = sampleRateIn;

    guitarTracker.prepare(sampleRate);
    vocalPitchDetector.prepare(sampleRate);
    vocalDelayBuffer.prepare(sampleRate, maxBlockSize);
    pitchShifter.prepare(sampleRate, maxBlockSize);
    feedbackCalibrator.prepare(sampleRate);
    notchSuppressor.prepare(sampleRate, maxBlockSize);
    noiseGate.prepare(sampleRate, maxBlockSize);

    outputTripHoldBlocksTotal = std::max(1, static_cast<int>(mode2::params::outputTripHoldSeconds * sampleRate
                                                             / std::max(1, maxBlockSize)));

    delayedVocalScratch.assign(static_cast<size_t>(maxBlockSize), 0.0f);
    shiftedVocalScratch.assign(static_cast<size_t>(maxBlockSize), 0.0f);
    boostedVocalScratch.assign(static_cast<size_t>(maxBlockSize), 0.0f);
}

void Mode2Controller::reset()
{
    feedbackDuckGain = 1.0f;
    outputLevelEma = 0.0f;
    previousOutputRms = 0.0f;
    outputTripGain = 1.0f;
    outputTripHoldBlocksRemaining = 0;
    noiseGate.reset();

    guitarTracker.reset();
    vocalPitchDetector.reset();
    vocalDelayBuffer.reset();
    pitchShifter.reset();
    feedbackCalibrator.reset();
    notchSuppressor.reset();
}

void Mode2Controller::processBlock(const float* guitarIn, const float* vocalIn, float* outL, float* outR, int numSamples)
{
    const GuitarTargetTracker::Output guitarOut = guitarTracker.processBlock(guitarIn, numSamples);

    // 목소리 입력이 약한 장치를 위해 먼저 부스트한 뒤 지연·피치검출·시프트에 태운다.
    const float inputGain = vocalInputGain.load();
    if (static_cast<int>(boostedVocalScratch.size()) < numSamples)
        boostedVocalScratch.resize(static_cast<size_t>(numSamples), 0.0f);
    for (int i = 0; i < numSamples; ++i)
        boostedVocalScratch[static_cast<size_t>(i)] = vocalIn[i] * inputGain;

    vocalDelayBuffer.processBlock(boostedVocalScratch.data(), delayedVocalScratch.data(), numSamples);
    const PitchDetector::Result vocalPitch = vocalPitchDetector.processBlock(delayedVocalScratch.data(), numSamples);

    const bool vocalConfident = vocalPitch.valid && vocalPitch.confidence >= mode2::params::vocalConfidenceGateThreshold;
    const std::optional<float> vocalMidiOpt = vocalConfident ? std::optional<float>(vocalPitch.midiFloat) : std::nullopt;
    const std::optional<float> targetMidiOpt = guitarOut.hasTarget ? std::optional<float>(guitarOut.targetMidi) : std::nullopt;

    const float correctionSemitones = CorrectionCalculator::computeCorrection(vocalMidiOpt, targetMidiOpt, mode2::params::pitchFollowStrength);

    pitchShifter.processBlock(delayedVocalScratch.data(), shiftedVocalScratch.data(), numSamples, correctionSemitones);

    // M2-4: 기타 목표를 잃었거나(guitarOut.fadeGain) 목소리 검출 신뢰도가 낮으면
    // 시프트된 신호 대신 드라이 목소리 쪽으로 감쇠한다. 별도의 신뢰도 전용 상태기계 없이
    // confidence를 연속값으로 곱해 같은 효과를 낸다(이번 구현 범위 내 단순화).
    const float wetness = guitarOut.fadeGain * (vocalPitch.valid ? std::clamp(vocalPitch.confidence, 0.0f, 1.0f) : 0.0f);

    float testGain = 1.0f;
    feedbackCalibrator.processBlock(vocalIn, numSamples, testGain);
    const float calibratedCeiling = feedbackCalibrator.isCalibrating() ? testGain : feedbackCalibrator.getMasterGainCeiling();

    const bool guardOn = howlGuardEnabled.load();

    // 노이즈 게이트는 하울링 억제와 독립적으로 동작한다(헤드폰 환경에서도 실내 잡음·기타
    // 유입을 막는 데 쓰기 때문). 판정은 부스트 후 목소리 레벨로, 적용은 출력 신호에 한다.
    noiseGate.setEnabled(noiseGateEnabledRequest.load());
    noiseGate.setThreshold(noiseGateThresholdRequest.load());
    const float boostedRms = computeRms(boostedVocalScratch.data(), numSamples);

    // 하울링 자동 억제: 출력이 계속 높게 유지되면 되먹임으로 보고 빠르게 누르고,
    // 잠잠해지면 천천히 회복한다(7절 조기 경보 신호를 그대로 동작으로 옮긴 것).
    if (guardOn)
    {
        if (outputLevelEma > mode2::params::feedbackDuckOutputThreshold)
            feedbackDuckGain = std::max(mode2::params::feedbackDuckMinimumGain,
                                        feedbackDuckGain * mode2::params::feedbackDuckAttackPerBlock);
        else
            feedbackDuckGain = std::min(1.0f, feedbackDuckGain + mode2::params::feedbackDuckRecoveryPerBlock);
    }
    else
    {
        feedbackDuckGain = 1.0f;
    }

    // 출력 하울링 트립 게이트: feedbackDuck보다 훨씬 강한 마지막 수단이다. 직전 블록의
    // 출력이 기준을 넘었으면 이번 블록을 즉시 완전 묵음시키고, 유지 시간이 끝날 때까지는
    // 레벨이 내려가도 계속 묵음을 유지한 뒤(바로 풀면 되먹임이 곧장 다시 차오른다)
    // 서서히 게인을 되돌린다.
    if (guardOn)
    {
        if (previousOutputRms > outputTripThresholdRequest.load())
        {
            outputTripGain = 0.0f;
            outputTripHoldBlocksRemaining = outputTripHoldBlocksTotal;
        }
        else if (outputTripHoldBlocksRemaining > 0)
        {
            --outputTripHoldBlocksRemaining;
        }
        else
        {
            outputTripGain = std::min(1.0f, outputTripGain + mode2::params::outputTripRecoveryPerBlock);
        }
    }
    else
    {
        outputTripGain = 1.0f;
        outputTripHoldBlocksRemaining = 0;
    }

    const float outputGain = calibratedCeiling * outputVolume.load() * feedbackDuckGain * outputTripGain;

    for (int i = 0; i < numSamples; ++i)
    {
        outL[i] = shiftedVocalScratch[static_cast<size_t>(i)] * wetness
                  + delayedVocalScratch[static_cast<size_t>(i)] * (1.0f - wetness);
    }

    // 하울링 주파수만 좁게 깎는다. 게인을 통째로 누르는 것보다 음색 손상이 훨씬 적다.
    notchSuppressor.processBlock(outL, numSamples);

    // 게이트는 샘플 단위로 어택/릴리스를 적용하므로 여기서 신호에 직접 걸린다.
    noiseGate.processBlock(outL, numSamples, boostedRms);

    for (int i = 0; i < numSamples; ++i)
    {
        // 어떤 경우에도 폭주하지 않도록 마지막에 리미터를 건다.
        const float sample = std::clamp(outL[i] * outputGain,
                                        -mode2::params::outputLimitPeak,
                                        mode2::params::outputLimitPeak);
        outL[i] = sample;
        outR[i] = sample;
    }

    const float outRms = computeRms(outL, numSamples);
    outputLevelEma += mode2::params::outputLevelEmaCoeff * (outRms - outputLevelEma);
    previousOutputRms = outRms;

    uiHasGuitarTarget.store(guitarOut.hasTarget);
    uiGuitarTargetMidi.store(guitarOut.targetMidi);
    uiHasVocalPitch.store(vocalConfident);
    uiVocalMidi.store(vocalPitch.valid ? vocalPitch.midiFloat : 0.0f);
    uiCorrectedMidi.store((vocalPitch.valid ? vocalPitch.midiFloat : 0.0f) + correctionSemitones);
    uiWetness.store(wetness);
    uiGuitarState.store(static_cast<int>(guitarOut.state));
    uiGuitarInputLevel.store(computeRms(guitarIn, numSamples));
    uiVocalInputLevel.store(computeRms(boostedVocalScratch.data(), numSamples));
    uiVocalConfidence.store(vocalPitch.valid ? vocalPitch.confidence : 0.0f);
    uiOutputLevel.store(outRms);
    uiOutputGain.store(outputGain);
    uiFeedbackDuckGain.store(feedbackDuckGain);
    uiNoiseGateGain.store(noiseGate.getCurrentGain());
    uiNoiseGateOpen.store(noiseGate.isOpen());
    uiOutputTripGain.store(outputTripGain);
    uiOutputTripActive.store(outputTripHoldBlocksRemaining > 0 || outputTripGain < 0.999f);

    const int notchCount = notchSuppressor.getActiveNotchCount();
    uiNotchCount.store(notchCount);
    for (int i = 0; i < FeedbackNotchSuppressor::maxNotches; ++i)
        uiNotchFrequencies[static_cast<size_t>(i)].store(notchSuppressor.getActiveNotchFrequency(i));
}

Mode2Controller::DisplayState Mode2Controller::getDisplayState() const
{
    DisplayState state;
    state.hasGuitarTarget = uiHasGuitarTarget.load();
    state.guitarTargetMidi = uiGuitarTargetMidi.load();
    state.hasVocalPitch = uiHasVocalPitch.load();
    state.vocalMidi = uiVocalMidi.load();
    state.correctedMidi = uiCorrectedMidi.load();
    state.wetness = uiWetness.load();
    state.guitarState = static_cast<PitchStabilizer::State>(uiGuitarState.load());
    state.calibrating = feedbackCalibrator.isCalibrating();
    state.guitarInputLevel = uiGuitarInputLevel.load();
    state.vocalInputLevel = uiVocalInputLevel.load();
    state.outputLevel = uiOutputLevel.load();
    state.outputGain = uiOutputGain.load();
    state.vocalConfidence = uiVocalConfidence.load();
    state.feedbackDuckGain = uiFeedbackDuckGain.load();
    state.noiseGateGain = uiNoiseGateGain.load();
    state.noiseGateOpen = uiNoiseGateOpen.load();
    state.outputTripGain = uiOutputTripGain.load();
    state.outputTripActive = uiOutputTripActive.load();
    state.notchCount = uiNotchCount.load();
    for (int i = 0; i < FeedbackNotchSuppressor::maxNotches; ++i)
        state.notchFrequencies[static_cast<size_t>(i)] = uiNotchFrequencies[static_cast<size_t>(i)].load();
    return state;
}
