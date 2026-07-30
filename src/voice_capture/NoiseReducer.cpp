#include "NoiseReducer.h"

#include <cmath>

namespace voice_capture
{
void NoiseReducer::process(juce::AudioBuffer<float>& audio, double sampleRate)
{
    if (audio.getNumChannels() == 0 || audio.getNumSamples() == 0 || sampleRate <= 0.0)
        return;

    auto* samples = audio.getWritePointer(0);
    const int sampleCount = audio.getNumSamples();
    const int roomToneSamples =
        juce::jmin(sampleCount, static_cast<int>(sampleRate));

    double noiseSquares = 0.0;
    for (int i = 0; i < roomToneSamples; ++i)
        noiseSquares += static_cast<double>(samples[i]) * samples[i];

    const float noiseRms = static_cast<float>(
        std::sqrt(noiseSquares / juce::jmax(1, roomToneSamples)));
    const float gateThreshold = juce::jmax(
        noiseRms * 2.8f,
        juce::Decibels::decibelsToGain(-55.0f));

    const float highPassR =
        static_cast<float>(std::exp(-juce::MathConstants<double>::twoPi * 70.0 / sampleRate));
    const float envelopeRelease =
        static_cast<float>(std::exp(-1.0 / (0.08 * sampleRate)));
    const float gainSmoothing =
        static_cast<float>(std::exp(-1.0 / (0.012 * sampleRate)));

    float previousInput = 0.0f;
    float previousHighPassed = 0.0f;
    float envelope = 0.0f;
    float gain = 1.0f;

    for (int i = 0; i < sampleCount; ++i)
    {
        const float input = samples[i];
        const float highPassed =
            input - previousInput + highPassR * previousHighPassed;
        previousInput = input;
        previousHighPassed = highPassed;

        const float magnitude = std::abs(highPassed);
        envelope = magnitude > envelope
            ? magnitude
            : envelopeRelease * envelope + (1.0f - envelopeRelease) * magnitude;

        const float ratio = juce::jlimit(0.0f, 1.0f, envelope / gateThreshold);
        const float targetGain = envelope >= gateThreshold
            ? 1.0f
            : 0.12f + 0.88f * ratio * ratio;
        gain = gainSmoothing * gain + (1.0f - gainSmoothing) * targetGain;
        samples[i] = highPassed * gain;
    }

    const int fadeSamples = juce::jmin(
        sampleCount / 2,
        static_cast<int>(sampleRate * 0.01));
    audio.applyGainRamp(0, 0, fadeSamples, 0.0f, 1.0f);
    audio.applyGainRamp(0, sampleCount - fadeSamples, fadeSamples, 1.0f, 0.0f);
}
}
