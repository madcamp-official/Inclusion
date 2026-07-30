#pragma once

#include "BeatClock.h"
#include "SongPackage.h"

namespace mode1
{

class PredictivePhraseScheduler
{
public:
    void prepare(double sampleRate) noexcept;
    void setSong(const SongPackage* package) noexcept;
    void reset() noexcept;
    void setOutputLatencySeconds(double seconds) noexcept;

    // Returns at most one phrase per block. The caller may keep the legacy
    // scheduler running in parallel for score-position observations.
    int processBlock(const BeatClockSnapshot& clock) noexcept;

    [[nodiscard]] int getNextPhraseIndex() const noexcept
    {
        return nextPhraseIndex;
    }
    [[nodiscard]] int getExpiredPhraseCount() const noexcept
    {
        return expiredPhraseCount;
    }
    [[nodiscard]] double getCommitLookAheadSeconds() const noexcept
    {
        return commitLookAheadSeconds;
    }

private:
    const SongPackage* song = nullptr;
    double sampleRate = 48'000.0;
    double commitLookAheadSeconds = 0.025;
    int nextPhraseIndex = 0;
    int expiredPhraseCount = 0;
};

} // namespace mode1
