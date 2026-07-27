#pragma once

#include <vector>

// 고정 크기 원형 버퍼. VocalDelayBuffer 등 고정 지연선 구현에 재사용한다.
template <typename SampleType>
class RingBuffer
{
public:
    void setSize(int numSamples)
    {
        buffer.assign(static_cast<size_t>(numSamples), SampleType{});
        writeIndex = 0;
    }

    void clear()
    {
        std::fill(buffer.begin(), buffer.end(), SampleType{});
        writeIndex = 0;
    }

    int size() const { return static_cast<int>(buffer.size()); }

    void push(SampleType sample)
    {
        const int n = size();
        if (n == 0)
            return;
        buffer[static_cast<size_t>(writeIndex)] = sample;
        writeIndex = (writeIndex + 1) % n;
    }

    // delaySamples 이전에 쓰인 샘플을 읽는다 (0 = 방금 push한 샘플).
    SampleType readDelayed(int delaySamples) const
    {
        const int n = size();
        if (n == 0)
            return SampleType{};
        int idx = writeIndex - 1 - delaySamples;
        idx = ((idx % n) + n) % n;
        return buffer[static_cast<size_t>(idx)];
    }

private:
    std::vector<SampleType> buffer;
    int writeIndex = 0;
};
