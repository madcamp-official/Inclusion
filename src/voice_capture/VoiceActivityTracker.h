#pragma once

#include <algorithm>
#include <cmath>

namespace voice_capture
{

// Adaptive-RMS voice activity detector, structurally mirroring
// mode1::GuitarOnsetTracker but tuned for continuous speech: instead of a
// single refractory-gated pulse it tracks a voiced/silent state with
// hysteresis and reports word-boundary onsets (silence -> voiced after a
// minimum gap) so callers can advance a word-by-word highlight in real time
// without needing full speech recognition.
class VoiceActivityTracker
{
public:
    void prepare(double newSampleRate) noexcept;
    void reset() noexcept;

    void processBlock(const float* input, int numSamples) noexcept;

    // Edge-triggered: true once per detected word onset, then clears.
    bool consumeWordOnset() noexcept;

    [[nodiscard]] bool isVoiced() const noexcept { return voicedNow; }
    [[nodiscard]] float getCurrentRms() const noexcept { return currentRms; }
    [[nodiscard]] double getSecondsSinceVoiceEnded() const noexcept { return secondsSinceVoiceEnded; }
    [[nodiscard]] double getSecondsVoicedRun() const noexcept { return secondsVoicedRun; }

private:
    double sampleRate = 48'000.0;
    float energyBaseline = 1.0e-4f;
    float currentRms = 0.0f;
    bool voicedNow = false;
    bool pendingWordOnset = false;
    double secondsSinceVoiceEnded = 10.0;
    double secondsVoicedRun = 0.0;

    static constexpr float onsetRatio = 1.8f;
    static constexpr float releaseRatio = 1.3f;
    static constexpr float minimumOnsetRms = 0.004f;
    static constexpr double minimumSilenceGapSeconds = 0.12;
};

} // namespace voice_capture
