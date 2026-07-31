#pragma once

#include "params/Mode2Params.h"

#include <atomic>
#include <deque>
#include <memory>
#include <vector>

#if HAVE_RUBBERBAND
#include <rubberband/RubberBandStretcher.h>
#endif

#if HAVE_SOUNDTOUCH
#include <SoundTouch.h>
#endif

#if HAVE_WORLD
#include "WorldRealtimePitchShifter.h"
#endif

// 목소리 피치 시프터. Rubber Band를 기본으로 쓰고, SoundTouch와 WORLD 실험
// 백엔드는 A/B 및 폴백용으로 유지한다.
class PitchShifterEngine
{
public:
    enum class Backend
    {
        World,
        RubberBand,
        RubberBandLowLatency,
        SoundTouch
    };

    void prepare(double sampleRateIn, int maxBlockSize);
    void reset();

    // requestedSemitones: 이번 블록에 원하는 시프트량(CorrectionCalculator 결과).
    // input/output은 모노 버퍼이며 in-place 호출 가능(output == input).
    void processBlock(const float* input, float* output, int numSamples, float requestedSemitones,
                      float absoluteTargetF0Hz = 0.0f);

    float getCurrentShiftSemitones() const { return currentShiftSemitones; }

    // 글라이드(포르타멘토) 속도, 반음/초. 목표가 바뀔 때 그 음까지 미끄러지는 빠르기다.
    // UI 스레드에서 호출되고 오디오 스레드가 읽으므로 atomic이다.
    void setGlideRate(float semitonesPerSecond) { glideSemitonesPerSecond.store(semitonesPerSecond); }

    // prepare/process와 동시에 호출하지 않는다. 앱은 기본 백엔드를 사용하고, 오프라인
    // A/B 도구가 prepare 전에 이 값을 지정한다.
    void setBackend(Backend requested);
    Backend getActiveBackend() const { return activeBackend; }
    int getLatencySamples() const { return latencySamples; }
    static bool isBackendAvailable(Backend backend);
    static const char* getBackendName(Backend backend);

private:
    void processRubberBand(const float* input, float* output, int numSamples);
    void processSoundTouch(const float* input, float* output, int numSamples);

    double sampleRate = 44100.0;
    int latencySamples = 0;
    float currentShiftSemitones = 0.0f;
    std::atomic<float> glideSemitonesPerSecond { mode2::params::defaultGlideSemitonesPerSecond };

#if HAVE_RUBBERBAND
    std::unique_ptr<RubberBand::RubberBandStretcher> rubberBand;
    std::deque<float> rubberBandOutputQueue;
    std::vector<float> rubberBandReceiveScratch;
    std::vector<float> rubberBandSilentPad;
    size_t rubberBandSamplesToDiscard = 0;
    size_t rubberBandStartDelaySamples = 0;
    size_t rubberBandSafetyDelaySamples = 0;
    int rubberBandRecoveryFadeSamples = 0;
    float lastRubberBandOutputSample = 0.0f;
#endif

#if HAVE_SOUNDTOUCH
    soundtouch::SoundTouch soundTouch;
    // SoundTouch는 WSOLA 처리 특성상 넣은 만큼 즉시 나오지 않으므로(초기 지연 존재),
    // 받은 샘플을 큐에 모아뒀다가 블록 크기만큼만 꺼내 쓴다.
    std::deque<float> soundTouchOutputQueue;
    std::vector<float> soundTouchReceiveScratch;
#endif

#if HAVE_WORLD
    WorldRealtimePitchShifter world;
#endif

#if HAVE_RUBBERBAND
    Backend activeBackend = Backend::RubberBand;
#elif HAVE_SOUNDTOUCH
    Backend activeBackend = Backend::SoundTouch;
#else
    Backend activeBackend = Backend::World;
#endif
};
