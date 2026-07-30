#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <atomic>
#include <vector>

class AudioLatencyCalibrator
{
public:
    struct Result
    {
        bool valid = false;
        double roundTripSamples = 0.0;
        double jitterSamples = 0.0;
        float correlation = 0.0f;
        int successfulProbes = 0;
        int totalProbes = 0;
        juce::String message;
    };

    void prepare(double sampleRate);
    bool start();
    void cancel();

    // Called only from the audio callback. input may be nullptr.
    void processBlock(const float* input,
                      float* outputLeft,
                      float* outputRight,
                      int numSamples) noexcept;

    bool isMeasuring() const noexcept;
    bool isAnalysisPending() const noexcept;
    Result analyseCompletedCapture();

private:
    enum class State
    {
        idle,
        measuring,
        analysisPending,
    };

    static constexpr int probeLength = 255;
    static constexpr int probeCount = 5;
    static constexpr float probeLevel = 0.08f;

    void buildProbe();
    int getEmissionSample(int probeIndex) const noexcept;

    std::atomic<State> state { State::idle };
    double preparedSampleRate = 48'000.0;
    int preRollSamples = 0;
    int probeSpacingSamples = 0;
    int captureLengthSamples = 0;
    int writePosition = 0;
    std::array<float, probeLength> probe {};
    std::vector<float> capturedInput;
};
