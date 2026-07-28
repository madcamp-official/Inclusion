#pragma once

#include <juce_core/juce_core.h>

#include <atomic>

namespace voice_capture
{

class InputLevelCalibrator
{
public:
    enum class Phase
    {
        idle,
        roomTone,
        voice,
        complete,
        failed
    };

    struct Result
    {
        Phase phase = Phase::idle;
        double progress = 0.0;
        float roomToneDb = -100.0f;
        float voiceRmsDb = -100.0f;
        float voicePeakDb = -100.0f;
        float recommendedGain = 1.0f;
        juce::String message;
    };

    void prepare(double newSampleRate) noexcept;
    void start() noexcept;
    void cancel() noexcept;
    void processBlock(const float* input, int numSamples) noexcept;

    [[nodiscard]] bool isRunning() const noexcept;
    [[nodiscard]] Result getResult() const;
    [[nodiscard]] float getInputPeak() const noexcept { return inputPeak.load(); }

private:
    double sampleRate = 48'000.0;
    std::atomic<Phase> phase { Phase::idle };
    std::atomic<int64_t> samplesInPhase { 0 };
    std::atomic<double> roomToneSquares { 0.0 };
    std::atomic<int64_t> roomToneSamples { 0 };
    std::atomic<double> voiceSquares { 0.0 };
    std::atomic<int64_t> voiceSamples { 0 };
    std::atomic<float> voicePeak { 0.0f };
    std::atomic<float> inputPeak { 0.0f };

    static constexpr double roomToneSeconds = 1.5;
    static constexpr double voiceSeconds = 3.0;
};

} // namespace voice_capture
