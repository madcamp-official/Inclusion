#include "PredictivePhraseScheduler.h"

#include <algorithm>

namespace mode1
{

void PredictivePhraseScheduler::prepare(double newSampleRate) noexcept
{
    sampleRate = std::max(1.0, newSampleRate);
}

void PredictivePhraseScheduler::setSong(const SongPackage* package) noexcept
{
    song = package;
    reset();
}

void PredictivePhraseScheduler::reset() noexcept
{
    nextPhraseIndex = 0;
    expiredPhraseCount = 0;
}

void PredictivePhraseScheduler::setOutputLatencySeconds(double seconds) noexcept
{
    commitLookAheadSeconds = std::clamp(seconds + 0.025, 0.020, 0.080);
}

int PredictivePhraseScheduler::processBlock(
    const BeatClockSnapshot& clock) noexcept
{
    if (song == nullptr || !song->isLoaded())
        return -1;
    const bool clockCanSchedule =
        clock.state == BeatClockState::locked
        || clock.state == BeatClockState::coasting
        || (
            clock.state == BeatClockState::recovering
            && clock.confidence >= 0.45);
    if (!clockCanSchedule)
    {
        return -1;
    }

    const auto& phrases = song->getPhrases();
    if (nextPhraseIndex >= static_cast<int>(phrases.size()))
        return -1;

    const double nominalBeatSeconds =
        60.0 / std::max(1.0, song->getScoreBpm());
    const double predictedScoreSeconds =
        clock.beatPosition * nominalBeatSeconds;
    const double dueScoreSeconds =
        predictedScoreSeconds + commitLookAheadSeconds;

    // A recovery jump must not emit a burst. Expire only phrases that are
    // clearly stale, leaving the most recent due phrase as the single output.
    while (nextPhraseIndex + 1 < static_cast<int>(phrases.size())
        && phrases[static_cast<size_t>(
            nextPhraseIndex + 1)].sourceStartSeconds
            <= dueScoreSeconds - 0.150)
    {
        ++nextPhraseIndex;
        ++expiredPhraseCount;
    }

    if (phrases[static_cast<size_t>(
            nextPhraseIndex)].sourceStartSeconds
        > dueScoreSeconds)
    {
        return -1;
    }

    return nextPhraseIndex++;
}

} // namespace mode1
