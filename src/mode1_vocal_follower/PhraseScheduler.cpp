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
    clearTimingAnchors();
    acceptedTimingAnchorCount = 0;
    rejectedTimingAnchorCount = 0;
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

void PhraseScheduler::clearTimingAnchors() noexcept
{
    timingAnchorCount = 0;
    timingAnchors.fill({});
}

bool PhraseScheduler::hasReliableTimingSlope() const noexcept
{
    const int minimumAnchors =
        song != nullptr && song->hasTabTracking() ? 4 : 2;
    if (timingAnchorCount < minimumAnchors)
        return false;
    const double scoreSpan =
        timingAnchors[static_cast<size_t>(timingAnchorCount - 1)].scoreSeconds
        - timingAnchors.front().scoreSeconds;
    const double minimumBeats =
        song != nullptr && song->hasTabTracking() ? 8.0 : 2.0;
    return scoreSpan >= scoreBeatSeconds() * minimumBeats;
}

double PhraseScheduler::performanceSecondsPerScoreSecond() const noexcept
{
    if (!hasReliableTimingSlope())
        return 1.0 / juce::jlimit(0.80, 1.25, tempoScale);

    // The median of all sufficiently separated anchor-pair slopes is robust
    // to a single chord whose detected start/end boundary was split badly.
    // Eight anchors produce at most 28 values, so this stays allocation-free
    // in the audio callback.
    std::array<double, 28> slopes {};
    int slopeCount = 0;
    const double minimumSpan = scoreBeatSeconds()
        * (song != nullptr && song->hasTabTracking() ? 8.0 : 2.0);
    for (int left = 0; left < timingAnchorCount; ++left)
    {
        for (int right = left + 1; right < timingAnchorCount; ++right)
        {
            const double scoreDelta =
                timingAnchors[static_cast<size_t>(right)].scoreSeconds
                - timingAnchors[static_cast<size_t>(left)].scoreSeconds;
            const double performanceDelta =
                timingAnchors[static_cast<size_t>(right)].performanceSeconds
                - timingAnchors[static_cast<size_t>(left)].performanceSeconds;
            if (scoreDelta >= minimumSpan && performanceDelta > 0.0)
                slopes[static_cast<size_t>(slopeCount++)] =
                    performanceDelta / scoreDelta;
        }
    }
    if (slopeCount == 0)
        return 1.0 / juce::jlimit(0.80, 1.25, tempoScale);
    std::sort(slopes.begin(), slopes.begin() + slopeCount);
    const double median = slopeCount % 2 == 0
        ? 0.5 * (
            slopes[static_cast<size_t>(slopeCount / 2 - 1)]
            + slopes[static_cast<size_t>(slopeCount / 2)])
        : slopes[static_cast<size_t>(slopeCount / 2)];
    return juce::jlimit(0.80, 1.25, median);
}

double PhraseScheduler::mapScoreTimeToPerformanceSeconds(
    double scoreSeconds) const noexcept
{
    if (timingAnchorCount == 0)
        return performanceTimeSeconds;

    const double slope = performanceSecondsPerScoreSecond();
    if (!hasReliableTimingSlope())
    {
        const auto& first = timingAnchors.front();
        return first.performanceSeconds
            + slope * (scoreSeconds - first.scoreSeconds);
    }
    const auto affinePrediction = [this, slope](
        double targetScoreSeconds) noexcept
    {
        std::array<double, maximumTimingAnchors> offsets {};
        for (int index = 0; index < timingAnchorCount; ++index)
        {
            const auto& anchor =
                timingAnchors[static_cast<size_t>(index)];
            offsets[static_cast<size_t>(index)] =
                anchor.performanceSeconds - slope * anchor.scoreSeconds;
        }
        std::sort(
            offsets.begin(),
            offsets.begin() + timingAnchorCount);
        const double intercept = timingAnchorCount % 2 == 0
            ? 0.5 * (
                offsets[static_cast<size_t>(timingAnchorCount / 2 - 1)]
                + offsets[static_cast<size_t>(timingAnchorCount / 2)])
            : offsets[static_cast<size_t>(timingAnchorCount / 2)];
        return intercept + slope * targetScoreSeconds;
    };

    const auto& first = timingAnchors.front();
    if (timingAnchorCount == 1 || scoreSeconds <= first.scoreSeconds)
        return affinePrediction(scoreSeconds);

    // Once both causal anchors are known, map directly between their
    // absolute times. No per-chord error can accumulate beyond this span.
    for (int index = 1; index < timingAnchorCount; ++index)
    {
        const auto& right = timingAnchors[static_cast<size_t>(index)];
        if (scoreSeconds > right.scoreSeconds)
            continue;
        const auto& left =
            timingAnchors[static_cast<size_t>(index - 1)];
        const double scoreSpan = right.scoreSeconds - left.scoreSeconds;
        if (scoreSpan <= 1.0e-6)
            return right.performanceSeconds;
        const double phase = juce::jlimit(
            0.0,
            1.0,
            (scoreSeconds - left.scoreSeconds) / scoreSpan);
        return left.performanceSeconds
            + phase * (
                right.performanceSeconds - left.performanceSeconds);
    }

    // Live playback cannot see the future right anchor. Extrapolate from the
    // recent accepted boundaries with a robust slope and median phase.
    return affinePrediction(scoreSeconds);
}

bool PhraseScheduler::recordTimingAnchor(
    int eventIndex,
    double boundaryPerformanceSeconds,
    bool allowPauseRebase) noexcept
{
    if (song == nullptr || eventIndex < 0)
        return false;
    const auto& timeline = song->getChordTimeline();
    if (eventIndex >= static_cast<int>(timeline.size()))
        return false;

    const double scoreSeconds =
        timeline[static_cast<size_t>(eventIndex)].startSeconds;
    if (allowPauseRebase && timingAnchorCount > 0)
    {
        // A real pause is part of the performance. Start a new local affine
        // segment rather than interpreting the pause as a bad chord or
        // letting it permanently skew the song-wide tempo.
        clearTimingAnchors();
    }

    if (timingAnchorCount > 0)
    {
        const auto& last =
            timingAnchors[static_cast<size_t>(timingAnchorCount - 1)];
        if (eventIndex <= last.eventIndex
            || scoreSeconds <= last.scoreSeconds
            || boundaryPerformanceSeconds <= last.performanceSeconds)
        {
            ++rejectedTimingAnchorCount;
            return false;
        }

        const double scoreDelta = scoreSeconds - last.scoreSeconds;
        const double performanceDelta =
            boundaryPerformanceSeconds - last.performanceSeconds;
        const double intervalSlope = performanceDelta / scoreDelta;
        const double expectedSlope =
            performanceSecondsPerScoreSecond();
        const double relativeDurationRatio =
            intervalSlope / std::max(1.0e-6, expectedSlope);
        const double residual = boundaryPerformanceSeconds
            - mapScoreTimeToPerformanceSeconds(scoreSeconds);

        // These are confidence gates, not a fixed BPM. A genuinely slower
        // player produces a consistent sequence of accepted slopes, while a
        // single boundary that steals most of its neighbour is discarded.
        if (relativeDurationRatio < 0.65
            || relativeDurationRatio > 1.50
            || std::abs(residual) > 0.40)
        {
            ++rejectedTimingAnchorCount;
            return false;
        }
    }

    if (timingAnchorCount == maximumTimingAnchors)
    {
        std::move(
            timingAnchors.begin() + 1,
            timingAnchors.end(),
            timingAnchors.begin());
        --timingAnchorCount;
    }
    timingAnchors[static_cast<size_t>(timingAnchorCount++)] = {
        eventIndex, scoreSeconds, boundaryPerformanceSeconds
    };
    ++acceptedTimingAnchorCount;
    return true;
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
            bestScoreFromOrigin
                / juce::jlimit(0.80, 1.25, tempoScale);
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

double PhraseScheduler::estimatedChordBoundaryPerformanceSeconds(
    int eventIndex,
    double onsetPerformanceSeconds) const noexcept
{
    juce::ignoreUnused(eventIndex);
    // The detector onset is the causal evidence that the player actually
    // supplied. Subtracting a TAB note offset here made a correct ~1.0x
    // performance look about 1.07x fast and was itself a tempo bias.
    return onsetPerformanceSeconds;
}

void PhraseScheduler::updateTempoEstimate(
    int matchedEventIndex,
    double matchedPerformanceSeconds) noexcept
{
    if (song == nullptr
        || matchedEventIndex < 0)
        return;

    const double boundaryPerformanceSeconds =
        matchedPerformanceSeconds >= 0.0
        ? matchedPerformanceSeconds
        : performanceTimeSeconds;
    const double beatPerformanceSeconds =
        scoreBeatSeconds() * performanceSecondsPerScoreSecond();
    const bool realPause =
        inactiveTailSeconds > beatPerformanceSeconds * 0.75
        && timingAnchorCount > 0
        && boundaryPerformanceSeconds
            - mapScoreTimeToPerformanceSeconds(
                song->getChordTimeline()[
                    static_cast<size_t>(matchedEventIndex)].startSeconds)
            > beatPerformanceSeconds * 0.55;
    recordTimingAnchor(
        matchedEventIndex,
        boundaryPerformanceSeconds,
        realPause);

    const auto& timeline = song->getChordTimeline();
    if (song->hasTabTracking() && performanceOriginEventIndex >= 0)
    {
        const double scoreFromOrigin =
            timeline[static_cast<size_t>(matchedEventIndex)].startSeconds
            - timeline[static_cast<size_t>(
                performanceOriginEventIndex)].startSeconds;
        const double performanceFromOrigin =
            boundaryPerformanceSeconds - performanceOriginSeconds;
        if (scoreFromOrigin < scoreBeatSeconds() * 12.0
            || performanceFromOrigin <= 0.10)
            return;
        const double observation =
            scoreFromOrigin / performanceFromOrigin;
        if (observation < 0.75 || observation > 1.30)
            return;
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
        tempoAnchorPerformanceSeconds = boundaryPerformanceSeconds;
        return;
    }
    if (matchedEventIndex <= tempoAnchorEventIndex)
        return;

    const double scoreDelta =
        timeline[static_cast<size_t>(matchedEventIndex)].startSeconds
        - timeline[static_cast<size_t>(tempoAnchorEventIndex)].startSeconds;
    const double elapsedSeconds =
        boundaryPerformanceSeconds - tempoAnchorPerformanceSeconds;
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
    tempoAnchorPerformanceSeconds = boundaryPerformanceSeconds;
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
    updateTempoEstimate(bestIndex, correctedBoundary);
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
    if (force)
    {
        // A pedal/Space trigger is an explicit absolute position anchor. It
        // may be pressed without real-time waiting in tests or rehearsal, so
        // rebase directly instead of interpreting its interval as tempo.
        recordTimingAnchor(
            matchedEventIndex, correctedBoundary, true);
    }
    else
    {
        updateTempoEstimate(matchedEventIndex, correctedBoundary);
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

    // Map every immutable source target from absolute causal anchors. A
    // song-wide tempo estimate still guides score-position selection, but it
    // no longer decides the local in-chord vocal target by itself.
    const double targetPerformanceSeconds =
        mapScoreTimeToPerformanceSeconds(nextPhrase.sourceStartSeconds)
        - guitarVocalLeadSeconds;
    if (performanceTimeSeconds < targetPerformanceSeconds)
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
