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

    // minFrequencyHzIn: 탐색 하한. 이 값이 낮으면 배주기(sub-harmonic) 피크까지 후보에 들어와
    // 한 옥타브 아래로 오검출할 수 있으므로, 대상 음역에 맞춰 최대한 높게 잡는 게 좋다.
    // 기타는 6번줄 개방현(82Hz)까지 필요하지만 노래 목소리는 그보다 훨씬 위에 있다.
    // analysisWindowSizeIn: 자기상관 분석 창(샘플). 길면 관측 주기 수가 늘어 추정이 안정되지만
    // 그만큼 반응이 늦고, 음이 바뀐 직후 이전 음이 섞여 있는 시간도 길어진다.
    //
    // 탐색 범위(min/max)는 대상 음역에 맞게 좁게 잡는 것이 중요하다. 넓으면 배주기 피크가
    // 후보에 들어와 한 옥타브 아래로, 배음 피크가 들어와 한 옥타브 위로 오검출한다.
    void prepare(double sampleRateIn, float minFrequencyHzIn, float maxFrequencyHzIn,
                 int analysisWindowSizeIn);
    void reset();

    // 최근 오디오 블록을 밀어넣고, 슬라이딩 분석 윈도우 기준 최신 피치 추정치를 반환한다.
    Result processBlock(const float* samples, int numSamples);

    static float frequencyToMidi(float frequencyHz);

private:
    float minFrequencyHz = 70.0f;
    float maxFrequencyHz = 1200.0f;
    int analysisWindowSize = 2048;
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
