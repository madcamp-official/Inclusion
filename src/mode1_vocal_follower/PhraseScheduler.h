#pragma once

#include "SongPackage.h"

namespace mode1
{

class PhraseScheduler
{
public:
    void prepare(double sampleRate);
    void setSong(const SongPackage* package);
    void reset() noexcept;

    // Returns the phrase index to start, or -1 when no phrase starts.
    int processBlock(int numSamples, bool guitarOnset, bool manualTrigger) noexcept;

    [[nodiscard]] int getNextPhraseIndex() const noexcept { return nextPhraseIndex; }
    [[nodiscard]] double getSongTimeSeconds() const noexcept { return songTimeSeconds; }
    [[nodiscard]] bool isRunning() const noexcept { return running; }

private:
    [[nodiscard]] double normalizedStartTime(int phraseIndex) const noexcept;
    int startNextPhrase() noexcept;

    const SongPackage* song = nullptr;
    double sampleRate = 48'000.0;
    double songTimeSeconds = 0.0;
    double earlyWindowSeconds = 0.80;
    double lateWindowSeconds = 0.20;
    double automaticStartGraceSeconds = 0.20;
    int nextPhraseIndex = 0;
    bool running = false;
};

} // namespace mode1
