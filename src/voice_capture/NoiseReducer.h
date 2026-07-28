#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

namespace voice_capture
{
class NoiseReducer
{
public:
    // The first second is treated as room tone. Processing is intentionally
    // conservative so that the singer's timbre is not damaged.
    static void process(juce::AudioBuffer<float>& audio, double sampleRate);
};
}
