#include "VoiceActivityTracker.h"

namespace voice_capture
{

void VoiceActivityTracker::prepare(double newSampleRate) noexcept
{
    sampleRate = std::max(1.0, newSampleRate);
    reset();
}

void VoiceActivityTracker::reset() noexcept
{
    energyBaseline = 1.0e-4f;
    currentRms = 0.0f;
    voicedNow = false;
    pendingWordOnset = false;
    secondsSinceVoiceEnded = 10.0;
    secondsVoicedRun = 0.0;
}

void VoiceActivityTracker::processBlock(const float* input, int numSamples) noexcept
{
    if (input == nullptr || numSamples <= 0)
        return;

    double sumSquares = 0.0;
    for (int sample = 0; sample < numSamples; ++sample)
        sumSquares += static_cast<double>(input[sample]) * input[sample];
    currentRms = static_cast<float>(std::sqrt(sumSquares / numSamples));

    const double blockSeconds = static_cast<double>(numSamples) / sampleRate;

    if (!voicedNow)
    {
        secondsSinceVoiceEnded += blockSeconds;

        const float enterThreshold =
            std::max(minimumOnsetRms, energyBaseline * onsetRatio);
        if (currentRms >= enterThreshold)
        {
            if (secondsSinceVoiceEnded >= minimumSilenceGapSeconds)
                pendingWordOnset = true;
            voicedNow = true;
            secondsVoicedRun = 0.0;
        }
        else
        {
            // Only adapt the noise-floor baseline while we are confident
            // we are hearing silence, not the tail of a word.
            const float coefficient = currentRms > energyBaseline ? 0.02f : 0.1f;
            energyBaseline += coefficient * (currentRms - energyBaseline);
            energyBaseline = std::max(energyBaseline, 1.0e-5f);
        }
    }
    else
    {
        secondsVoicedRun += blockSeconds;
        const float exitThreshold = energyBaseline * releaseRatio;
        if (currentRms < exitThreshold)
        {
            voicedNow = false;
            secondsSinceVoiceEnded = 0.0;
        }
    }
}

bool VoiceActivityTracker::consumeWordOnset() noexcept
{
    const bool onset = pendingWordOnset;
    pendingWordOnset = false;
    return onset;
}

} // namespace voice_capture
