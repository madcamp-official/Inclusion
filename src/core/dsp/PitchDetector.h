#pragma once

#include <vector>

// MPM(McLeod Pitch Method, NSDF 기반) 단음 피치 검출기.
// 기타/보컬 양쪽에 동일하게 재사용한다(블록 크기·샘플레이트만 파라미터화).
class PitchDetector
{
public:
    struct Result
    {
        bool valid = false;
        float frequencyHz = 0.0f;
        float midiFloat = 0.0f;
        float confidence = 0.0f; // 0~1, NSDF 피크 높이(clarity)
    };

    void prepare(double sampleRateIn);
    void reset();

    // 최근 오디오 블록을 밀어넣고, 슬라이딩 분석 윈도우 기준 최신 피치 추정치를 반환한다.
    Result processBlock(const float* samples, int numSamples);

    static float frequencyToMidi(float frequencyHz);

private:
    static constexpr int analysisWindowSize = 2048;
    static constexpr float minFrequencyHz = 70.0f;
    static constexpr float maxFrequencyHz = 1200.0f;
    static constexpr float peakPickingThreshold = 0.9f;
    static constexpr float minClarityForValidPitch = 0.35f;

    double sampleRate = 44100.0;
    int minLag = 0;
    int maxLag = 0;
    std::vector<float> window;
    int filledSamples = 0;

    void pushSamples(const float* samples, int numSamples);
    Result analyze() const;
};
