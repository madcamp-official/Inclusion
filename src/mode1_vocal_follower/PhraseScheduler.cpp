#include "PhraseScheduler.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace mode1
{
namespace
{
int notePitchClass(const juce::String& text) noexcept
{
    const auto name = text.trim().toUpperCase();
    if (name.isEmpty())
        return -1;
    int root = -1;
    switch (name[0])
    {
        case 'C': root = 0; break;
        case 'D': root = 2; break;
        case 'E': root = 4; break;
        case 'F': root = 5; break;
        case 'G': root = 7; break;
        case 'A': root = 9; break;
        case 'B': root = 11; break;
        default: return -1;
    }
    if (name.length() >= 2)
    {
        if (name[1] == '#')
            root = (root + 1) % 12;
        else if (name[1] == 'B')
            root = (root + 11) % 12;
    }
    return root;
}

int chordQuality(const juce::String& chord) noexcept
{
    const auto suffix = chord.fromFirstOccurrenceOf(
        chord.substring(0, chord.indexOfAnyOf("#b") >= 0 ? 2 : 1),
        false,
        false).upToFirstOccurrenceOf("/", false, false).toLowerCase();
    if (suffix.contains("maj7")) return 6;
    if (suffix.contains("m7")) return 7;
    if (suffix.contains("dim")) return 2;
    if (suffix.contains("sus2")) return 3;
    if (suffix.contains("sus")) return 4;
    if (suffix.contains("7")) return 5;
    if (suffix.startsWith("m")) return 1;
    return 0;
}

int bassPitchClass(const juce::String& chord) noexcept
{
    const int slash = chord.indexOfChar('/');
    return slash >= 0 ? notePitchClass(chord.substring(slash + 1)) : -1;
}

float maskSimilarity(int left, int right) noexcept
{
    if (left == 0 || right == 0)
        return 0.0f;
    const auto intersection = static_cast<unsigned int>(left & right);
    const auto unionMask = static_cast<unsigned int>(left | right);
    return static_cast<float>(std::popcount(intersection))
        / std::max(1, std::popcount(unionMask));
}
} // namespace

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
    performanceOriginEventIndex = -1;
    performanceOriginSeconds = 0.0;
    recoveredSkippedChordCount = 0;
    expiredPhraseCount = 0;
    evidenceCorrectionCount = 0;
    pendingEvidenceCandidate = -1;
    pendingEvidenceCount = 0;
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
    float onsetStrength) noexcept
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
    if (song->hasTabTracking() && performanceOriginEventIndex >= 0)
    {
        const double performedFromOrigin =
            performanceTimeSeconds - performanceOriginSeconds;
        int candidate = nextChordEventIndex;
        int bestIndex = -1;
        double bestError = std::numeric_limits<double>::max();
        for (int lookAhead = 0;
             lookAhead <= 5 && candidate >= 0;
             ++lookAhead)
        {
            const double scoreFromOrigin =
                timeline[static_cast<size_t>(candidate)].startSeconds
                - timeline[static_cast<size_t>(
                    performanceOriginEventIndex)].startSeconds;
            const double expectedFromOrigin =
                scoreFromOrigin / juce::jlimit(0.80, 1.25, tempoScale);
            if (performedFromOrigin
                >= expectedFromOrigin - earlyTolerance)
            {
                const double error =
                    std::abs(performedFromOrigin - expectedFromOrigin)
                    + lookAhead * beatRealSeconds * 0.08;
                if (error < bestError)
                {
                    bestError = error;
                    bestIndex = candidate;
                }
            }
            else if (bestIndex >= 0)
            {
                break;
            }
            candidate = nextPlayableChordEvent(candidate);
        }
        if (bestIndex < 0)
            return -1;
        if (bestIndex != nextChordEventIndex)
        {
            const double activityRatio = activeSecondsSinceChordMatch
                / std::max(1.0e-6, secondsSinceChordMatch);
            const bool safeRecovery =
                activityRatio >= 0.55
                && inactiveTailSeconds <= beatRealSeconds * 0.75;
            if (!safeRecovery)
                bestIndex = nextChordEventIndex;
        }

        // If the performer inserted a genuine pause, move the phase origin
        // instead of jumping through several score events. Written rests are
        // already present in scoreFromOrigin and therefore do not trigger
        // this correction.
        const double bestScoreFromOrigin =
            timeline[static_cast<size_t>(bestIndex)].startSeconds
            - timeline[static_cast<size_t>(
                performanceOriginEventIndex)].startSeconds;
        const double bestExpected =
            bestScoreFromOrigin / juce::jlimit(0.80, 1.25, tempoScale);
        const double lateness = performedFromOrigin - bestExpected;
        if (inactiveTailSeconds > beatRealSeconds * 1.25
            && lateness > beatRealSeconds * 1.25)
        {
            performanceOriginSeconds +=
                lateness - beatRealSeconds * 0.35;
            return nextChordEventIndex;
        }
        return bestIndex;
    }
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

double PhraseScheduler::observedChordBoundaryPerformanceSeconds(
    int eventIndex,
    double onsetPerformanceSeconds) const noexcept
{
    if (song == nullptr)
        return onsetPerformanceSeconds;
    const auto* hint = song->getTabChordHint(eventIndex);
    if (hint == nullptr)
        return onsetPerformanceSeconds;
    const double maximumObservableOffset = scoreBeatSeconds() * 0.50;
    const double realOffset =
        std::min(hint->firstOnsetOffsetSeconds, maximumObservableOffset)
        / juce::jlimit(0.80, 1.25, tempoScale);
    return std::max(0.0, onsetPerformanceSeconds - realOffset);
}

double PhraseScheduler::estimatedChordBoundaryPerformanceSeconds(
    int eventIndex,
    double onsetPerformanceSeconds) const noexcept
{
    const double observed = observedChordBoundaryPerformanceSeconds(
        eventIndex, onsetPerformanceSeconds);
    if (song == nullptr
        || !song->hasTabTracking()
        || performanceOriginEventIndex < 0
        || eventIndex <= performanceOriginEventIndex)
        return observed;

    const auto& timeline = song->getChordTimeline();
    const double scoreFromOrigin =
        timeline[static_cast<size_t>(eventIndex)].startSeconds
        - timeline[static_cast<size_t>(
            performanceOriginEventIndex)].startSeconds;
    const double predicted = performanceOriginSeconds
        + scoreFromOrigin / juce::jlimit(0.80, 1.25, tempoScale);
    const double maximumPhaseCorrection =
        scoreBeatSeconds()
        / juce::jlimit(0.80, 1.25, tempoScale)
        * 0.45;
    return predicted + juce::jlimit(
        -maximumPhaseCorrection,
        maximumPhaseCorrection,
        observed - predicted);
}

void PhraseScheduler::updateTempoEstimate(
    int matchedEventIndex,
    double matchedPerformanceSeconds) noexcept
{
    if (song == nullptr
        || matchedEventIndex < 0)
        return;

    const double observationPerformanceSeconds =
        matchedPerformanceSeconds >= 0.0
        ? matchedPerformanceSeconds
        : performanceTimeSeconds;
    const auto& timeline = song->getChordTimeline();
    if (song->hasTabTracking() && performanceOriginEventIndex >= 0)
    {
        const double scoreFromOrigin =
            timeline[static_cast<size_t>(matchedEventIndex)].startSeconds
            - timeline[static_cast<size_t>(
                performanceOriginEventIndex)].startSeconds;
        const double performanceFromOrigin =
            observationPerformanceSeconds - performanceOriginSeconds;
        if (scoreFromOrigin < scoreBeatSeconds() * 12.0
            || performanceFromOrigin <= 0.10)
            return;
        const double observation =
            scoreFromOrigin / performanceFromOrigin;
        if (observation < 0.75 || observation > 1.30)
            return;
        // An origin-anchored estimate cannot accumulate the varying delay of
        // individual arpeggio notes. It behaves like a causal regression
        // slope and follows sustained tempo changes gradually.
        tempoScale = juce::jlimit(
            0.80,
            1.25,
            tempoScale + 0.06 * (observation - tempoScale));
        ++tempoObservationCount;
        return;
    }
    if (tempoAnchorEventIndex < 0)
    {
        tempoAnchorEventIndex = matchedEventIndex;
        tempoAnchorPerformanceSeconds = observationPerformanceSeconds;
        return;
    }
    if (matchedEventIndex <= tempoAnchorEventIndex)
        return;

    const double scoreDelta =
        timeline[static_cast<size_t>(matchedEventIndex)].startSeconds
        - timeline[static_cast<size_t>(tempoAnchorEventIndex)].startSeconds;
    const double elapsedSeconds =
        observationPerformanceSeconds - tempoAnchorPerformanceSeconds;

    // Chord changes inside one beat are commonly played early or late for
    // feel. Using those short intervals as a BPM observation makes repeated
    // strums look like a much faster song. Accumulate at least two beats so
    // the estimate represents musical tempo rather than articulation.
    const double minimumTempoObservationBeats =
        song->hasTabTracking() ? 12.0 : 2.0;
    if (scoreDelta
            < scoreBeatSeconds() * minimumTempoObservationBeats
        || elapsedSeconds <= 0.10)
        return;

    const double observation = scoreDelta / elapsedSeconds;
    if (observation < 0.75 || observation > 1.30)
        return;

    const double smoothing = song->hasTabTracking()
        ? 0.05
        : (tempoObservationCount < 4 ? 0.32 : 0.16);
    tempoScale = juce::jlimit(
        0.80,
        1.25,
        tempoScale + smoothing * (observation - tempoScale));
    ++tempoObservationCount;
    tempoAnchorEventIndex = matchedEventIndex;
    tempoAnchorPerformanceSeconds = observationPerformanceSeconds;
}

void PhraseScheduler::applyChordEvidence(
    const ChordEvidence& evidence) noexcept
{
    if (song == nullptr || !evidence.valid || currentChordEventIndex < 0)
        return;
    // Before the first lyric the repeated intro progression is deliberately
    // ambiguous. The existing phase guard owns this section; chord evidence
    // must not jump across the four-bar count-in.
    if (nextPhraseIndex == 0)
        return;

    const auto& timeline = song->getChordTimeline();
    const auto& currentChord =
        timeline[static_cast<size_t>(currentChordEventIndex)].chord;
    if (notePitchClass(currentChord) == evidence.rootPitchClass)
    {
        pendingEvidenceCandidate = -1;
        pendingEvidenceCount = 0;
        return;
    }
    const double evidenceTime = std::max(
        0.0, performanceTimeSeconds - evidence.analysisDelaySeconds);
    const double beatSeconds = scoreBeatSeconds()
        / juce::jlimit(0.80, 1.25, tempoScale);
    const auto expectedPerformanceTime = [&](int eventIndex) noexcept
    {
        if (performanceOriginEventIndex < 0)
            return evidenceTime;
        const double scoreFromOrigin =
            timeline[static_cast<size_t>(eventIndex)].startSeconds
            - timeline[static_cast<size_t>(
                performanceOriginEventIndex)].startSeconds;
        return performanceOriginSeconds
            + scoreFromOrigin / juce::jlimit(0.80, 1.25, tempoScale);
    };
    const double currentPhaseError = std::abs(
        evidenceTime - expectedPerformanceTime(currentChordEventIndex));
    if (currentPhaseError < beatSeconds * 0.55)
        return;

    int candidate = currentChordEventIndex;
    int bestIndex = currentChordEventIndex;
    float currentScore = -100.0f;
    float bestScore = -100.0f;
    double bestPhaseError = currentPhaseError;
    for (int distance = 0; distance <= 3 && candidate >= 0; ++distance)
    {
        const auto& expected =
            timeline[static_cast<size_t>(candidate)].chord;
        float score = distance * -0.38f;
        const double phaseError = std::abs(
            evidenceTime - expectedPerformanceTime(candidate));
        score -= static_cast<float>(
            phaseError / std::max(0.10, beatSeconds) * 0.65);
        const int expectedRoot = notePitchClass(expected);
        score += expectedRoot == evidence.rootPitchClass ? 2.6f : -1.4f;

        const int expectedBass = bassPitchClass(expected);
        if (expectedBass >= 0 && evidence.bassPitchClass >= 0)
            score += expectedBass == evidence.bassPitchClass ? 0.55f : -0.25f;

        const int expectedQuality = chordQuality(expected);
        if (evidence.quality < 8)
        {
            if (expectedQuality == evidence.quality)
                score += 0.8f;
            else if (
                (expectedQuality == 0 && evidence.quality == 6)
                || (expectedQuality == 1 && evidence.quality == 7)
                || (expectedQuality == 6 && evidence.quality == 0)
                || (expectedQuality == 7 && evidence.quality == 1))
                score += 0.25f;
            else
                score -= 0.35f;
        }

        float similarity = 0.0f;
        if (const auto* hint = song->getTabChordHint(candidate))
        {
            similarity = maskSimilarity(
                hint->pitchClassMask, evidence.pitchClassMask);
            score += similarity * 3.0f;
        }
        if (distance == 0)
            currentScore = score;
        if (score > bestScore)
        {
            bestScore = score;
            bestIndex = candidate;
            bestPhaseError = phaseError;
        }
        candidate = nextPlayableChordEvent(candidate);
    }

    const float margin = bestScore - currentScore;
    if (bestIndex == currentChordEventIndex
        || bestScore < 2.2f
        || margin < 1.25f
        || bestPhaseError + beatSeconds * 0.20 >= currentPhaseError)
    {
        pendingEvidenceCandidate = -1;
        pendingEvidenceCount = 0;
        return;
    }

    if (pendingEvidenceCandidate == bestIndex)
        ++pendingEvidenceCount;
    else
    {
        pendingEvidenceCandidate = bestIndex;
        pendingEvidenceCount = 1;
    }
    // Never jump from a single noisy FFT frame. This is the small beam's
    // commit rule: the same absolute score candidate must win twice.
    if (pendingEvidenceCount < 2)
        return;

    const double correctedBoundary =
        estimatedChordBoundaryPerformanceSeconds(bestIndex, evidenceTime);
    // Tempo observations use the actual detected onset over a long window.
    // TAB offsets are useful for phase, but subtracting a possibly wrong
    // intra-chord note offset biases BPM upward.
    updateTempoEstimate(bestIndex, evidenceTime);
    int skipped = 0;
    for (int index = nextPlayableChordEvent(currentChordEventIndex);
         index >= 0 && index != bestIndex;
         index = nextPlayableChordEvent(index))
        ++skipped;
    recoveredSkippedChordCount += skipped;
    currentChordEventIndex = bestIndex;
    nextChordEventIndex = nextPlayableChordEvent(bestIndex);
    currentChordPerformanceStartSeconds = correctedBoundary;
    songTimeSeconds =
        timeline[static_cast<size_t>(bestIndex)].startSeconds;
    secondsSinceChordMatch =
        performanceTimeSeconds - correctedBoundary;
    activeSecondsSinceChordMatch = 0.0;
    inactiveTailSeconds = 0.0;
    tempoAnchorEventIndex = bestIndex;
    tempoAnchorPerformanceSeconds = correctedBoundary;
    pendingEvidenceCandidate = -1;
    pendingEvidenceCount = 0;
    ++evidenceCorrectionCount;
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

    const double correctedBoundary =
        estimatedChordBoundaryPerformanceSeconds(
            matchedEventIndex, performanceTimeSeconds);
    if (!force)
        updateTempoEstimate(matchedEventIndex, performanceTimeSeconds);
    else
    {
        tempoAnchorEventIndex = matchedEventIndex;
        tempoAnchorPerformanceSeconds = correctedBoundary;
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
    if (performanceOriginEventIndex < 0)
    {
        performanceOriginEventIndex = matchedEventIndex;
        performanceOriginSeconds = correctedBoundary;
    }
    currentChordPerformanceStartSeconds = correctedBoundary;
    const auto& matchedEvent =
        song->getChordTimeline()[static_cast<size_t>(matchedEventIndex)];
    songTimeSeconds = matchedEvent.startSeconds;
    nextChordEventIndex = nextPlayableChordEvent(currentChordEventIndex);
    secondsSinceChordMatch =
        performanceTimeSeconds - correctedBoundary;
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

    // Stage 1 mora clock: every phrase keeps its own immutable target inside
    // the current chord segment. Do not derive this phrase's target from the
    // time at which the previous audio callback happened to emit a phrase.
    // That old dependency added up to one block of drift per mora. The small
    // collision guard below is only a final real-time safety net; a phrase
    // that misses its target is late locally and does not move later targets.
    if (lastStartedPhraseIndex >= 0
        && phraseToStart > lastStartedPhraseIndex)
    {
        if (performanceTimeSeconds
                - lastPhraseTriggerPerformanceSeconds
            < 0.050)
            return -1;
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
    float onsetStrength,
    const ChordEvidence* chordEvidence) noexcept
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
    if (chordEvidence != nullptr && chordEvidence->valid && running)
        applyChordEvidence(*chordEvidence);

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
