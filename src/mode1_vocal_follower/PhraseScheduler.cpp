#include "PhraseScheduler.h"

#include <algorithm>

namespace mode1
{

void PhraseScheduler::prepare(double newSampleRate)
{
    sampleRate = std::max(1.0, newSampleRate);
}

void PhraseScheduler::setSong(const SongPackage* package)
{
    song = package;
    reset();
}

void PhraseScheduler::reset() noexcept
{
    songTimeSeconds = 0.0;
    nextPhraseIndex = 0;
    running = false;
}

double PhraseScheduler::normalizedStartTime(int phraseIndex) const noexcept
{
    if (song == nullptr || song->getPhrases().empty())
        return 0.0;

    const auto& phrases = song->getPhrases();
    const int index = juce::jlimit(0, static_cast<int>(phrases.size()) - 1, phraseIndex);
    return phrases[static_cast<size_t>(index)].scoreStartSeconds
        - phrases.front().scoreStartSeconds;
}

int PhraseScheduler::startNextPhrase() noexcept
{
    if (song == nullptr
        || nextPhraseIndex >= static_cast<int>(song->getPhrases().size()))
        return -1;

    const int phraseToStart = nextPhraseIndex++;
    songTimeSeconds = normalizedStartTime(phraseToStart);
    running = true;
    return phraseToStart;
}

int PhraseScheduler::processBlock(
    int numSamples,
    bool guitarOnset,
    bool manualTrigger,
    bool automaticPlayback) noexcept
{
    if (song == nullptr || !song->isLoaded())
        return -1;

    if (!running)
    {
        if (guitarOnset || manualTrigger || automaticPlayback)
            return startNextPhrase();
        return -1;
    }

    songTimeSeconds += numSamples / sampleRate;

    if (nextPhraseIndex >= static_cast<int>(song->getPhrases().size()))
        return -1;

    const double targetTime = normalizedStartTime(nextPhraseIndex);
    const double difference = targetTime - songTimeSeconds;
    const bool onsetIsNotTooEarly =
        guitarOnset && difference <= earlyWindowSeconds;

    // In guitar-follow mode the score timeline never advances a phrase by
    // itself. A late strum is still accepted so the performer can recover
    // after pausing or missing an onset. Space remains the manual fallback.
    if (manualTrigger
        || onsetIsNotTooEarly
        || (automaticPlayback && songTimeSeconds >= targetTime))
        return startNextPhrase();

    return -1;
}

} // namespace mode1
