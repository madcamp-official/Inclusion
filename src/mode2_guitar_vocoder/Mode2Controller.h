#pragma once

#include "core/audio/FeedbackCalibrator.h"
#include "core/audio/FeedbackNotchSuppressor.h"
#include "params/Mode2Params.h"
#include "core/dsp/NoiseGate.h"
#include "core/dsp/PitchDetector.h"
#include "core/dsp/PitchStabilizer.h"
#include "GuitarBleedCanceller.h"
#include "GuitarTargetTracker.h"
#include "PitchShifterEngine.h"
#include "VocalDelayBuffer.h"

#include <array>
#include <atomic>
#include <vector>

// 모드 2 최상위 컨트롤러: 기타 목표 추적, 목소리 지연/피치 검출, 보정량 계산,
// 피치 시프트, 하울링 게인 상한까지 한 블록 분량을 배선한다.
// UI(Mode2Screen)는 getDisplayState()로 오디오 스레드 상태를 폴링한다.
class Mode2Controller
{
public:
    struct DisplayState
    {
        bool hasGuitarTarget = false;
        float guitarTargetMidi = 0.0f;
        bool hasVocalPitch = false;
        float vocalMidi = 0.0f;
        float correctedMidi = 0.0f;
        float appliedShiftSemitones = 0.0f;
        float wetness = 0.0f;
        PitchStabilizer::State guitarState = PitchStabilizer::State::Idle;
        bool calibrating = false;

        // 하드웨어/채널 배선이 맞는지 눈으로 확인할 수 있도록, 피치 검출과 무관하게
        // 실제로 들어오는 원시 입력 레벨(RMS)을 그대로 노출한다.
        float guitarInputLevel = 0.0f;
        // 부스트를 적용한 뒤, 실제로 피치 검출에 들어가는 레벨(슬라이더 효과가 보이도록).
        float vocalInputLevel = 0.0f;
        // 목소리 피치 검출 신뢰도. 게이트 임계에 얼마나 못 미치는지 눈으로 보기 위함.
        float vocalConfidence = 0.0f;

        // 스피커로 실제 나가는 신호 레벨과, 거기에 곱해진 게인. 소리가 안 들릴 때
        // "신호가 없는 것"과 "게인이 눌린 것"을 구분하기 위해 둘 다 노출한다.
        float outputLevel = 0.0f;
        float outputGain = 1.0f;

        // 하울링 자동 억제가 지금 얼마나 게인을 누르고 있는지(1.0 = 억제 없음).
        float feedbackDuckGain = 1.0f;
        // 출력 하울링 트립 게이트: 출력이 기준을 넘으면 즉시 0으로 떨어졌다가 서서히 복귀한다.
        // feedbackDuckGain보다 훨씬 강하고 빠른 마지막 안전장치.
        float outputTripGain = 1.0f;
        bool outputTripActive = false;
        // 노이즈 게이트가 열린 정도(0 = 닫힘)와 열림/닫힘 상태.
        float noiseGateGain = 0.0f;
        bool noiseGateOpen = false;

        // 지금 걸려 있는 하울링 노치 개수와 그 주파수(Hz).
        int notchCount = 0;
        std::array<float, FeedbackNotchSuppressor::maxNotches> notchFrequencies {};

        // 기타 유입 상쇄가 목소리 채널에서 몇 dB를 걷어내고 있는지, 그리고 지금 학습 중인지.
        // 노래하는 동안에는 목소리가 분모에 남아 0에 가깝게 보이므로, 기타만 치고 노래하지
        // 않는 구간에서 읽어야 실제 제거량이 드러난다.
        float bleedCancelledDb = 0.0f;
        bool bleedCancelAdapting = false;
    };

    void prepare(double sampleRateIn, int maxBlockSize);
    void reset();

    // guitarIn/vocalIn: 모노 입력 블록. outL/outR: 스테레오 출력(변조된 목소리 하나를 복제).
    void processBlock(const float* guitarIn, const float* vocalIn, float* outL, float* outR, int numSamples);

    void startCalibration() { feedbackCalibrator.startCalibration(); }

    // 하울링 캘리브레이션이 게인 상한을 과하게 낮게 잠갔을 때 되돌린다.
    void resetCalibration() { feedbackCalibrator.reset(); }

    // 목소리 입력이 약한 장치(내장 마이크 등)를 위한 입력 부스트, 그리고 최종 출력 볼륨.
    void setVocalInputGain(float gain) { vocalInputGain.store(gain); }
    void setOutputVolume(float gain) { outputVolume.store(gain); }

    // 헤드폰으로 모니터링하는 등 되먹임이 없는 환경에서는 억제를 끌 수 있게 한다.
    // 노이즈 게이트는 되먹임과 무관하게 실내 잡음·기타 유입을 막는 용도로도 쓰므로 별개다.
    void setHowlGuardEnabled(bool shouldEnable)
    {
        howlGuardEnabled.store(shouldEnable);
        notchSuppressor.setEnabled(shouldEnable);
    }

    void setNoiseGateEnabled(bool shouldEnable) { noiseGateEnabledRequest.store(shouldEnable); }
    void setNoiseGateThreshold(float rmsThreshold) { noiseGateThresholdRequest.store(rmsThreshold); }

    // 출력 하울링 트립 게이트의 트립 기준(출력 RMS).
    void setOutputTripThreshold(float rmsThreshold) { outputTripThresholdRequest.store(rmsThreshold); }

    // 목표 음을 옥타브 단위로 옮긴다(0 = 기타 음 그대로, -1 = 한 옥타브 아래).
    void setTargetOctaveShift(int octaves) { targetOctaveShift.store(octaves); }

    // 글라이드(포르타멘토) 속도, 반음/초. 낮추면 음 사이를 미끄러져 레가토처럼 들린다.
    void setGlideRate(float semitonesPerSecond) { pitchShifter.setGlideRate(semitonesPerSecond); }

    // 오프라인 A/B용. prepare 전에 설정한다.
    void setVocalControlLookahead(float seconds) { vocalControlLookaheadSeconds = seconds; }
    float getVocalControlLookahead() const { return vocalControlLookaheadSeconds; }
    void setPitchShifterBackend(PitchShifterEngine::Backend backend) { pitchShifter.setBackend(backend); }
    PitchShifterEngine::Backend getPitchShifterBackend() const { return pitchShifter.getActiveBackend(); }
    int getPitchShifterLatencySamples() const { return pitchShifter.getLatencySamples(); }
    double getPitchShifterLatencyMilliseconds() const
    {
        return 1000.0 * static_cast<double>(pitchShifter.getLatencySamples()) / sampleRate;
    }

    // 목소리 마이크에 새어 들어온 기타를 지운다. 기타를 라인(DI)으로 받고 목소리를 근접
    // 마이크로 받는 등 유입이 애초에 없는 환경에서는 꺼도 된다.
    void setBleedCancelEnabled(bool shouldEnable) { bleedCanceller.setEnabled(shouldEnable); }

    DisplayState getDisplayState() const;

private:
    double sampleRate = 44100.0;

    GuitarBleedCanceller bleedCanceller;
    GuitarTargetTracker guitarTracker;
    PitchDetector vocalPitchDetector;
    VocalDelayBuffer vocalDelayBuffer;
    PitchShifterEngine pitchShifter;
    FeedbackCalibrator feedbackCalibrator;
    FeedbackNotchSuppressor notchSuppressor;
    NoiseGate noiseGate;

    std::vector<float> delayedVocalScratch;
    std::vector<float> shiftedVocalScratch;

    std::atomic<bool> uiHasGuitarTarget { false };
    std::atomic<float> uiGuitarTargetMidi { 0.0f };
    std::atomic<bool> uiHasVocalPitch { false };
    std::atomic<float> uiVocalMidi { 0.0f };
    std::atomic<float> uiCorrectedMidi { 0.0f };
    std::atomic<float> uiAppliedShiftSemitones { 0.0f };
    std::atomic<float> uiWetness { 0.0f };
    std::atomic<int> uiGuitarState { 0 };
    std::atomic<float> uiGuitarInputLevel { 0.0f };
    std::atomic<float> uiVocalInputLevel { 0.0f };
    std::atomic<float> uiOutputLevel { 0.0f };
    std::atomic<float> uiOutputGain { 1.0f };
    std::atomic<float> uiVocalConfidence { 0.0f };

    std::atomic<float> vocalInputGain { mode2::params::defaultVocalInputGain };
    std::atomic<float> outputVolume { mode2::params::defaultOutputVolume };
    std::atomic<bool> howlGuardEnabled { mode2::params::defaultHowlGuardEnabled };
    std::atomic<bool> noiseGateEnabledRequest { true };
    std::atomic<float> noiseGateThresholdRequest { mode2::params::vocalNoiseGateThreshold };
    std::atomic<bool> uiNoiseGateOpen { false };
    std::atomic<float> uiFeedbackDuckGain { 1.0f };
    std::atomic<float> uiNoiseGateGain { 0.0f };
    std::atomic<float> uiOutputTripGain { 1.0f };
    std::atomic<bool> uiOutputTripActive { false };
    std::atomic<float> outputTripThresholdRequest { mode2::params::outputTripThreshold };
    std::atomic<int> targetOctaveShift { mode2::params::targetOctaveShift };
    std::atomic<float> uiBleedCancelledDb { 0.0f };
    std::atomic<bool> uiBleedCancelAdapting { false };
    std::atomic<int> uiNotchCount { 0 };
    std::array<std::atomic<float>, FeedbackNotchSuppressor::maxNotches> uiNotchFrequencies {};

    // 오디오 스레드 전용 상태(원자적일 필요 없음).
    float feedbackDuckGain = 1.0f;
    float outputLevelEma = 0.0f;
    float previousOutputRms = 0.0f;
    float outputTripGain = 1.0f;
    int outputTripHoldBlocksRemaining = 0;
    int outputTripHoldBlocksTotal = 1;
    float timbrePresenceLowpass = 0.0f;
    float timbrePresenceLowpassCoeff = 1.0f;

    // 목소리 피치 미디언 필터 — 검출 지터가 보정량으로 새어 들어가 음정이 떨리는 걸 막는다.
    // 유효한 추정치만 넣고, 채워지기 전에는 들어온 값 중에서 미디언을 낸다.
    std::vector<float> vocalPitchHistory;
    int vocalPitchHistoryCount = 0;
    int vocalPitchHistoryWrite = 0;
    std::vector<float> vocalPitchSortScratch;
    float medianVocalMidi(float newMidi);

    // 제어 경로와 오디오 경로의 정렬량. 오프라인 스윕으로 최적값을 찾을 수 있게 런타임에
    // 열어 뒀다(Mode2Offline의 --lookahead). 앱은 측정으로 정한 기본값을 그대로 쓴다.
    float vocalControlLookaheadSeconds = mode2::params::vocalControlLookaheadSeconds;

    // 목소리 신뢰도 HOLD/FADE — 순간적인 신뢰도 하락에 원본 음정이 새어 나오지 않게,
    // 직전 보정과 완전 wet을 유지했다가 지속적으로 잃었을 때만 드라이로 되돌린다.
    float lastCorrectionSemitones = 0.0f;
    bool haveVocalCorrection = false;
    float vocalWetGain = 0.0f;
    int vocalHoldBlocksRemaining = 0;
    int vocalHoldBlocksTotal = 1;
    float vocalFadePerBlock = 1.0f;

    std::vector<float> boostedVocalScratch;
    // 기타 유입을 뺀 목소리(부스트 적용 전).
    std::vector<float> cleanVocalScratch;
};
