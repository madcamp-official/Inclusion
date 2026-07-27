#pragma once

#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>

// WORLD 분석은 오디오 콜백에서 실행하기에는 너무 무겁다. 이 클래스는 입력과 기타의
// 절대 목표 F0를 lock-free SPSC 링에 넣고, 별도 워커에서 160 ms 분석 / 80 ms hop으로
// 처리한다. 합성 구간은 40 ms 겹쳐 이어 붙인다. 콜백은 완성된 출력만 꺼내므로 WORLD가
// 잠시 늦어져도 오디오 스레드를 막지 않는다.
class WorldRealtimePitchShifter
{
public:
    WorldRealtimePitchShifter() = default;
    ~WorldRealtimePitchShifter();

    WorldRealtimePitchShifter(const WorldRealtimePitchShifter&) = delete;
    WorldRealtimePitchShifter& operator=(const WorldRealtimePitchShifter&) = delete;

    void prepare(double sampleRateIn, int maxBlockSize);
    void reset();

    // absoluteTargetF0Hz가 0보다 크면 원본 F0를 버리고 이 주파수로 합성한다.
    // 목표가 없을 때만 fallbackSemitones를 원본 WORLD F0에 상대 적용한다.
    void processBlock(const float* input, float* output, int numSamples,
                      float absoluteTargetF0Hz, float fallbackSemitones);

    int getLatencySamples() const { return latencySamples; }

private:
    struct InputSample
    {
        float audio = 0.0f;
        float targetF0Hz = 0.0f;
        float fallbackSemitones = 0.0f;
    };

    void startWorker();
    void stopWorker();
    void workerLoop();
    void processWindow();

    bool pushInput(const InputSample& sample);
    bool popInput(InputSample& sample);
    bool pushOutput(float sample);
    bool popOutput(float& sample);

    double sampleRate = 44100.0;
    int windowSamples = 0;
    int hopSamples = 0;
    int preRollSamples = 0;
    int latencySamples = 0;

    std::vector<InputSample> inputRing;
    std::vector<float> outputRing;
    std::atomic<size_t> inputWrite { 0 };
    std::atomic<size_t> inputRead { 0 };
    std::atomic<size_t> outputWrite { 0 };
    std::atomic<size_t> outputRead { 0 };

    std::vector<double> analysisAudio;
    std::vector<float> analysisTargetF0;
    std::vector<float> analysisFallbackShift;
    std::vector<float> previousOverlap;
    int analysisFill = 0;
    bool havePreviousOverlap = false;

    std::atomic<bool> shouldStop { false };
    std::thread worker;
};
