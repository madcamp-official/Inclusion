#include "BeatClock.h"

#include <algorithm>
#include <cmath>

namespace mode1
{

void BeatClock::prepare(double newSampleRate, double nominalBpm) noexcept
{
    sampleRate = std::max(1.0, newSampleRate);
    nominalSecondsPerBeat = 60.0 / std::clamp(nominalBpm, 30.0, 240.0);
    reset();
}

void BeatClock::reset() noexcept
{
    beatPosition = 0.0;
    secondsPerBeat = nominalSecondsPerBeat;
    phaseErrorSeconds = 0.0;
    confidence = 0.0;
    lastObservedBeat = -1.0;
    originObservedBeat = -1.0;
    lastUpdateSample = 0;
    lastOnsetSample = -1;
    lastAcceptedSample = -1;
    originAcceptedSample = -1;
    acceptedObservations = 0;
    rejectedObservations = 0;
    state = BeatClockState::disarmed;
}

void BeatClock::advanceTo(std::int64_t sample) noexcept
{
    if (sample <= lastUpdateSample)
        return;
    const double elapsed =
        static_cast<double>(sample - lastUpdateSample) / sampleRate;
    if (state != BeatClockState::disarmed
        && state != BeatClockState::holding)
    {
        beatPosition += elapsed / std::max(0.1, secondsPerBeat);
    }
    lastUpdateSample = sample;
}

void BeatClock::observe(
    std::int64_t onsetSample,
    double observedScoreSeconds) noexcept
{
    if (observedScoreSeconds < 0.0)
    {
        ++rejectedObservations;
        return;
    }

    const double observedBeat =
        observedScoreSeconds / nominalSecondsPerBeat;
    if (acceptedObservations == 0)
    {
        beatPosition = observedBeat;
        lastObservedBeat = observedBeat;
        originObservedBeat = observedBeat;
        lastAcceptedSample = onsetSample;
        originAcceptedSample = onsetSample;
        lastOnsetSample = onsetSample;
        acceptedObservations = 1;
        confidence = 0.20;
        phaseErrorSeconds = 0.0;
        state = BeatClockState::arming;
        return;
    }

    lastOnsetSample = onsetSample;
    const double scoreDeltaBeats = observedBeat - lastObservedBeat;
    const double elapsedSeconds =
        static_cast<double>(onsetSample - lastAcceptedSample) / sampleRate;

    // Repeated strings and arpeggio subdivisions often map to the same score
    // event. They keep the clock alive but must not drag phase backwards or
    // double the tempo.
    if (scoreDeltaBeats < 0.125 || elapsedSeconds <= 0.0)
    {
        ++rejectedObservations;
        confidence = std::max(0.0, confidence - 0.01);
        return;
    }

    double candidateSecondsPerBeat =
        elapsedSeconds / scoreDeltaBeats;
    const double originScoreDeltaBeats =
        observedBeat - originObservedBeat;
    const double originElapsedSeconds =
        static_cast<double>(onsetSample - originAcceptedSample) / sampleRate;
    if (originScoreDeltaBeats >= 2.0 && originElapsedSeconds > 0.0)
    {
        const double originCandidate =
            originElapsedSeconds / originScoreDeltaBeats;
        candidateSecondsPerBeat =
            0.35 * candidateSecondsPerBeat + 0.65 * originCandidate;
    }
    const double minimumSecondsPerBeat = nominalSecondsPerBeat / 1.35;
    const double maximumSecondsPerBeat = nominalSecondsPerBeat / 0.65;
    if (candidateSecondsPerBeat < minimumSecondsPerBeat
        || candidateSecondsPerBeat > maximumSecondsPerBeat)
    {
        ++rejectedObservations;
        confidence = std::max(0.0, confidence - 0.04);
        if (state == BeatClockState::locked)
            state = BeatClockState::recovering;
        return;
    }

    phaseErrorSeconds = (observedBeat - beatPosition) * secondsPerBeat;
    const double phaseGain =
        state == BeatClockState::locked ? 0.25 : 0.55;
    beatPosition += phaseGain * (observedBeat - beatPosition);

    const double tempoGain =
        acceptedObservations < 4 ? 0.35 : 0.12;
    secondsPerBeat += tempoGain
        * (candidateSecondsPerBeat - secondsPerBeat);
    secondsPerBeat = std::clamp(
        secondsPerBeat,
        minimumSecondsPerBeat,
        maximumSecondsPerBeat);

    lastObservedBeat = observedBeat;
    lastAcceptedSample = onsetSample;
    ++acceptedObservations;

    const double residualBeats =
        std::abs(phaseErrorSeconds) / std::max(0.1, secondsPerBeat);
    confidence = std::clamp(
        confidence + (residualBeats <= 0.35 ? 0.18 : 0.08),
        0.0,
        1.0);
    state = acceptedObservations >= 3 && confidence >= 0.55
        ? BeatClockState::locked
        : BeatClockState::arming;
}

void BeatClock::updateSilenceState(
    std::int64_t currentSample,
    double expectedNextScoreSeconds,
    bool guitarActive) noexcept
{
    if (state == BeatClockState::disarmed || lastOnsetSample < 0)
        return;

    const double silentBeats =
        static_cast<double>(currentSample - lastOnsetSample)
        / sampleRate
        / std::max(0.1, secondsPerBeat);
    const double expectedNextBeat = expectedNextScoreSeconds >= 0.0
        ? expectedNextScoreSeconds / nominalSecondsPerBeat
        : -1.0;
    const bool nextScoreAttackIsDue =
        expectedNextBeat < 0.0 || beatPosition >= expectedNextBeat + 1.0;
    if (silentBeats >= 1.0 && !guitarActive && nextScoreAttackIsDue)
    {
        state = BeatClockState::holding;
        confidence = std::max(0.0, confidence - 0.08);
    }
    else if (silentBeats >= 0.5
        && state == BeatClockState::locked)
    {
        state = BeatClockState::coasting;
        confidence = std::max(0.0, confidence - 0.02);
    }
}

void BeatClock::processBlock(
    std::int64_t blockStartSample,
    int numSamples,
    bool transportRunning,
    bool onset,
    std::int64_t onsetSample,
    bool scoreObservation,
    double observedScoreSeconds,
    double expectedNextScoreSeconds,
    bool guitarActive) noexcept
{
    const auto blockEndSample =
        blockStartSample + std::max(0, numSamples);
    if (!transportRunning)
    {
        reset();
        lastUpdateSample = blockEndSample;
        return;
    }

    if (state == BeatClockState::disarmed)
        lastUpdateSample = blockStartSample;

    if (onset)
    {
        advanceTo(std::clamp(
            onsetSample,
            blockStartSample,
            blockEndSample));
        lastOnsetSample = onsetSample;
        if (scoreObservation)
            observe(onsetSample, observedScoreSeconds);
    }
    advanceTo(blockEndSample);
    updateSilenceState(
        blockEndSample,
        expectedNextScoreSeconds,
        guitarActive);
}

BeatClockSnapshot BeatClock::getSnapshot() const noexcept
{
    return {
        state,
        beatPosition,
        secondsPerBeat,
        phaseErrorSeconds,
        confidence,
        acceptedObservations,
        rejectedObservations
    };
}

double BeatClock::predictSampleForBeat(double targetBeat) const noexcept
{
    return static_cast<double>(lastUpdateSample)
        + (targetBeat - beatPosition) * secondsPerBeat * sampleRate;
}

} // namespace mode1
