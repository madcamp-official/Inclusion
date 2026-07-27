#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

namespace voice_capture
{
struct RecordingQuality
{
    double durationSeconds = 0.0;
    float rmsDb = -100.0f;
    float peakDb = -100.0f;
    float clippingRatio = 0.0f;
    float activeSpeechRatio = 0.0f;
    bool passed = false;
    juce::String summary;
};

class RecordingQualityChecker
{
public:
    static RecordingQuality analyse(const juce::AudioBuffer<float>& audio, double sampleRate);
};
}
