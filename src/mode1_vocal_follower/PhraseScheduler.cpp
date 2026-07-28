#include "PhraseScheduler.h"

#include <algorithm>
#include <cmath>

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
    pendingDuePhraseIndex = -1;
    lastStartedPhraseIndex = -1;
    lastPhraseTriggerPerformanceSeconds = 0.0;
    nextChordEventIndex = firstPlayableChordEvent();
    currentChordEventIndex = -1;
    currentChordPerformanceStartSeconds = 0.0;
    running = false;
    secondsSinceChordMatch = 0.0;
    activeSecondsSinceChordMatch = 0.0;
    inactiveTailSeconds = 0.0;
    performanceTimeSeconds = 0.0;
    tempoScale = 1.0;
    tempoObservationCount = 0;
    tempoAnchorEventIndex = -1;
    tempoAnchorPerformanceSeconds = 0.0;
    recoveredSkippedChordCount = 0;
    expiredPhraseCount = 0;
}

double PhraseScheduler::normalizedScoreStartTime(int phraseIndex) const noexcept
{
    if (song == nullptr || song->getPhrases().empty())
        return 0.0;

    const auto& phrases = song->getPhrases();
    const int index = juce::jlimit(0, static_cast<int>(phrases.size()) - 1, phraseIndex);
    return phrases[static_cast<size_t>(index)].scoreStartSeconds
        - phrases.front().scoreStartSeconds;
}

int PhraseScheduler::firstPlayableChordEvent() const noexcept
{
    return nextPlayableChordEvent(-1);
}

int PhraseScheduler::nextPlayableChordEvent(int afterIndex) const noexcept
{
    if (song == nullptr)
        return -1;

    const auto& timeline = song->getChordTimeline();
    for (int index = afterIndex + 1;
         index < static_cast<int>(timeline.size());
         ++index)
    {
        const auto chord = timeline[static_cast<size_t>(index)].chord.trim();
        if (chord.isNotEmpty()
            && chord.toUpperCase() != "N"
            && chord.toUpperCase() != "N.C.")
            return index;
    }
    return -1;
}

double PhraseScheduler::scoreBeatSeconds() const noexcept
{
    if (song == nullptr || song->getScoreBpm() <= 1.0)
        return 0.5;
    return 60.0 / song->getScoreBpm();
}

int PhraseScheduler::chooseChordEventForOnset(
    float onsetStrength) const noexcept
{
    if (song == nullptr || nextChordEventIndex < 0)
        return -1;
    if (currentChordEventIndex < 0)
        return nextChordEventIndex;

    const auto& timeline = song->getChordTimeline();
    const double currentScoreTime = timeline[
        static_cast<size_t>(currentChordEventIndex)].startSeconds;
    const double beatRealSeconds =
        scoreBeatSeconds() / std::max(0.65, tempoScale);
    // A generous early window mistakes subdivision strums for the next score
    // event. Keep this below roughly one third of a beat; actual tempo drift
    // is handled by the multi-beat tempo estimate instead.
    const double earlyTolerance = juce::jlimit(
        0.10,
        0.18,
        beatRealSeconds * 0.30);
    const double nextScoreDelta = timeline[
        static_cast<size_t>(nextChordEventIndex)].startSeconds
        - currentScoreTime;
    const double nextPrediction =
        nextScoreDelta / std::max(0.65, tempoScale);
    const bool strongBoundaryPrediction =
        nextScoreDelta >= scoreBeatSeconds() * 2.0
        && onsetStrength >= 3.0f
        && secondsSinceChordMatch
            >= nextPrediction - beatRealSeconds * 1.20;
    if (secondsSinceChordMatch < nextPrediction - earlyTolerance
        && !strongBoundaryPrediction)
        return -1;

    // A long real pause is a pause in the performance, not a missed chord.
    // Only search ahead when the guitar remained active for much of the
    // interval, which indicates that the player continued while an onset was
    // missed by the detector.
    const double activityRatio = activeSecondsSinceChordMatch
        / std::max(1.0e-6, secondsSinceChordMatch);
    const bool recoveryAllowed =
        activityRatio >= 0.55
        && inactiveTailSeconds <= beatRealSeconds * 0.75
        && secondsSinceChordMatch
            > nextPrediction + beatRealSeconds * 0.70;
    if (!recoveryAllowed)
        return nextChordEventIndex;

    int bestIndex = nextChordEventIndex;
    double bestCost = std::abs(secondsSinceChordMatch - nextPrediction);
    int candidateIndex = nextChordEventIndex;
    for (int skipped = 1; skipped <= 3; ++skipped)
    {
        candidateIndex = nextPlayableChordEvent(candidateIndex);
        if (candidateIndex < 0)
            break;
        const double scoreDelta = timeline[
            static_cast<size_t>(candidateIndex)].startSeconds
            - currentScoreTime;
        const double prediction =
            scoreDelta / std::max(0.65, tempoScale);
        const double skipPenalty =
            skipped * beatRealSeconds * 0.28;
        const double cost =
            std::abs(secondsSinceChordMatch - prediction) + skipPenalty;
        if (cost < bestCost)
        {
            bestCost = cost;
            bestIndex = candidateIndex;
        }
    }
    return bestIndex;
}

void PhraseScheduler::updateTempoEstimate(int matchedEventIndex) noexcept
{
    if (song == nullptr
        || matchedEventIndex < 0)
        return;

    if (tempoAnchorEventIndex < 0)
    {
        tempoAnchorEventIndex = matchedEventIndex;
        tempoAnchorPerformanceSeconds = performanceTimeSeconds;
        return;
    }
    if (matchedEventIndex <= tempoAnchorEventIndex)
        return;

    const auto& timeline = song->getChordTimeline();
    const double scoreDelta =
        timeline[static_cast<size_t>(matchedEventIndex)].startSeconds
        - timeline[static_cast<size_t>(tempoAnchorEventIndex)].startSeconds;
    const double elapsedSeconds =
        performanceTimeSeconds - tempoAnchorPerformanceSeconds;

    // Chord changes inside one beat are commonly played early or late for
    // feel. Using those short intervals as a BPM observation makes repeated
    // strums look like a much faster song. Accumulate at least two beats so
    // the estimate represents musical tempo rather than articulation.
    const double minimumTempoObservationBeats =
        song->hasTabTracking() ? 4.0 : 2.0;
    if (scoreDelta
            < scoreBeatSeconds() * minimumTempoObservationBeats
        || elapsedSeconds <= 0.10)
        return;

    const double observation = scoreDelta / elapsedSeconds;
    if (observation < 0.75 || observation > 1.30)
        return;

    const double smoothing = song->hasTabTracking()
        ? (tempoObservationCount < 4 ? 0.16 : 0.08)
        : (tempoObservationCount < 4 ? 0.32 : 0.16);
    tempoScale = juce::jlimit(
        0.80,
        1.25,
        tempoScale + smoothing * (observation - tempoScale));
    ++tempoObservationCount;
    tempoAnchorEventIndex = matchedEventIndex;
    tempoAnchorPerformanceSeconds = performanceTimeSeconds;
}

int PhraseScheduler::advanceChordCursor(
    bool force,
    float onsetStrength) noexcept
{
    if (song == nullptr || nextChordEventIndex < 0)
        return -1;

    const int matchedEventIndex = force
        ? nextChordEventIndex
        : chooseChordEventForOnset(onsetStrength);
    if (matchedEventIndex < 0)
        return -1;

    if (!force)
        updateTempoEstimate(matchedEventIndex);
    else
    {
        tempoAnchorEventIndex = matchedEventIndex;
        tempoAnchorPerformanceSeconds = performanceTimeSeconds;
    }
    const int previousEventIndex = currentChordEventIndex;
    if (previousEventIndex >= 0)
    {
        int skipped = 0;
        for (int index = nextChordEventIndex;
             index >= 0 && index != matchedEventIndex;
             index = nextPlayableChordEvent(index))
            ++skipped;
        recoveredSkippedChordCount += skipped;
    }

    currentChordEventIndex = matchedEventIndex;
    currentChordPerformanceStartSeconds = performanceTimeSeconds;
    const auto& matchedEvent =
        song->getChordTimeline()[static_cast<size_t>(matchedEventIndex)];
    songTimeSeconds = matchedEvent.startSeconds;
    nextChordEventIndex = nextPlayableChordEvent(currentChordEventIndex);
    secondsSinceChordMatch = 0.0;
    activeSecondsSinceChordMatch = 0.0;
    inactiveTailSeconds = 0.0;
    running = true;
    return currentChordEventIndex;
}

int PhraseScheduler::startDueGuitarPhrase() noexcept
{
    if (song == nullptr
        || nextPhraseIndex >= static_cast<int>(song->getPhrases().size()))
        return -1;

    const auto& phrases = song->getPhrases();
    while (nextPhraseIndex < static_cast<int>(phrases.size())
        && phrases[static_cast<size_t>(
            nextPhraseIndex)].anchorChordEventIndex >= 0
        && phrases[static_cast<size_t>(
            nextPhraseIndex)].anchorChordEventIndex
            < currentChordEventIndex)
    {
        ++nextPhraseIndex;
        ++expiredPhraseCount;
        pendingDuePhraseIndex = -1;
    }
    if (nextPhraseIndex >= static_cast<int>(phrases.size()))
        return -1;

    const auto& nextPhrase =
        phrases[static_cast<size_t>(nextPhraseIndex)];
    if (nextPhrase.anchorChordEventIndex < 0)
        return -1;
    if (nextPhrase.anchorChordEventIndex > currentChordEventIndex)
        return -1;

    const double segmentElapsedSeconds =
        performanceTimeSeconds - currentChordPerformanceStartSeconds;
    const double segmentTargetSeconds =
        nextPhrase.chordRelativeStartSeconds
            / juce::jlimit(0.80, 1.25, tempoScale)
        - guitarVocalLeadSeconds;
    if (segmentElapsedSeconds < segmentTargetSeconds)
        return -1;

    // If timing jumped forward at a chord boundary, never burst through a
    // backlog of mora clips. The chord anchor above expires older segments;
    // within the current segment, phrases remain ordered and keep their
    // source-relative spacing.
    int phraseToStart = pendingDuePhraseIndex;
    if (phraseToStart < 0)
    {
        phraseToStart = nextPhraseIndex;
        pendingDuePhraseIndex = phraseToStart;
    }

    // Score-position recovery may jump the score clock forward, but it must
    // not compress breathing spaces or syllable timing. Preserve the source
    // interval from the last emitted micro phrase, scaled only by the
    // performer's measured tempo.
    if (lastStartedPhraseIndex >= 0
        && phraseToStart > lastStartedPhraseIndex)
    {
        if (performanceTimeSeconds
                - lastPhraseTriggerPerformanceSeconds
            < 0.050)
            return -1;
        const auto& lastPhrase =
            phrases[static_cast<size_t>(lastStartedPhraseIndex)];
        const auto& duePhrase =
            phrases[static_cast<size_t>(phraseToStart)];
        if (lastPhrase.anchorChordEventIndex
            == duePhrase.anchorChordEventIndex)
        {
            const double sourceGap =
                duePhrase.sourceStartSeconds
                - lastPhrase.sourceStartSeconds;
            const double minimumRealGap =
                sourceGap / juce::jlimit(0.80, 1.25, tempoScale);
            if (performanceTimeSeconds - lastPhraseTriggerPerformanceSeconds
                < minimumRealGap)
                return -1;
        }
    }

    nextPhraseIndex = phraseToStart + 1;
    pendingDuePhraseIndex = -1;
    lastStartedPhraseIndex = phraseToStart;
    lastPhraseTriggerPerformanceSeconds = performanceTimeSeconds;
    return phraseToStart;
}

int PhraseScheduler::startNextAutomaticPhrase() noexcept
{
    if (song == nullptr
        || nextPhraseIndex >= static_cast<int>(song->getPhrases().size()))
        return -1;

    const int phraseToStart = nextPhraseIndex++;
    if (!running)
    {
        songTimeSeconds = normalizedScoreStartTime(phraseToStart);
        running = true;
    }
    return phraseToStart;
}

int PhraseScheduler::processBlock(
    int numSamples,
    bool guitarOnset,
    bool manualTrigger,
    bool automaticPlayback,
    bool guitarActive,
    float onsetStrength) noexcept
{
    if (song == nullptr || !song->isLoaded())
        return -1;

    if (automaticPlayback)
    {
        if (!running)
            return startNextAutomaticPhrase();

        songTimeSeconds += numSamples / sampleRate;
        if (nextPhraseIndex < static_cast<int>(song->getPhrases().size())
            && songTimeSeconds
                >= normalizedScoreStartTime(nextPhraseIndex))
            return startNextAutomaticPhrase();
        return -1;
    }

    const bool chordTrigger = guitarOnset || manualTrigger;
    const bool hasTimeline = !song->getChordTimeline().empty();

    // Older packages without a global chord timeline retain a conservative
    // one-trigger-per-phrase fallback.
    if (!hasTimeline)
    {
        if (chordTrigger
            && nextPhraseIndex < static_cast<int>(song->getPhrases().size()))
        {
            running = true;
            return nextPhraseIndex++;
        }
        return -1;
    }

    if (running)
    {
        const double elapsed = numSamples / sampleRate;
        songTimeSeconds += elapsed * tempoScale;
        secondsSinceChordMatch += elapsed;
        performanceTimeSeconds += elapsed;
        if (guitarActive)
        {
            activeSecondsSinceChordMatch += elapsed;
            inactiveTailSeconds = 0.0;
        }
        else
        {
            inactiveTailSeconds += elapsed;
        }
    }
    if (manualTrigger)
        advanceChordCursor(true, onsetStrength);
    else if (guitarOnset)
        advanceChordCursor(false, onsetStrength);

    if (!running)
        return -1;

    // A vocal segment may run freely inside the currently held chord, but it
    // cannot cross the next printed chord change until the player strikes it.
    if (nextChordEventIndex >= 0)
    {
        const double boundary = song->getChordTimeline()[
            static_cast<size_t>(nextChordEventIndex)].startSeconds;
        songTimeSeconds = std::min(songTimeSeconds, boundary - 1.0e-4);
    }

    return startDueGuitarPhrase();
}

} // namespace mode1
