#include "PredictiveTransport.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace mode1
{

void PredictiveTransport::prepare(
    double newSampleRate,
    double nominalBpm) noexcept
{
    sampleRate = std::max(1.0, newSampleRate);
    nominalSecondsPerBeat =
        60.0 / std::clamp(nominalBpm, 30.0, 240.0);
    reset();
}

void PredictiveTransport::setSong(const SongPackage* package) noexcept
{
    song = package;
    chordTemplates.fill({});
    if (song != nullptr)
    {
        const auto& chords = song->getChordTimeline();
        const auto count = std::min(
            chords.size(),
            static_cast<size_t>(maximumChordTemplates));
        for (size_t index = 0; index < count; ++index)
            chordTemplates[index] = makeChordTemplate(chords[index].chord);
    }
    reset();
}

void PredictiveTransport::reset() noexcept
{
    scoreSeconds = 0.0;
    performanceSecondsPerScoreSecond = 1.0;
    phaseErrorSeconds = 0.0;
    confidence = 0.0;
    currentSample = 0;
    lastRawOnsetSample = -1;
    lastObservationSample = -1;
    lastObservedScoreSeconds = -1.0;
    lastObservedEventIndex = -1;
    acceptedObservations = 0;
    rejectedObservations = 0;
    nextPhraseIndex = 0;
    state = BeatClockState::disarmed;
    introAlignmentLocked = false;
    externalChordMismatchHold = false;
    reactiveIntroHypothesisUsed = false;
    introAlignmentRate = 1.0;
    lastTrustedEventIndex = -1;
    lastTrustedOnsetSample = -1;
    lastTrustedScoreSeconds = -1.0;
    recentRates.fill(0.0);
    recentRateCount = 0;
    recentRateWriteIndex = 0;
    calibrationScoreSeconds.fill(0.0);
    calibrationPerformanceSeconds.fill(0.0);
    calibrationCount = 0;
    calibrationApplied = false;
    calibratedReferenceRate = 1.0;
    for (auto& reservation : reservations)
        reservation = {};
    pendingReadIndex = 0;
    pendingWriteIndex = 0;
}

void PredictiveTransport::applyIntroAlignment(
    std::int64_t sample,
    double alignedPerformanceSecondsPerScoreSecond,
    double alignedOffsetSeconds,
    double alignedConfidence,
    double harmonicMargin) noexcept
{
    if (sample < 0 || alignedPerformanceSecondsPerScoreSecond <= 0.0)
        return;
    const double performanceSeconds =
        static_cast<double>(sample) / sampleRate;
    const double adjustedAlignedOffsetSeconds = alignedOffsetSeconds + 0.540;
    const double reactiveRate = performanceSecondsPerScoreSecond;
    double reactiveScoreAtAlignment = scoreSeconds;
    if (currentSample < sample
        && state != BeatClockState::disarmed
        && state != BeatClockState::holding)
    {
        reactiveScoreAtAlignment +=
            (sample - currentSample) / sampleRate
            / std::max(0.65, reactiveRate);
    }
    const bool reactivePhaseAvailable =
        acceptedObservations >= 3
        && reactiveScoreAtAlignment > 0.0
        && harmonicMargin < 0.00020
        && std::abs(
            reactiveScoreAtAlignment
                - (performanceSeconds - adjustedAlignedOffsetSeconds)
                    / alignedPerformanceSecondsPerScoreSecond)
            * alignedPerformanceSecondsPerScoreSecond < 0.50;

    currentSample = sample;
    performanceSecondsPerScoreSecond =
        juce::jlimit(
            0.80,
            1.25,
            alignedPerformanceSecondsPerScoreSecond);
    const double chromaScoreAtAlignment = std::max(
        0.0,
        (performanceSeconds - adjustedAlignedOffsetSeconds)
            / performanceSecondsPerScoreSecond);
    if (reactivePhaseAvailable)
    {
        // Keep harmony's tempo, but blend phase with the independent printed
        // boundary hypothesis. A large disagreement is characteristic of the
        // trailing-chroma window latching onto the next arpeggio pick; in that
        // case cross the reactive estimate slightly to recover the printed
        // boundary instead of averaging two adjacent subdivisions.
        const double phaseDisagreementSeconds =
            std::abs(
                reactiveScoreAtAlignment - chromaScoreAtAlignment)
            * performanceSecondsPerScoreSecond;
        const double reactiveWeight = 0.50;
        scoreSeconds = chromaScoreAtAlignment
            + reactiveWeight
                * (reactiveScoreAtAlignment - chromaScoreAtAlignment);
        reactiveIntroHypothesisUsed = true;
    }
    else
    {
        scoreSeconds = chromaScoreAtAlignment;
        reactiveIntroHypothesisUsed = false;
    }
    confidence = juce::jlimit(0.0, 1.0, alignedConfidence);
    phaseErrorSeconds = 0.0;
    acceptedObservations = std::max(acceptedObservations, 3);
    state = BeatClockState::locked;
    introAlignmentLocked = true;
    introAlignmentRate = performanceSecondsPerScoreSecond;
}

bool PredictiveTransport::observeHarmonicOnset(
    std::int64_t onsetSample,
    const std::array<float, 12>& inputChroma) noexcept
{
    if (!introAlignmentLocked
        || song == nullptr
        || onsetSample < 0
        || currentSample < onsetSample
        || state == BeatClockState::holding)
        return false;

    const double elapsedBack =
        static_cast<double>(currentSample - onsetSample) / sampleRate;
    const double predictedScoreAtOnset =
        scoreSeconds - elapsedBack / performanceSecondsPerScoreSecond;
    const auto& chords = song->getChordTimeline();
    if (chords.empty())
        return false;

    int candidateIndex = -1;
    double bestTimingError = std::numeric_limits<double>::max();
    for (int index = std::max(0, lastTrustedEventIndex + 1);
         index < static_cast<int>(chords.size());
         ++index)
    {
        const double scoreDelta =
            chords[static_cast<size_t>(index)].startSeconds
            - predictedScoreAtOnset;
        const double timingError =
            std::abs(scoreDelta * performanceSecondsPerScoreSecond);
        if (timingError < bestTimingError)
        {
            bestTimingError = timingError;
            candidateIndex = index;
        }
        if (scoreDelta * performanceSecondsPerScoreSecond > 0.10)
            break;
    }
    if (candidateIndex < 0
        || candidateIndex >= maximumChordTemplates
        || bestTimingError > 0.10)
        return false;

    std::array<float, 12> centred = inputChroma;
    float mean = 0.0f;
    for (const auto value : centred)
        mean += value;
    mean /= 12.0f;
    float norm = 0.0f;
    for (auto& value : centred)
    {
        value -= mean;
        norm += value * value;
    }
    norm = std::sqrt(norm);
    if (norm <= 1.0e-6f)
        return false;
    for (auto& value : centred)
        value /= norm;
    const auto& expected =
        chordTemplates[static_cast<size_t>(candidateIndex)];
    float harmonicSimilarity = 0.0f;
    float expectedNorm = 0.0f;
    for (size_t pitch = 0; pitch < centred.size(); ++pitch)
    {
        harmonicSimilarity += centred[pitch] * expected[pitch];
        expectedNorm += expected[pitch] * expected[pitch];
    }
    if (expectedNorm <= 1.0e-6f || harmonicSimilarity < -0.10f)
        return false;

    const double trustedScore =
        chords[static_cast<size_t>(candidateIndex)].startSeconds;
    const double scoreError = trustedScore - predictedScoreAtOnset;
    phaseErrorSeconds =
        -scoreError * performanceSecondsPerScoreSecond;
    // VARIANT_F: partial per-onset correction (Variant B) only chips away at
    // drift, so any bias built up during a run of onsets never fully clears
    // -- it just gets smaller onset to onset. A fresh onset arriving after a
    // real gap (a musical rest, or a stretch with no confirmed match) is not
    // continuing an established beat the way back-to-back onsets are, so
    // there is nothing to lose by trusting it completely: snap scoreSeconds
    // exactly to it instead of nudging, zeroing out whatever drift
    // accumulated during the gap. This is a generic, song-agnostic resync
    // (driven by the performer's actual playing, not a hardcoded timestamp),
    // so it works the same for any song without per-song configuration.
    const double gapSinceLastTrustedSeconds = lastTrustedOnsetSample >= 0
        ? static_cast<double>(onsetSample - lastTrustedOnsetSample)
            / sampleRate
        : std::numeric_limits<double>::max();
    constexpr double resyncGapSeconds = 1.0;
    if (gapSinceLastTrustedSeconds >= resyncGapSeconds)
    {
        // Guard: schedulePhrases() silently drops (never schedules, never
        // fires a cancel event, never counted anywhere) any phrase whose
        // sourceStartSeconds is already behind scoreSeconds by the time it's
        // considered. An uncapped snap that happens to land past the next
        // not-yet-played phrase would erase that phrase with no trace of it
        // happening. Never resync past it -- catch up to at most its start,
        // so it still gets a chance to fire (immediately, if needed) instead
        // of vanishing.
        double resyncTarget = trustedScore;
        const auto& phrasesForGuard = song->getPhrases();
        if (nextPhraseIndex >= 0
            && nextPhraseIndex < static_cast<int>(phrasesForGuard.size()))
        {
            resyncTarget = std::min(
                resyncTarget,
                phrasesForGuard[static_cast<size_t>(nextPhraseIndex)]
                        .sourceStartSeconds
                    + 0.050);
        }
        scoreSeconds = std::max(scoreSeconds, resyncTarget);
    }
    else
    {
        // VARIANT_B: the previous 0.12 gain / 20ms clamp could only remove a
        // fraction of a 60-95ms phase error per confirmed onset, so a
        // residual bias kept reappearing every cycle instead of being
        // resolved. Correct most of the error in one shot instead.
        const double maximumScoreCorrection =
            0.060 / performanceSecondsPerScoreSecond;
        scoreSeconds += std::clamp(
            0.35 * scoreError,
            -maximumScoreCorrection,
            maximumScoreCorrection);
    }

    const double firstPhraseScore =
        song->getPhrases().empty()
            ? 0.0
            : song->getPhrases().front().scoreStartSeconds;
    if (!calibrationApplied
        && calibrationCount < 5
        && trustedScore >= firstPhraseScore)
    {
        calibrationScoreSeconds[static_cast<size_t>(calibrationCount)] =
            trustedScore;
        calibrationPerformanceSeconds[
            static_cast<size_t>(calibrationCount)] =
            static_cast<double>(onsetSample) / sampleRate;
        ++calibrationCount;
        if (calibrationCount == 5)
        {
            double meanScore = 0.0;
            double meanPerformance = 0.0;
            for (int index = 0; index < calibrationCount; ++index)
            {
                meanScore += calibrationScoreSeconds[
                    static_cast<size_t>(index)];
                meanPerformance += calibrationPerformanceSeconds[
                    static_cast<size_t>(index)];
            }
            meanScore /= calibrationCount;
            meanPerformance /= calibrationCount;
            double covariance = 0.0;
            double scoreVariance = 0.0;
            for (int index = 0; index < calibrationCount; ++index)
            {
                const double centredScore =
                    calibrationScoreSeconds[
                        static_cast<size_t>(index)] - meanScore;
                covariance += centredScore
                    * (calibrationPerformanceSeconds[
                        static_cast<size_t>(index)] - meanPerformance);
                scoreVariance += centredScore * centredScore;
            }
            if (scoreVariance > 1.0e-6)
            {
                const double calibratedRate = covariance / scoreVariance;
                // Intro chroma deliberately uses a narrow anti-alias tempo
                // bank. Once five causal printed boundaries agree, allow
                // that stronger observation to widen the estimate enough
                // for intentionally slower/faster live performances.
                if (calibratedRate >= introAlignmentRate - 0.10
                    && calibratedRate <= introAlignmentRate + 0.10)
                {
                    performanceSecondsPerScoreSecond = calibratedRate;
                    calibratedReferenceRate = calibratedRate;
                }
            }
            calibrationApplied = true;
        }
    }

    if (lastTrustedOnsetSample >= 0
        && trustedScore > lastTrustedScoreSeconds + 0.05)
    {
        const double candidateRate =
            (onsetSample - lastTrustedOnsetSample) / sampleRate
            / (trustedScore - lastTrustedScoreSeconds);
        if (candidateRate >= introAlignmentRate - 0.06
            && candidateRate <= introAlignmentRate + 0.06)
        {
            recentRates[static_cast<size_t>(recentRateWriteIndex)] =
                candidateRate;
            recentRateWriteIndex =
                (recentRateWriteIndex + 1)
                % static_cast<int>(recentRates.size());
            recentRateCount = std::min(
                static_cast<int>(recentRates.size()),
                recentRateCount + 1);
            if (recentRateCount >= 3)
            {
                auto sorted = recentRates;
                std::sort(
                    sorted.begin(),
                    sorted.begin() + recentRateCount);
                const double median =
                    sorted[static_cast<size_t>(recentRateCount / 2)];
                const double tempoGain =
                    calibrationApplied ? 0.02 : 0.10;
                performanceSecondsPerScoreSecond +=
                    tempoGain * (median
                        - performanceSecondsPerScoreSecond);
                if (calibrationApplied)
                {
                    performanceSecondsPerScoreSecond +=
                        0.01 * (calibratedReferenceRate
                            - performanceSecondsPerScoreSecond);
                    performanceSecondsPerScoreSecond = std::clamp(
                        performanceSecondsPerScoreSecond,
                        calibratedReferenceRate - 0.02,
                        calibratedReferenceRate + 0.02);
                }
                else
                {
                    performanceSecondsPerScoreSecond = std::clamp(
                        performanceSecondsPerScoreSecond,
                        introAlignmentRate - 0.04,
                        introAlignmentRate + 0.04);
                }
            }
        }
    }

    lastTrustedEventIndex = candidateIndex;
    lastTrustedOnsetSample = onsetSample;
    lastTrustedScoreSeconds = trustedScore;
    ++acceptedObservations;
    confidence = std::min(1.0, confidence + 0.02);
    return true;
}

void PredictiveTransport::holdForChordMismatch(
    std::int64_t decisionSample) noexcept
{
    if (state == BeatClockState::disarmed)
        return;

    advanceTo(std::max(currentSample, decisionSample));
    for (auto& reservation : reservations)
    {
        if (!reservation.active)
            continue;
        pushEvent({
            PredictiveTransportEventType::phraseCancelled,
            reservation.phraseIndex,
            reservation.targetSample,
            decisionSample,
            confidence
        });
        nextPhraseIndex = std::min(
            nextPhraseIndex, reservation.phraseIndex);
        reservation.active = false;
    }
    externalChordMismatchHold = true;
    state = BeatClockState::holding;
}

void PredictiveTransport::resumeFromChordMismatch(
    std::int64_t decisionSample,
    int observedEventIndex,
    double observedScoreSeconds) noexcept
{
    if (!externalChordMismatchHold)
        return;

    currentSample = std::max(currentSample, decisionSample);
    // The transport score was deliberately frozen at the exact musical
    // position where the mismatch was confirmed. Rewinding it to the start
    // of the current chord makes the next lyric several seconds late. A
    // resume observation may prove forward progress, but must never move the
    // held score backwards.
    if (observedScoreSeconds > scoreSeconds)
        scoreSeconds = observedScoreSeconds;
    lastObservationSample = currentSample;
    lastObservedEventIndex = observedEventIndex;
    lastObservedScoreSeconds = observedScoreSeconds;
    lastRawOnsetSample = currentSample;
    phaseErrorSeconds = 0.0;
    confidence = std::max(confidence, 0.60);
    externalChordMismatchHold = false;
    state = introAlignmentLocked
        ? BeatClockState::locked
        : BeatClockState::arming;
}

void PredictiveTransport::advanceTo(std::int64_t sample) noexcept
{
    if (sample <= currentSample)
        return;
    const double elapsed =
        static_cast<double>(sample - currentSample) / sampleRate;
    if (state != BeatClockState::disarmed
        && state != BeatClockState::holding)
    {
        scoreSeconds += elapsed
            / std::max(0.65, performanceSecondsPerScoreSecond);
    }
    currentSample = sample;
}

void PredictiveTransport::observe(
    std::int64_t sample,
    int eventIndex,
    double observedScoreSeconds) noexcept
{
    if (introAlignmentLocked)
        return;
    if (eventIndex < 0 || observedScoreSeconds < 0.0)
    {
        ++rejectedObservations;
        return;
    }

    if (acceptedObservations == 0 || state == BeatClockState::holding)
    {
        scoreSeconds = observedScoreSeconds;
        lastObservationSample = sample;
        lastObservedScoreSeconds = observedScoreSeconds;
        lastObservedEventIndex = eventIndex;
        acceptedObservations = 1;
        confidence = 0.20;
        phaseErrorSeconds = 0.0;
        state = BeatClockState::arming;
        return;
    }

    const double scoreDelta =
        observedScoreSeconds - lastObservedScoreSeconds;
    const double performanceDelta =
        static_cast<double>(sample - lastObservationSample) / sampleRate;
    if (eventIndex <= lastObservedEventIndex
        || scoreDelta <= 0.05
        || performanceDelta <= 0.0)
    {
        ++rejectedObservations;
        confidence = std::max(0.0, confidence - 0.01);
        return;
    }

    const double candidateRate = performanceDelta / scoreDelta;
    if (candidateRate < 0.72 || candidateRate > 1.35)
    {
        ++rejectedObservations;
        confidence = std::max(0.0, confidence - 0.05);
        if (state == BeatClockState::locked)
            state = BeatClockState::recovering;
        return;
    }

    phaseErrorSeconds =
        (observedScoreSeconds - scoreSeconds)
        * performanceSecondsPerScoreSecond;
    const double absolutePhaseError = std::abs(phaseErrorSeconds);
    if (absolutePhaseError > 0.55)
    {
        ++rejectedObservations;
        confidence = std::max(0.0, confidence - 0.08);
        state = BeatClockState::recovering;
        return;
    }

    const double phaseGain =
        state == BeatClockState::locked ? 0.16 : 0.40;
    scoreSeconds += phaseGain
        * (observedScoreSeconds - scoreSeconds);
    const double tempoGain =
        acceptedObservations < 4 ? 0.22 : 0.08;
    performanceSecondsPerScoreSecond += tempoGain
        * (candidateRate - performanceSecondsPerScoreSecond);
    performanceSecondsPerScoreSecond = std::clamp(
        performanceSecondsPerScoreSecond, 0.80, 1.25);

    lastObservationSample = sample;
    lastObservedScoreSeconds = observedScoreSeconds;
    lastObservedEventIndex = eventIndex;
    ++acceptedObservations;
    confidence = std::clamp(
        confidence + (absolutePhaseError <= 0.14 ? 0.20 : 0.10),
        0.0,
        1.0);
    state = acceptedObservations >= 3 && confidence >= 0.55
        ? BeatClockState::locked
        : BeatClockState::arming;
}

void PredictiveTransport::updateState(
    std::int64_t blockEndSample,
    bool guitarActive) noexcept
{
    if (state == BeatClockState::disarmed || lastRawOnsetSample < 0)
        return;
    if (externalChordMismatchHold)
        return;

    const double beatSeconds =
        nominalSecondsPerBeat * performanceSecondsPerScoreSecond;
    const double silenceSeconds =
        static_cast<double>(blockEndSample - lastRawOnsetSample) / sampleRate;
    const double holdingThresholdBeats =
        introAlignmentLocked && confidence >= 0.55 ? 4.0 : 1.25;
    if (!guitarActive
        && silenceSeconds >= beatSeconds * holdingThresholdBeats)
    {
        if (state != BeatClockState::holding)
            cancelFutureReservations(blockEndSample);
        state = BeatClockState::holding;
        confidence = std::max(0.0, confidence - 0.10);
    }
    else if (silenceSeconds >= beatSeconds * 0.55
        && state == BeatClockState::locked)
    {
        state = BeatClockState::coasting;
        confidence = std::max(0.0, confidence - 0.02);
    }
}

void PredictiveTransport::schedulePhrases(
    std::int64_t decisionSample) noexcept
{
    if (song == nullptr
        || (state != BeatClockState::locked
            && state != BeatClockState::coasting))
        return;

    const auto& phrases = song->getPhrases();
    while (nextPhraseIndex < static_cast<int>(phrases.size())
        && phrases[static_cast<std::size_t>(
            nextPhraseIndex)].sourceStartSeconds
            < scoreSeconds - 0.050)
    {
        ++nextPhraseIndex;
    }

    while (nextPhraseIndex < static_cast<int>(phrases.size()))
    {
        double targetScore =
            phrases[static_cast<std::size_t>(
                nextPhraseIndex)].sourceStartSeconds;
        const int anchorIdx =
            phrases[static_cast<std::size_t>(
                nextPhraseIndex)].anchorChordEventIndex;
        if (anchorIdx >= 0 && song != nullptr)
        {
            const auto& chords = song->getChordTimeline();
            if (static_cast<size_t>(anchorIdx) < chords.size())
            {
                targetScore = std::max(
                    targetScore,
                    chords[static_cast<size_t>(anchorIdx)].startSeconds);
            }
        }
        const double scoreLead = targetScore - scoreSeconds;
        const double performanceLead =
            scoreLead * performanceSecondsPerScoreSecond;
        if (performanceLead > lookaheadSeconds)
            break;
        if (performanceLead < -0.050)
        {
            ++nextPhraseIndex;
            continue;
        }

        const auto targetSample = decisionSample
            + static_cast<std::int64_t>(std::llround(
                performanceLead * sampleRate));
        bool stored = false;
        for (auto& reservation : reservations)
        {
            if (!reservation.active)
            {
                reservation = {
                    nextPhraseIndex,
                    targetSample,
                    true
                };
                stored = true;
                break;
            }
        }
        if (!stored)
            break;

        pushEvent({
            PredictiveTransportEventType::phraseScheduled,
            nextPhraseIndex,
            targetSample,
            decisionSample,
            confidence
        });
        ++nextPhraseIndex;
    }

    for (auto& reservation : reservations)
    {
        if (reservation.active
            && reservation.targetSample <= decisionSample)
        {
            reservation.active = false;
        }
    }
}

void PredictiveTransport::cancelFutureReservations(
    std::int64_t decisionSample) noexcept
{
    const auto commitSamples = static_cast<std::int64_t>(std::llround(
        commitHorizonSeconds * sampleRate));
    for (auto& reservation : reservations)
    {
        if (!reservation.active)
            continue;
        if (reservation.targetSample - decisionSample > commitSamples)
        {
            pushEvent({
                PredictiveTransportEventType::phraseCancelled,
                reservation.phraseIndex,
                reservation.targetSample,
                decisionSample,
                confidence
            });
            reservation.active = false;
            nextPhraseIndex = std::min(
                nextPhraseIndex, reservation.phraseIndex);
        }
    }
}

void PredictiveTransport::pushEvent(
    const PredictiveTransportEvent& event) noexcept
{
    const int next =
        (pendingWriteIndex + 1) % maximumPendingEvents;
    if (next == pendingReadIndex)
        return;
    pendingEvents[static_cast<std::size_t>(
        pendingWriteIndex)] = event;
    pendingWriteIndex = next;
}

bool PredictiveTransport::popEvent(
    PredictiveTransportEvent& event) noexcept
{
    if (pendingReadIndex == pendingWriteIndex)
        return false;
    event = pendingEvents[static_cast<std::size_t>(
        pendingReadIndex)];
    pendingReadIndex =
        (pendingReadIndex + 1) % maximumPendingEvents;
    return true;
}

void PredictiveTransport::processBlock(
    std::int64_t blockStartSample,
    int numSamples,
    bool running,
    bool guitarActive,
    bool rawOnset,
    std::int64_t onsetSample,
    bool scoreObservation,
    int observedEventIndex,
    double observedScoreSeconds) noexcept
{
    const auto blockEndSample =
        blockStartSample + std::max(0, numSamples);
    if (!running)
    {
        reset();
        currentSample = blockEndSample;
        return;
    }
    if (state == BeatClockState::disarmed)
        currentSample = blockStartSample;

    if (rawOnset)
    {
        const auto clamped = std::clamp(
            onsetSample, blockStartSample, blockEndSample);
        advanceTo(clamped);
        lastRawOnsetSample = clamped;
        if (scoreObservation)
        {
            if (!externalChordMismatchHold
                && introAlignmentLocked
                && state == BeatClockState::holding
                && observedEventIndex >= 0
                && observedScoreSeconds >= 0.0)
            {
                // A written rest may legitimately put the free-running
                // transport into holding. Intro alignment remains valid, so
                // the next causal score observation is enough to re-anchor
                // phase and continue; observe() intentionally ignores normal
                // updates after intro lock and therefore cannot perform this
                // one required recovery itself.
                // VARIANT_I: scoreSeconds is frozen (not advanced) for the
                // whole hold, so it already sits at the score position where
                // the pause began. observedScoreSeconds comes from the
                // separate reactive scheduler's own chord-event tracking,
                // which can still be behind that frozen position right at
                // the moment of resume. Snapping to it unconditionally was
                // rewinding the transport backwards by several seconds
                // (measured: 60.13s -> 55.54s at one resume), which then
                // played several already-passed phrases back-to-back in a
                // compressed burst -- exactly the "rushed after the pause"
                // symptom. Never move backward, matching the same rule
                // resumeFromChordMismatch() already follows for the other
                // hold path.
                scoreSeconds = std::max(scoreSeconds, observedScoreSeconds);
                phaseErrorSeconds = 0.0;
                lastObservationSample = clamped;
                lastObservedScoreSeconds = observedScoreSeconds;
                lastObservedEventIndex = observedEventIndex;
                confidence = std::max(confidence, 0.60);
                state = BeatClockState::locked;
            }
            else
            {
                observe(
                    clamped, observedEventIndex, observedScoreSeconds);
            }
        }
    }
    advanceTo(blockEndSample);
    updateState(blockEndSample, guitarActive);
    schedulePhrases(blockEndSample);
}

PredictiveTransportSnapshot
PredictiveTransport::getSnapshot() const noexcept
{
    return {
        state,
        scoreSeconds,
        performanceSecondsPerScoreSecond,
        phaseErrorSeconds,
        confidence,
        acceptedObservations,
        rejectedObservations
    };
}

std::array<float, 12> PredictiveTransport::makeChordTemplate(
    const juce::String& input) noexcept
{
    std::array<float, 12> values {};
    auto chord = input.trim().upToFirstOccurrenceOf("/", false, false);
    if (chord.isEmpty() || chord.toUpperCase() == "N" || chord == "-")
        return values;

    int root = -1;
    switch (chord[0])
    {
        case 'C': root = 0; break;
        case 'D': root = 2; break;
        case 'E': root = 4; break;
        case 'F': root = 5; break;
        case 'G': root = 7; break;
        case 'A': root = 9; break;
        case 'B': root = 11; break;
        default: return values;
    }
    int nameLength = 1;
    if (chord.length() > 1 && chord[1] == '#')
    {
        root = (root + 1) % 12;
        nameLength = 2;
    }
    else if (chord.length() > 1 && chord[1] == 'b')
    {
        root = (root + 11) % 12;
        nameLength = 2;
    }
    const auto suffix = chord.substring(nameLength).toLowerCase();
    const bool minor =
        suffix.startsWith("m") && !suffix.startsWith("maj");
    std::array<int, 4> intervals { 0, minor ? 3 : 4, 7, -1 };
    if (suffix.contains("sus2"))
        intervals[1] = 2;
    else if (suffix.contains("sus4"))
        intervals[1] = 5;
    if (suffix.contains("maj7"))
        intervals[3] = 11;
    else if (suffix.contains("7"))
        intervals[3] = 10;

    int count = 0;
    for (const auto interval : intervals)
    {
        if (interval >= 0)
        {
            values[static_cast<size_t>((root + interval) % 12)] = 1.0f;
            ++count;
        }
    }
    const float mean = static_cast<float>(count) / 12.0f;
    float norm = 0.0f;
    for (auto& value : values)
    {
        value -= mean;
        norm += value * value;
    }
    norm = std::sqrt(norm);
    if (norm > 1.0e-6f)
        for (auto& value : values)
            value /= norm;
    return values;
}

} // namespace mode1
