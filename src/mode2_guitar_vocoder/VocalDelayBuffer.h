#pragma once

#include "core/audio/RingBuffer.h"
#include "params/Mode2Params.h"

#include <algorithm>

// 목소리 경로에 짧은 인위적 지연을 둔다(5절 장치 ①): 온셋 검출까지 걸리는 시간만큼만
// 늦춰서, 기타 목표 음정이 확정될 시간을 벌어준다.
class VocalDelayBuffer
{
public:
    void prepare(double sampleRateIn, int /*blockSize*/, float lookaheadSeconds)
    {
        delayInSamples = std::max(1, static_cast<int>(sampleRateIn * lookaheadSeconds + 0.5));
        ringBuffer.setSize(delayInSamples + 1);
    }

    void prepare(double sampleRateIn, int blockSize)
    {
        prepare(sampleRateIn, blockSize, mode2::params::vocalControlLookaheadSeconds);
    }

    void reset() { ringBuffer.clear(); }

    // in-place 호출 가능(output == input).
    void processBlock(const float* input, float* output, int numSamples)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const float delayed = ringBuffer.readDelayed(delayInSamples - 1);
            ringBuffer.push(input[i]);
            output[i] = delayed;
        }
    }

    int getDelaySamples() const { return delayInSamples; }

private:
    RingBuffer<float> ringBuffer;
    int delayInSamples = 0;
};
