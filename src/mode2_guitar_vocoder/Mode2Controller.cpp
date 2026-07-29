#include "Mode2Controller.h"
#include "CorrectionCalculator.h"
#include "params/Mode2Params.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>

// 블록 단위 피치 진단 로그. 소리 문제를 쫓을 때만 켠다(-DMODE2_DIAGNOSTIC_LOG=1).
// 평소에 켜두면 오디오 스레드에서 매 블록 문자열을 만들게 되고, 오프라인 도구 출력도 묻힌다.
#ifndef MODE2_DIAGNOSTIC_LOG
 #define MODE2_DIAGNOSTIC_LOG 0
#endif

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

    bleedCanceller.prepare(sampleRate);
    guitarTracker.prepare(sampleRate);
    vocalPitchDetector.prepare(sampleRate, mode2::params::vocalPitchMinFrequencyHz,
                               mode2::params::vocalPitchMaxFrequencyHz,
                               mode2::params::scalePitchWindowForSampleRate(
                                   mode2::params::vocalPitchWindowSize, sampleRate));
    vocalDelayBuffer.prepare(sampleRate, maxBlockSize, vocalControlLookaheadSeconds);
    pitchShifter.prepare(sampleRate, maxBlockSize);
    feedbackCalibrator.prepare(sampleRate);
    notchSuppressor.prepare(sampleRate, maxBlockSize);
    noiseGate.prepare(sampleRate, maxBlockSize);

    outputTripHoldBlocksTotal = std::max(1, static_cast<int>(mode2::params::outputTripHoldSeconds * sampleRate
                                                             / std::max(1, maxBlockSize)));

    const double blocksPerSecond = sampleRate / std::max(1, maxBlockSize);
    vocalHoldBlocksTotal = std::max(1, static_cast<int>(mode2::params::vocalConfidenceHoldSeconds * blocksPerSecond));
    const double fadeBlocks = mode2::params::vocalConfidenceFadeSeconds * blocksPerSecond;
    vocalFadePerBlock = fadeBlocks > 1.0 ? static_cast<float>(1.0 / fadeBlocks) : 1.0f;

    vocalPitchHistory.assign(static_cast<size_t>(std::max(1, mode2::params::vocalPitchMedianTaps)), 0.0f);
    vocalPitchSortScratch.reserve(vocalPitchHistory.size());
    vocalPitchHistoryCount = 0;
    vocalPitchHistoryWrite = 0;
    timbrePresenceLowpassCoeff =
        1.0f - std::exp(-2.0f * juce::MathConstants<float>::pi
                        * mode2::params::timbrePresenceCutoffHz
                        / static_cast<float>(sampleRate));

    delayedVocalScratch.assign(static_cast<size_t>(maxBlockSize), 0.0f);
    shiftedVocalScratch.assign(static_cast<size_t>(maxBlockSize), 0.0f);
    boostedVocalScratch.assign(static_cast<size_t>(maxBlockSize), 0.0f);
    cleanVocalScratch.assign(static_cast<size_t>(maxBlockSize), 0.0f);
}

float Mode2Controller::medianVocalMidi(float newMidi)
{
    const int taps = static_cast<int>(vocalPitchHistory.size());
    if (taps <= 0)
        return newMidi;

    vocalPitchHistory[static_cast<size_t>(vocalPitchHistoryWrite)] = newMidi;
    vocalPitchHistoryWrite = (vocalPitchHistoryWrite + 1) % taps;
    vocalPitchHistoryCount = std::min(taps, vocalPitchHistoryCount + 1);

    // 아직 다 채워지지 않았으면 들어온 만큼만 보고 미디언을 낸다(초반에도 값이 튀지 않게).
    vocalPitchSortScratch.assign(vocalPitchHistory.begin(),
                                 vocalPitchHistory.begin() + vocalPitchHistoryCount);
    const size_t mid = vocalPitchSortScratch.size() / 2;
    std::nth_element(vocalPitchSortScratch.begin(), vocalPitchSortScratch.begin() + mid,
                     vocalPitchSortScratch.end());
    return vocalPitchSortScratch[mid];
}

void Mode2Controller::reset()
{
    feedbackDuckGain = 1.0f;
    outputLevelEma = 0.0f;
    previousOutputRms = 0.0f;
    outputTripGain = 1.0f;
    outputTripHoldBlocksRemaining = 0;
    timbrePresenceLowpass = 0.0f;
    noiseGate.reset();

    lastCorrectionSemitones = 0.0f;
    haveVocalCorrection = false;
    vocalWetGain = 0.0f;
    vocalHoldBlocksRemaining = 0;

    vocalPitchHistoryCount = 0;
    vocalPitchHistoryWrite = 0;
    std::fill(vocalPitchHistory.begin(), vocalPitchHistory.end(), 0.0f);
    bleedCanceller.reset();
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

    if (static_cast<int>(boostedVocalScratch.size()) < numSamples)
        boostedVocalScratch.resize(static_cast<size_t>(numSamples), 0.0f);
    if (static_cast<int>(cleanVocalScratch.size()) < numSamples)
        cleanVocalScratch.resize(static_cast<size_t>(numSamples), 0.0f);

    // 목소리 마이크에 새어 들어온 기타를 먼저 지운다. 부스트 "전"에 거는 이유는, 부스트 뒤에
    // 걸면 사용자가 슬라이더를 움직일 때마다 학습한 경로 이득이 어긋나 다시 수렴해야 하기 때문.
    bleedCanceller.processBlock(vocalIn, guitarIn, cleanVocalScratch.data(), numSamples);

    // 목소리 입력이 약한 장치를 위해 먼저 부스트한다.
    const float inputGain = vocalInputGain.load();
    for (int i = 0; i < numSamples; ++i)
        boostedVocalScratch[static_cast<size_t>(i)] = cleanVocalScratch[static_cast<size_t>(i)] * inputGain;

    // 제어 경로는 현재(지연 전) 목소리를 분석하고, 오디오 경로만 분석창의 반 길이만큼
    // 늦춘다. 그러면 피치 추정값이 나타내는 창 중앙 시점과 시프터에 들어갈 음성 시점이
    // 맞는다. 둘 다 delayedVocalScratch를 쓰면 인위적 지연 뒤에 분석 지연이 다시 붙는다.
    const PitchDetector::Result vocalPitch =
        vocalPitchDetector.processBlock(boostedVocalScratch.data(), numSamples);
    vocalDelayBuffer.processBlock(boostedVocalScratch.data(), delayedVocalScratch.data(), numSamples);

    const bool vocalConfident = vocalPitch.valid && vocalPitch.confidence >= mode2::params::vocalConfidenceGateThreshold;

    // 검출 지터를 미디언으로 걸러낸 값으로 보정량을 계산한다. 이걸 거치지 않으면 신뢰도가
    // 0.9 이상이어도 매 블록 몇 반음씩 튀는 추정치가 그대로 출력 음정을 흔든다.
    // 절대 출력 옥타브를 고정하려면 실제 보컬의 옥타브를 그대로 알아야 한다. 예전에는
    // ±12반음 변화를 검출 오류로 간주해 직전 옥타브로 되접었는데, 그러면 사용자가 정말
    // 한 옥타브 바꿔 부른 경우도 숨겨져 출력이 목소리를 따라 이동했다. 3탭 미디언으로
    // 단발성 오류만 제거하고 지속되는 옥타브 변화는 그대로 보정 계산에 반영한다.
    const float smoothedVocalMidi = vocalConfident ? medianVocalMidi(vocalPitch.midiFloat)
                                                   : vocalPitch.midiFloat;
    const std::optional<float> vocalMidiOpt =
        vocalConfident ? std::optional<float>(smoothedVocalMidi) : std::nullopt;
    const std::optional<float> targetMidiOpt =
        guitarOut.hasTarget ? std::optional<float>(guitarOut.targetMidi) : std::nullopt;

    // 출력의 절대 목표 음높이를 기타 MIDI + 사용자가 고른 옥타브로 고정한다. 예를 들어
    // 기타 E3, 옥타브 0이면 보컬이 E2로 내려가거나 E4로 올라가도 출력은 항상 E3이다.
    // 예전의 pitch-class 고정 방식은 보정량만 같은 분기에 유지해서, 보컬이 한 옥타브
    // 바뀌면 출력도 그대로 한 옥타브 따라 움직이는 문제가 있었다.
    const int requestedTargetOctaveShift = targetOctaveShift.load();

    // M2-4: 기타 목표를 잃었거나(guitarOut.fadeGain) 목소리 검출 신뢰도가 낮으면 시프트된
    // 신호 대신 드라이 목소리 쪽으로 감쇠한다.
    //
    // 신뢰도를 wet 비율로 환산하면(비례 블렌드) 자음·숨·음 전환처럼 신뢰도가 순간적으로
    // 떨어지는 구간마다 보정 안 된 원본이 그대로 새어 나온다. 원본은 내가 부른 음이라 기타
    // 음에 안 맞으므로 "내 목소리 음이 남는" 소리가 된다. 그래서 기타 쪽 상태기계와 같은
    // HOLD/FADE로 처리한다: 신뢰도를 확보한 동안은 보정을 갱신하며 완전 wet을 유지하고,
    // 신뢰도를 잃으면 직전 보정을 그대로 물린 채 hold 시간을 버틴 뒤에야 드라이로 fade 한다.
    // 부수 효과로, 시프터 내부 지연 때문에 시프트 경로와 드라이 경로는 시간이 어긋나
    // 있는데(콤 필터링) wet이 1에 붙어 있으면 그 간섭도 함께 사라진다.
    float correctionSemitones = 0.0f;
    if (vocalConfident && targetMidiOpt.has_value())
    {
        const float absoluteTargetMidi =
            *targetMidiOpt + 12.0f * static_cast<float>(requestedTargetOctaveShift);
        correctionSemitones = CorrectionCalculator::computeCorrection(
            vocalMidiOpt, absoluteTargetMidi, mode2::params::pitchFollowStrength);
        lastCorrectionSemitones = correctionSemitones;
        haveVocalCorrection = true;
        vocalHoldBlocksRemaining = vocalHoldBlocksTotal;
        vocalWetGain = 1.0f;
    }
    else if (haveVocalCorrection)
    {
        // 직전 보정을 유지한다. 여기서 0으로 되돌리면 원본 음정이 그대로 튀어나온다.
        correctionSemitones = lastCorrectionSemitones;

        if constexpr (mode2::params::vocalConfidenceFallbackToDry)
        {
            if (vocalHoldBlocksRemaining > 0)
                --vocalHoldBlocksRemaining;
            else
                vocalWetGain = std::max(0.0f, vocalWetGain - vocalFadePerBlock);

            if (vocalWetGain <= 0.0f)
                haveVocalCorrection = false;
        }
        else
        {
            // 드라이로 되돌리지 않는다. 신뢰도가 떨어지는 실제 원인은 대부분 무성음(자음·숨)이고
            // 무성음에는 음정이 없어서 직전 보정으로 시프트한 채 내보내도 어색하지 않다. 반면
            // 드라이는 내가 부른 음이라 기타 음에 맞지 않아, 그대로 "틀린 음"으로 들린다.
            // 목소리가 정말로 멈추면 노이즈 게이트가 닫아준다. 자세한 근거는 Mode2Params.h의
            // vocalConfidenceFallbackToDry 주석 참고.
            vocalWetGain = 1.0f;
        }
    }
    else
    {
        vocalWetGain = 0.0f;
    }

    // 드라이 복귀도 시프터를 우회한 원본을 섞지 않고, 같은 처리 경로에서 보정량만
    // 0으로 내려 보낸다. 시프터 출력은 입력보다 늦기 때문에 아래처럼
    //   shifted * wet + delayedDry * (1-wet)
    // 를 하면 전환 구간에서 서로 다른 시점의 목소리 두 개가 겹쳐 콤 필터링과 이중 음정이
    // 생긴다. 0-semitone 시프터 출력은 음정상 드라이지만 처리 지연은 wet과 같으므로
    // 한 경로만 유지하면 그 문제가 없다.
    // guitarOut.fadeGain은 오디오 게인이 아니라 아래에서 **보정 반음 수**에 곱해진다. 그래서
    // 목표를 잃고 페이드가 걸리면 소리가 잦아드는 게 아니라 음정이 목표에서 원래 목소리로
    // 연속으로 미끄러진다 — 글리산도("위웅")가 된다. 옥타브 이동 -1이면 2옥타브를 훑는다.
    // 목소리 신뢰도 경로가 같은 이유로 이미 드라이 복귀를 끈 것과 같은 판단을 여기에도 적용해,
    // 한번 보정을 잡았으면 페이드로 되돌리지 않고 직전 보정을 그대로 물고 있는다.
    const float guitarFadeGain =
        (! mode2::params::guitarTargetFallbackToDry && haveVocalCorrection)
            ? 1.0f
            : guitarOut.fadeGain;
    const float correctionMix = guitarFadeGain * vocalWetGain;
    const float effectiveCorrectionSemitones = correctionSemitones * correctionMix;

    // WORLD에는 "몇 반음 옮겨라"보다 기타의 절대 F0를 직접 준다. 이 경로에서는 WORLD가
    // 분석한 보컬 F0가 한 옥타브 잘못 잡혀도 합성 F0가 기타 목표를 따라가므로, 같은 기타
    // 음에서 사용자가 노래한 옥타브에 따라 출력 옥타브가 달라지지 않는다.
    float absoluteTargetF0Hz = 0.0f;
    if (guitarOut.hasTarget && haveVocalCorrection && correctionMix >= 0.999f)
    {
        const float absoluteTargetMidi =
            guitarOut.targetMidi + 12.0f * static_cast<float>(requestedTargetOctaveShift);
        absoluteTargetF0Hz =
            440.0f * std::pow(2.0f, (absoluteTargetMidi - 69.0f) / 12.0f);
    }
    pitchShifter.processBlock(delayedVocalScratch.data(), shiftedVocalScratch.data(), numSamples,
                              effectiveCorrectionSemitones, absoluteTargetF0Hz);

    // 하향 시프트로 같이 내려간 자음·포먼트의 존재감을 시프트된 경로 안에서만 복원한다.
    // 원본 목소리를 병렬로 섞지 않으므로 원래 피치가 이중으로 들리거나 시간축이 어긋나는
    // 문제는 없다. 시프트가 작을 때는 보강도 자동으로 줄어 원래 음색을 과하게 밝히지 않는다.
    const float downwardShiftRatio =
        std::clamp(-effectiveCorrectionSemitones
                       / mode2::params::timbrePresenceFullShiftSemitones,
                   0.0f, 1.0f);
    // Rubber Band는 포먼트를 직접 보존하므로 SoundTouch용 고역 보정을 중복 적용하지 않는다.
    const bool needsPresenceCompensation =
        pitchShifter.getActiveBackend() == PitchShifterEngine::Backend::SoundTouch;
    const float presenceAmount = needsPresenceCompensation
        ? mode2::params::timbrePresenceAmount * downwardShiftRatio
        : 0.0f;
    for (int i = 0; i < numSamples; ++i)
    {
        const float sample = shiftedVocalScratch[static_cast<size_t>(i)];
        timbrePresenceLowpass += timbrePresenceLowpassCoeff * (sample - timbrePresenceLowpass);
        const float highBand = sample - timbrePresenceLowpass;
        shiftedVocalScratch[static_cast<size_t>(i)] = sample + presenceAmount * highBand;
    }

#if MODE2_DIAGNOSTIC_LOG
    {
        // 음정 흔들림이 어느 단계에서 생기는지 보기 위한 로깅.
        // tgt(기타 목표) / voc(내 음정) / corr(요청 보정량) / shift(램프 적용 후 실제 시프트).
        // tgt·voc가 흔들리면 검출·안정화 문제, 둘이 안정한데 소리가 흔들리면 시프터 문제다.
        static int callCount = 0;
        if (++callCount % 8 == 0)
        {
            juce::Logger::writeToLog("[PITCH] st=" + juce::String(static_cast<int>(guitarOut.state))
                                     + " tgt=" + (guitarOut.hasTarget ? juce::String(guitarOut.targetMidi, 2) : juce::String("-"))
                                     + " vocRaw=" + (vocalPitch.valid ? juce::String(vocalPitch.midiFloat, 2) : juce::String("-"))
                                     + " vocMed=" + juce::String(smoothedVocalMidi, 2)
                                     + " conf=" + juce::String(vocalPitch.confidence, 2)
                                     + " corr=" + juce::String(correctionSemitones, 2)
                                     + " shift=" + juce::String(pitchShifter.getCurrentShiftSemitones(), 2)
                                     + " wet=" + juce::String(vocalWetGain, 2));
        }
    }
#endif

    const float wetness = correctionMix;

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

    // 항상 같은 시프터 경로 하나만 출력한다. wetness는 이제 오디오 두 개의 혼합률이
    // 아니라 "현재 적용 중인 보정 강도"를 뜻한다.
    std::copy(shiftedVocalScratch.begin(), shiftedVocalScratch.begin() + numSamples, outL);

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
    // 화면도 원시 검출 옥타브가 아니라 실제 제어에 쓴 연속·평활 피치를 보여준다.
    // 그래야 검출기의 ±12반음 후보 전환이 UI에서 가짜 옥타브 점프로 보이지 않는다.
    uiVocalMidi.store(vocalConfident ? smoothedVocalMidi : 0.0f);
    uiCorrectedMidi.store(vocalConfident ? smoothedVocalMidi + effectiveCorrectionSemitones : 0.0f);
    uiAppliedShiftSemitones.store(pitchShifter.getCurrentShiftSemitones());
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

    uiBleedCancelledDb.store(bleedCanceller.getCancelledDb());
    uiBleedCancelAdapting.store(bleedCanceller.isAdapting());

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
    state.appliedShiftSemitones = uiAppliedShiftSemitones.load();
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
    state.bleedCancelledDb = uiBleedCancelledDb.load();
    state.bleedCancelAdapting = uiBleedCancelAdapting.load();
    state.notchCount = uiNotchCount.load();
    for (int i = 0; i < FeedbackNotchSuppressor::maxNotches; ++i)
        state.notchFrequencies[static_cast<size_t>(i)] = uiNotchFrequencies[static_cast<size_t>(i)].load();
    return state;
}
