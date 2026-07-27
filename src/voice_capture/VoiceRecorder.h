#pragma once

#include "RecordingQualityChecker.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <atomic>

namespace voice_capture
{
class VoiceRecorder
{
public:
    static constexpr double maximumRecordingSeconds = 25.0;

    void prepare(double newSampleRate);
    bool start();
    void stop();
    void processBlock(const juce::AudioBuffer<float>& input);

    bool isRecording() const noexcept { return recording.load(); }
    double getRecordedSeconds() const;
    RecordingQuality getQuality() const;
    juce::Result saveAsWav(const juce::File& destination) const;

private:
    mutable juce::SpinLock lock;
    juce::AudioBuffer<float> recordedAudio;
    double sampleRate = 0.0;
    int writePosition = 0;
    std::atomic<bool> recording { false };
};
}
