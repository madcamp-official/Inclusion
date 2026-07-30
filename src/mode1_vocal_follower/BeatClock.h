#pragma once

#include <cstdint>

namespace mode1
{

enum class BeatClockState : std::uint8_t
{
    disarmed,
    arming,
    locked,
    coasting,
    holding,
    recovering
};

struct BeatClockSnapshot
{
    BeatClockState state = BeatClockState::disarmed;
    double beatPosition = 0.0;
    double secondsPerBeat = 0.5;
    double phaseErrorSeconds = 0.0;
    double confidence = 0.0;
    int acceptedObservations = 0;
    int rejectedObservations = 0;
};

// A song-agnostic shadow clock. It never starts audio; it estimates a
// continuously advancing beat and confidence from score-position observations.
class BeatClock
{
public:
    void prepare(double sampleRate, double nominalBpm) noexcept;
    void reset() noexcept;

    void processBlock(
        std::int64_t blockStartSample,
        int numSamples,
        bool transportRunning,
        bool onset,
        std::int64_t onsetSample,
        bool scoreObservation,
        double observedScoreSeconds,
        double expectedNextScoreSeconds,
        bool guitarActive) noexcept;

    [[nodiscard]] BeatClockSnapshot getSnapshot() const noexcept;
    [[nodiscard]] double predictSampleForBeat(double targetBeat) const noexcept;

private:
    void advanceTo(std::int64_t sample) noexcept;
    void observe(
        std::int64_t onsetSample,
        double observedScoreSeconds) noexcept;
    void updateSilenceState(
        std::int64_t currentSample,
        double expectedNextScoreSeconds,
        bool guitarActive) noexcept;

    double sampleRate = 48'000.0;
    double nominalSecondsPerBeat = 0.5;
    double beatPosition = 0.0;
    double secondsPerBeat = 0.5;
    double phaseErrorSeconds = 0.0;
    double confidence = 0.0;
    double lastObservedBeat = -1.0;
    double originObservedBeat = -1.0;
    std::int64_t lastUpdateSample = 0;
    std::int64_t lastOnsetSample = -1;
    std::int64_t lastAcceptedSample = -1;
    std::int64_t originAcceptedSample = -1;
    int acceptedObservations = 0;
    int rejectedObservations = 0;
    BeatClockState state = BeatClockState::disarmed;
};

} // namespace mode1
