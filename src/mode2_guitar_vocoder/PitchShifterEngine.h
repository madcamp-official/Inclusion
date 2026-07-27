#pragma once

#include "params/Mode2Params.h"

#include <atomic>
#include <vector>

#ifdef HAVE_SOUNDTOUCH
#include <SoundTouch.h>
#include <deque>
#endif

// 목소리 피치 시프터. HAVE_SOUNDTOUCH가 정의되어 있으면(=external/soundtouch가 실제로
// 채워져 CMake가 링크했으면) SoundTouch 백엔드를 쓰고, 아니면 시프트 없이 드라이로
// 통과시키는 스텁으로 동작한다. 어느 쪽이든 목표 시프트량 변경 시 급격한 점프를 막기
// 위해 시프트 램프를 적용한다.
class PitchShifterEngine
{
public:
    void prepare(double sampleRateIn, int maxBlockSize);
    void reset();

    // requestedSemitones: 이번 블록에 원하는 시프트량(CorrectionCalculator 결과).
    // input/output은 모노 버퍼이며 in-place 호출 가능(output == input).
    void processBlock(const float* input, float* output, int numSamples, float requestedSemitones);

    float getCurrentShiftSemitones() const { return currentShiftSemitones; }

    // 글라이드(포르타멘토) 속도, 반음/초. 목표가 바뀔 때 그 음까지 미끄러지는 빠르기다.
    // UI 스레드에서 호출되고 오디오 스레드가 읽으므로 atomic이다.
    void setGlideRate(float semitonesPerSecond) { glideSemitonesPerSecond.store(semitonesPerSecond); }

private:
    double sampleRate = 44100.0;
    float currentShiftSemitones = 0.0f;
    std::atomic<float> glideSemitonesPerSecond { mode2::params::defaultGlideSemitonesPerSecond };

#ifdef HAVE_SOUNDTOUCH
    soundtouch::SoundTouch soundTouch;
    // SoundTouch는 WSOLA 처리 특성상 넣은 만큼 즉시 나오지 않으므로(초기 지연 존재),
    // 받은 샘플을 큐에 모아뒀다가 블록 크기만큼만 꺼내 쓴다.
    std::deque<float> outputQueue;
    std::vector<float> receiveScratch;
#endif
};
