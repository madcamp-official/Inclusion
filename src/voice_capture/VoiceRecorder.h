#pragma once

#include "RecordingQualityChecker.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <atomic>

namespace voice_capture
{
class VoiceRecorder
{
public:
    // Long enough to hold the guided song stage as one continuous take
    // (~120 s plus headroom) rather than cutting the singer off partway.
    static constexpr double maximumRecordingSeconds = 150.0;
    static constexpr double storageSampleRate = 48'000.0;

    void prepare(double newSampleRate);
    bool start();
    void stop();
    void processBlock(const juce::AudioBuffer<float>& input, int inputChannel);

    bool isRecording() const noexcept { return recording.load(); }
    double getRecordedSeconds() const;
    float getInputPeak() const noexcept { return inputPeak.load(); }
    RecordingQuality getQuality() const;
    juce::Result saveAsWav(const juce::File& destination,
                           bool applyNoiseReduction) const;

private:
    mutable juce::SpinLock lock;
    juce::AudioBuffer<float> recordedAudio;
    double sampleRate = 0.0;
    int writePosition = 0;
    std::atomic<bool> recording { false };
    std::atomic<float> inputPeak { 0.0f };
};
}
