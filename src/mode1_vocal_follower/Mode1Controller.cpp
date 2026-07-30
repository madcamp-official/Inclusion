#include "Mode1Controller.h"

#include <limits>

namespace mode1
{

void Mode1Controller::prepare(double sampleRate, int maximumBlockSize)
{
    controllerSampleRate = std::max(1.0, sampleRate);
    maximumInternalBlockSize = std::max(
        16,
        static_cast<int>(std::llround(
            controllerSampleRate * 128.0 / 48'000.0)));
    processedSamples = 0;
    inputLatencySamples = 0;
    tracedChordEventIndex = -1;
    beatClockObservedScoreEventIndex = -1;
    realtimeTrace.reset();
    phrasePlayer.prepare(sampleRate, maximumBlockSize);
    scheduler.prepare(sampleRate);
    onsetTracker.prepare(sampleRate);
    chordTracker.prepare(sampleRate);
    beatClock.prepare(sampleRate, 120.0);
    predictiveScheduler.prepare(sampleRate);
    predictiveTransport.prepare(sampleRate, 120.0);
    introChromaAligner.prepare(sampleRate);
    continuousChromaExtractor.prepare(sampleRate);
    chordMismatchGate.reset();
}

void Mode1Controller::setInputLatencySeconds(double seconds) noexcept
{
    inputLatencySamples = static_cast<std::int64_t>(std::llround(
        std::max(0.0, seconds) * controllerSampleRate));
}

bool Mode1Controller::loadSongPackage(
    const juce::File& packageFile,
    juce::String& error)
{
    SongPackage candidate;
    if (!candidate.loadFromFile(packageFile, error))
        return false;

    songPackage = std::move(candidate);
    const int defaultExpression =
        songPackage.getDefaultExpressionStrength();
    if (!phrasePlayer.loadKeyAnchor(
            songPackage,
            songPackage.getBaseKeyShift(),
            error,
            defaultExpression))
    {
        songPackage.clear();
        return false;
    }

    scheduler.setSong(&songPackage);
    beatClock.prepare(controllerSampleRate, songPackage.getScoreBpm());
    predictiveScheduler.setSong(&songPackage);
    predictiveTransport.prepare(
        controllerSampleRate, songPackage.getScoreBpm());
    predictiveTransport.setSong(&songPackage);
    introChromaAligner.prepare(controllerSampleRate);
    introChromaAligner.setSong(&songPackage);
    continuousChromaExtractor.prepare(controllerSampleRate);
    introAlignmentApplied = false;
    lastPhysicalOnsetForChordAnalysis = -1;
    lastChordOnsetScoreEventForAnalysis = -1;
    chordMismatchGate.reset();
    chordMismatchPausedForUi.store(false);
    releaseChordMismatchPauseRequested.store(false);
    vocalDurationScale.store(1.0);
    onsetTracker.setArpeggioMode(songPackage.hasTabTracking());
    manualKeyShift.store(0);
    selectedKeyAnchor.store(songPackage.getBaseKeyShift());
    performanceKeyOffset.store(0);
    performanceKeyOffsetLocked.store(false);
    currentChordEventForUi.store(-1);
    manualKeyRevision.fetch_add(1);
    expressionStrength.store(defaultExpression);
    phrasePlayer.setExpressionStrength(defaultExpression);
    lastStartedPhrase.store(-1);
    pendingCommittedPhrase = -1;
    pendingCommitBlocks = 0;
    vocalRest.store(false);
    performanceRunning.store(false);
    return true;
}

void Mode1Controller::reset() noexcept
{
    scheduler.reset();
    phrasePlayer.stop();
    onsetTracker.reset();
    chordTracker.reset();
    manualTrigger.store(false);
    automaticPlayback.store(false);
    performanceRunning.store(false);
    pendingVirtualChordRoot.store(-1);
    guitarActive.store(false);
    lastOnset.store(false);
    vocalRest.store(false);
    guitarRms.store(0.0f);
    lastStartedPhrase.store(-1);
    pendingCommittedPhrase = -1;
    pendingCommitBlocks = 0;
    detectedChordRoot.store(-1);
    detectedChordBass.store(-1);
    detectedChordQuality.store(
        static_cast<int>(ChordQuality::unknown));
    performanceKeyOffset.store(0);
    performanceKeyOffsetLocked.store(false);
    currentChordEventForUi.store(-1);
    manualKeyShift.store(0);
    selectedKeyAnchor.store(songPackage.getBaseKeyShift());
    manualKeyRevision.fetch_add(1);
    pendingPitchShift = 0;
    pendingPitchShiftCount = 0;
    pendingPitchShiftLastEventIndex = -1;
    pendingPitchShiftDistinctEventCount = 0;
    processedSamples = 0;
    tracedChordEventIndex = -1;
    beatClockObservedScoreEventIndex = -1;
    realtimeTrace.reset();
    beatClock.reset();
    predictiveScheduler.reset();
    predictiveTransport.reset();
    introChromaAligner.reset();
    continuousChromaExtractor.reset();
    introAlignmentApplied = false;
    lastPhysicalOnsetForChordAnalysis = -1;
    lastChordOnsetScoreEventForAnalysis = -1;
    chordMismatchGate.reset();
    chordMismatchPausedForUi.store(false);
    releaseChordMismatchPauseRequested.store(false);
    vocalDurationScale.store(1.0);
    tracedBeatClockState = BeatClockState::disarmed;
    tracedPredictiveTransportState = BeatClockState::disarmed;
}

void Mode1Controller::processBlock(
    const float* guitarInput,
    float* outputLeft,
    float* outputRight,
    int numSamples) noexcept
{
    if (numSamples <= 0)
        return;

    bool anyOnset = false;
    for (int offset = 0; offset < numSamples; offset += maximumInternalBlockSize)
    {
        const int count =
            std::min(maximumInternalBlockSize, numSamples - offset);
        processSubBlock(
            guitarInput != nullptr ? guitarInput + offset : nullptr,
            outputLeft + offset,
            outputRight + offset,
            count);
        anyOnset = anyOnset || lastOnset.load(std::memory_order_relaxed);
    }
    lastOnset.store(anyOnset, std::memory_order_relaxed);
}

void Mode1Controller::processSubBlock(
    const float* guitarInput,
    float* outputLeft,
    float* outputRight,
    int numSamples) noexcept
{
    const std::int64_t blockStartSample = processedSamples;
    const std::int64_t blockEndSample = blockStartSample + numSamples;
    const auto keyRevision = manualKeyRevision.load();
    if (keyRevision != observedManualKeyRevision)
    {
        observedManualKeyRevision = keyRevision;
        pendingPitchShift = 0;
        pendingPitchShiftCount = 0;
        pendingPitchShiftLastEventIndex = -1;
        pendingPitchShiftDistinctEventCount = 0;
        performanceKeyOffset.store(0);
        performanceKeyOffsetLocked.store(false);
    }

    const bool physicalOnset =
        onsetTracker.processBlock(guitarInput, numSamples);
    const int virtualChordRoot =
        pendingVirtualChordRoot.exchange(-1);
    const bool virtualOnset = virtualChordRoot >= 0;
    const bool autoPlaying = automaticPlayback.load();
    const bool transportRunning =
        performanceRunning.load() || autoPlaying;
    if (releaseChordMismatchPauseRequested.exchange(false)
        && chordMismatchGate.isPaused())
    {
        releaseChordMismatchPause(
            blockEndSample,
            scheduler.getCurrentChordEventIndex());
    }
    const bool chordMismatchPausedAtBlockStart =
        chordMismatchGate.isPaused();
    const bool onset =
        !autoPlaying && (physicalOnset || virtualOnset);
    if (realtimeTraceEnabled.load(std::memory_order_relaxed) && onset)
    {
        const int onsetOffset = physicalOnset
            ? onsetTracker.getLastOnsetSampleOffset()
            : 0;
        realtimeTrace.push({
            RealtimeTraceType::onsetDetected,
            std::max<std::int64_t>(
                0,
                blockStartSample + onsetOffset - inputLatencySamples),
            blockEndSample,
            -1,
            physicalOnset
                ? onsetTracker.getLastOnsetStrength()
                : 1.0f,
            physicalOnset ? 1u : 2u
        });
    }
    ChordDetection chordDetection;
    bool hasChordDetection = false;
    if (virtualOnset && !autoPlaying)
    {
        chordDetection.valid = true;
        chordDetection.rootPitchClass = virtualChordRoot;
        chordDetection.bassPitchClass = virtualChordRoot;
        chordDetection.quality = ChordQuality::major;
        chordDetection.confidence = 1.0f;
        hasChordDetection = true;
    }
    else
    {
        hasChordDetection = chordTracker.processBlock(
            guitarInput,
            numSamples,
            physicalOnset && !autoPlaying,
            chordDetection);
    }
    if (hasChordDetection
        && chordDetection.valid
        && !chordMismatchGate.isPaused())
        updateTransposition(chordDetection);
    const bool manual =
        !autoPlaying && manualTrigger.exchange(false);
    guitarActive.store(onsetTracker.isActive());
    guitarRms.store(onsetTracker.getCurrentRms());
    lastOnset.store(onset);

    ChordEvidence chordEvidence;
    if (hasChordDetection && chordDetection.valid)
    {
        chordEvidence.valid = true;
        chordEvidence.rootPitchClass = chordDetection.rootPitchClass;
        chordEvidence.bassPitchClass = chordDetection.bassPitchClass;
        chordEvidence.quality = static_cast<int>(chordDetection.quality);
        chordEvidence.pitchClassMask = chordDetection.pitchClassMask;
        chordEvidence.confidence = chordDetection.confidence;
    }
    const auto previousClockSnapshot = beatClock.getSnapshot();
    const bool predictiveModeActive =
        followerMode.load(std::memory_order_relaxed)
            == FollowerMode::predictiveActive
        && !autoPlaying;
    const bool allowTrustedBoundaryPrediction =
        predictiveModeActive
        && (
            previousClockSnapshot.state == BeatClockState::locked
            || previousClockSnapshot.state == BeatClockState::coasting
            || (
                previousClockSnapshot.state == BeatClockState::recovering
                && previousClockSnapshot.confidence >= 0.45));
    // The BeatClock remains a shadow confidence/stop sensor for now. The
    // product path predicts only from PhraseScheduler's bounded continuous
    // score clock, which cannot jump several score events on one observation.
    const double predictedScoreSeconds = -1.0;
    const int baselinePhraseToStart =
        transportRunning && !chordMismatchPausedAtBlockStart
        ? scheduler.processBlock(
            numSamples,
            onset,
            manual,
            autoPlaying,
            onsetTracker.isActive(),
            physicalOnset
                ? onsetTracker.getLastOnsetStrength()
                : 1.0f,
            chordEvidence.valid ? &chordEvidence : nullptr,
            allowTrustedBoundaryPrediction,
            predictedScoreSeconds,
            predictiveModeActive)
        : -1;
    const int currentScoreEvent = scheduler.getCurrentChordEventIndex();
    if (pauseOnChordMismatchEnabled.load(std::memory_order_relaxed)
        && transportRunning
        && !autoPlaying
        && lastStartedPhrase.load(std::memory_order_relaxed) >= 0
        && hasChordDetection
        && chordDetection.valid)
    {
        const int evidenceEventIndex = virtualOnset
            ? currentScoreEvent
            : lastChordOnsetScoreEventForAnalysis;
        const int expectedRoot =
            expectedRootForChordEvent(evidenceEventIndex);
        float chordSimilarity = expectedChordSimilarity(
            evidenceEventIndex, chordDetection.chroma);
        // A live player can change a chord slightly before or after the
        // score cursor. Treat the immediate score neighbours as legal rather
        // than stopping on a musically valid boundary transition.
        for (const int neighbour :
             { evidenceEventIndex - 1, evidenceEventIndex + 1 })
        {
            chordSimilarity = std::max(
                chordSimilarity,
                expectedChordSimilarity(
                    neighbour, chordDetection.chroma));
            if (expectedRootForChordEvent(neighbour)
                == chordDetection.rootPitchClass)
            {
                chordSimilarity = 1.0f;
            }
        }
        if (realtimeTraceEnabled.load(std::memory_order_relaxed))
        {
            realtimeTrace.push({
                RealtimeTraceType::chordMismatchObservation,
                blockEndSample,
                blockEndSample,
                evidenceEventIndex,
                chordSimilarity,
                static_cast<std::uint32_t>(
                    (std::max(0, expectedRoot) << 8)
                    | std::max(0, chordDetection.rootPitchClass)),
                chordDetection.confidence
            });
        }
        const auto gateDecision = chordMismatchGate.observe(
            expectedRoot,
            chordDetection.rootPitchClass,
            chordDetection.confidence,
            chordSimilarity,
            blockEndSample / controllerSampleRate);
        if (gateDecision == ChordMismatchDecision::pause)
        {
            chordMismatchPausedForUi.store(true);
            phrasePlayer.fadeOutAndCancel(0.030);
            predictiveTransport.holdForChordMismatch(blockEndSample);
            if (realtimeTraceEnabled.load(std::memory_order_relaxed))
            {
                realtimeTrace.push({
                    RealtimeTraceType::chordMismatchPaused,
                    blockEndSample,
                    blockEndSample,
                    evidenceEventIndex,
                    static_cast<float>(chordDetection.rootPitchClass),
                    static_cast<std::uint32_t>(
                        std::max(0, expectedRoot)),
                    chordDetection.confidence
                });
            }
        }
        else if (gateDecision == ChordMismatchDecision::resume)
        {
            releaseChordMismatchPause(
                blockEndSample, evidenceEventIndex);
            if (realtimeTraceEnabled.load(std::memory_order_relaxed))
            {
                realtimeTrace.push({
                    RealtimeTraceType::chordMismatchResumed,
                    blockEndSample,
                    blockEndSample,
                    evidenceEventIndex,
                    static_cast<float>(chordDetection.rootPitchClass),
                    static_cast<std::uint32_t>(
                        std::max(0, expectedRoot)),
                    chordDetection.confidence
                });
            }
        }
    }
    // A prediction is a reservation, not a command to ignore the performer.
    // If the matching printed boundary arrives earlier than predicted, release
    // the already-buffered phrase in this same audio callback.
    if (onset && !chordMismatchGate.isPaused())
    {
        const int scheduledPhrase =
            phrasePlayer.getScheduledPhraseIndex();
        const auto& phrases = songPackage.getPhrases();
        if (scheduledPhrase >= 0
            && scheduledPhrase < static_cast<int>(phrases.size())
            && phrases[static_cast<size_t>(
                scheduledPhrase)].anchorChordEventIndex
                <= currentScoreEvent)
        {
            phrasePlayer.expediteScheduledPhrase();
        }
    }
    double observedScoreSeconds = -1.0;
    double expectedNextScoreSeconds = -1.0;
    const auto& chordTimeline = songPackage.getChordTimeline();
    if (currentScoreEvent >= 0
        && currentScoreEvent < static_cast<int>(chordTimeline.size()))
    {
        observedScoreSeconds =
            chordTimeline[static_cast<size_t>(currentScoreEvent)].startSeconds;
        if (currentScoreEvent + 1 < static_cast<int>(chordTimeline.size()))
        {
            expectedNextScoreSeconds = chordTimeline[
                static_cast<size_t>(currentScoreEvent + 1)].startSeconds;
        }
    }
    const bool hasNewScoreObservation =
        onset && currentScoreEvent >= 0
        && currentScoreEvent != beatClockObservedScoreEventIndex;
    if (hasNewScoreObservation)
        beatClockObservedScoreEventIndex = currentScoreEvent;
    const std::int64_t shadowOnsetSample = physicalOnset
        ? std::max<std::int64_t>(
            blockStartSample,
            blockStartSample + onsetTracker.getLastOnsetSampleOffset())
        : blockStartSample;
    if (physicalOnset)
    {
        lastPhysicalOnsetForChordAnalysis = shadowOnsetSample;
        lastChordOnsetScoreEventForAnalysis = currentScoreEvent;
    }
    beatClock.processBlock(
        blockStartSample,
        numSamples,
        transportRunning,
        onset && !chordMismatchGate.isPaused(),
        shadowOnsetSample,
        hasNewScoreObservation && !chordMismatchGate.isPaused(),
        observedScoreSeconds,
        expectedNextScoreSeconds,
        onsetTracker.isActive());
    const auto selectedFollowerMode =
        followerMode.load(std::memory_order_relaxed);
    const bool activeV2Output =
        selectedFollowerMode == FollowerMode::activeV2
        && !autoPlaying;
    const bool activeV2Enabled =
        (selectedFollowerMode == FollowerMode::activeV2Shadow
            || activeV2Output)
        && !autoPlaying;
    if (activeV2Enabled)
        continuousChromaExtractor.processBlock(guitarInput, numSamples);
    if (activeV2Enabled && onset && (chordDetection.valid || onsetTracker.getLastOnsetStrength() >= 0.20f))
        introChromaAligner.noteFirstOnset(shadowOnsetSample);
    if (activeV2Enabled)
    {
        const auto chromaFrame =
            continuousChromaExtractor.getLatestFrame();
        const auto chromaWindowStartSample =
            continuousChromaExtractor.getLatestWindowStartSample();
        introChromaAligner.processFrame(
            chromaWindowStartSample,
            chromaFrame,
            true);
        const auto introAlignment = introChromaAligner.getAlignment();
        if (introAlignment.locked && !introAlignmentApplied)
        {
            predictiveTransport.applyIntroAlignment(
                blockEndSample,
                introAlignment.performanceSecondsPerScoreSecond,
                introAlignment.offsetSeconds,
                introAlignment.confidence,
                introAlignment.harmonicMargin);
            introAlignmentApplied = true;
            if (realtimeTraceEnabled.load(std::memory_order_relaxed))
            {
                realtimeTrace.push({
                    RealtimeTraceType::introChromaAlignmentLocked,
                    blockEndSample,
                    static_cast<std::int64_t>(std::llround(
                        introAlignment.offsetSeconds
                            * controllerSampleRate)),
                    introAlignment.evidenceFrames,
                    static_cast<float>(
                        introAlignment
                            .performanceSecondsPerScoreSecond),
                    predictiveTransport.usedReactiveIntroHypothesis()
                        ? 1u : 0u,
                    static_cast<float>(introAlignment.harmonicMargin)
                });
            }
        }
    }
    if (activeV2Enabled
        && introAlignmentApplied
        && hasChordDetection
        && chordDetection.valid
        && lastPhysicalOnsetForChordAnalysis >= 0)
    {
        if (predictiveTransport.observeHarmonicOnset(
                lastPhysicalOnsetForChordAnalysis,
                chordDetection.chroma)
            && realtimeTraceEnabled.load(std::memory_order_relaxed))
        {
            const auto corrected = predictiveTransport.getSnapshot();
            realtimeTrace.push({
                RealtimeTraceType::predictiveTimingCorrection,
                lastPhysicalOnsetForChordAnalysis,
                blockEndSample,
                corrected.acceptedObservations,
                static_cast<float>(
                    corrected.performanceSecondsPerScoreSecond),
                0u,
                static_cast<float>(
                    corrected.phaseErrorSeconds * 1000.0)
            });
        }
        lastPhysicalOnsetForChordAnalysis = -1;
    }
    predictiveTransport.processBlock(
        blockStartSample,
        numSamples,
        transportRunning && activeV2Enabled,
        onsetTracker.isActive(),
        onset && !chordMismatchGate.isPaused(),
        shadowOnsetSample,
        hasNewScoreObservation && !chordMismatchGate.isPaused(),
        currentScoreEvent,
        observedScoreSeconds);
    const auto predictiveSnapshot =
        predictiveTransport.getSnapshot();
    const auto beatClockSnapshot = beatClock.getSnapshot();
    const bool predictiveActive =
        followerMode.load(std::memory_order_relaxed)
            == FollowerMode::predictiveActive
        && !autoPlaying;
    const int phraseToStart =
        activeV2Output || chordMismatchGate.isPaused()
        ? -1
        : baselinePhraseToStart;
    if (realtimeTraceEnabled.load(std::memory_order_relaxed)
        && hasNewScoreObservation)
    {
        RealtimeTraceEvent clockEvent;
        clockEvent.type = RealtimeTraceType::beatClockObservation;
        clockEvent.sample = shadowOnsetSample;
        clockEvent.relatedSample = blockEndSample;
        clockEvent.index = currentScoreEvent;
        clockEvent.value = static_cast<float>(
            60.0 / beatClockSnapshot.secondsPerBeat);
        clockEvent.flags =
            static_cast<std::uint32_t>(beatClockSnapshot.state);
        clockEvent.value2 =
            static_cast<float>(beatClockSnapshot.confidence);
        realtimeTrace.push(clockEvent);
    }
    if (realtimeTraceEnabled.load(std::memory_order_relaxed)
        && beatClockSnapshot.state != tracedBeatClockState)
    {
        tracedBeatClockState = beatClockSnapshot.state;
        RealtimeTraceEvent stateEvent;
        stateEvent.type = RealtimeTraceType::beatClockStateChanged;
        stateEvent.sample = blockEndSample;
        stateEvent.relatedSample = blockEndSample;
        stateEvent.index = currentScoreEvent;
        stateEvent.value = static_cast<float>(
            60.0 / beatClockSnapshot.secondsPerBeat);
        stateEvent.flags =
            static_cast<std::uint32_t>(beatClockSnapshot.state);
        stateEvent.value2 =
            static_cast<float>(beatClockSnapshot.confidence);
        realtimeTrace.push(stateEvent);
    }
    if (realtimeTraceEnabled.load(std::memory_order_relaxed)
        && activeV2Enabled
        && predictiveSnapshot.state
            != tracedPredictiveTransportState)
    {
        tracedPredictiveTransportState = predictiveSnapshot.state;
        realtimeTrace.push({
            RealtimeTraceType::predictiveTransportState,
            blockEndSample,
            blockEndSample,
            currentScoreEvent,
            static_cast<float>(predictiveSnapshot.scoreSeconds),
            static_cast<std::uint32_t>(predictiveSnapshot.state),
            static_cast<float>(predictiveSnapshot.confidence)
        });
    }
    PredictiveTransportEvent predictedEvent;
    while (activeV2Enabled
        && predictiveTransport.popEvent(predictedEvent))
    {
        realtimeTrace.push({
            predictedEvent.type
                    == PredictiveTransportEventType::phraseScheduled
                ? RealtimeTraceType::predictivePhraseScheduled
                : RealtimeTraceType::predictivePhraseCancelled,
            predictedEvent.targetSample,
            predictedEvent.decisionSample,
            predictedEvent.phraseIndex,
            static_cast<float>(
                (predictedEvent.targetSample
                    - predictedEvent.decisionSample)
                * 1000.0 / controllerSampleRate),
            static_cast<std::uint32_t>(
                predictiveSnapshot.state),
            static_cast<float>(predictedEvent.confidence)
        });
        if (activeV2Output)
        {
            if (predictedEvent.type
                == PredictiveTransportEventType::phraseScheduled)
            {
                scheduleActiveV2Phrase(
                    predictedEvent.phraseIndex,
                    predictedEvent.targetSample,
                    blockStartSample);
            }
            else
            {
                phrasePlayer.cancelScheduledPhrase(
                    predictedEvent.phraseIndex);
            }
        }
    }
    currentChordEventForUi.store(
        scheduler.getCurrentChordEventIndex());
    if (realtimeTraceEnabled.load(std::memory_order_relaxed)
        && scheduler.getCurrentChordEventIndex() != tracedChordEventIndex)
    {
        tracedChordEventIndex = scheduler.getCurrentChordEventIndex();
        realtimeTrace.push({
            RealtimeTraceType::scoreEventChanged,
            blockStartSample,
            blockEndSample,
            tracedChordEventIndex,
            static_cast<float>(scheduler.getTempoScale()),
            0u
        });
    }
    if (phraseToStart >= 0)
    {
        if (realtimeTraceEnabled.load(std::memory_order_relaxed))
        {
            realtimeTrace.push({
                RealtimeTraceType::phraseRequested,
                blockStartSample,
                blockEndSample,
                phraseToStart,
                0.0f,
                predictiveActive ? 1u : 0u
            });
        }
        startPhraseImmediately(phraseToStart);
    }

    phrasePlayer.setPitchSemitones(
        static_cast<float>(juce::jlimit(
            -3,
            3,
            getResidualKeyShift())));
    phrasePlayer.processBlock(outputLeft, outputRight, numSamples);
    if (realtimeTraceEnabled.load(std::memory_order_relaxed)
        && phrasePlayer.getFirstOutputSampleOffset() >= 0)
    {
        realtimeTrace.push({
            RealtimeTraceType::vocalFirstOutput,
            blockStartSample + phrasePlayer.getFirstOutputSampleOffset(),
            blockEndSample,
            phrasePlayer.getFirstOutputPhraseIndex(),
            0.0f,
            0u
        });
    }

    bool waitingInVocalRest = false;
    if (!autoPlaying && scheduler.isRunning() && !phrasePlayer.isPlaying())
    {
        const int nextPhrase = scheduler.getNextPhraseIndex();
        const auto& phrases = songPackage.getPhrases();
        waitingInVocalRest =
            nextPhrase >= static_cast<int>(phrases.size())
            || (
                nextPhrase >= 0
                && phrases[static_cast<size_t>(nextPhrase)].sourceStartSeconds
                    - scheduler.getSongTimeSeconds()
                    > 0.40);
    }
    vocalRest.store(waitingInVocalRest);
    processedSamples = blockEndSample;
}

void Mode1Controller::startPhraseImmediately(int phraseIndex) noexcept
{
    const auto& phrases = songPackage.getPhrases();
    if (phraseIndex < 0 || phraseIndex >= static_cast<int>(phrases.size()))
        return;

    const auto& phrase = phrases[static_cast<size_t>(phraseIndex)];
    const double durationScale = calculateVocalDurationScale();
    vocalDurationScale.store(durationScale, std::memory_order_relaxed);
    const double targetDuration =
        (phrase.sourceEndSeconds - phrase.sourceStartSeconds)
        * durationScale;
    double transitionSeconds = 0.035;
    if (phraseIndex > 0)
    {
        const auto& previous =
            phrases[static_cast<size_t>(phraseIndex - 1)];
        const double writtenGap =
            phrase.sourceStartSeconds - previous.sourceEndSeconds;
        transitionSeconds = writtenGap > 0.055 ? 0.006 : 0.035;
    }
    // Baseline/Shadow keep jumping straight to the scored vowel (leadIn 0),
    // exactly as before. Active plays the clip's natural pre-vowel pad
    // instead, but only on a phrase whose trigger time actually honoured
    // PhraseScheduler's (already earlier) target -- lastPhraseUsedLeadTiming
    // is false for a phrase that fired the instant a fresh onset confirmed
    // its boundary, where there was no room to move the trigger earlier and
    // adding a pad would only delay the vowel (Oracle C vs. Oracle B).
    const bool activeLeadInEnabled =
        followerMode.load(std::memory_order_relaxed)
            == FollowerMode::predictiveActive
        && !automaticPlayback.load(std::memory_order_relaxed)
        && scheduler.lastPhraseUsedLeadTiming();
    const double leadInSeconds =
        activeLeadInEnabled ? phrase.contentOffsetSeconds : 0.0;
    phrasePlayer.requestPhrase(
        phraseIndex,
        0.5f,
        targetDuration,
        transitionSeconds,
        scheduler.getScheduledPhraseDelaySamples(),
        leadInSeconds);
    lastStartedPhrase.store(phraseIndex);
    pendingCommittedPhrase = -1;
    pendingCommitBlocks = 0;
}

void Mode1Controller::scheduleActiveV2Phrase(
    int phraseIndex,
    std::int64_t targetSample,
    std::int64_t blockStartSample) noexcept
{
    const auto& phrases = songPackage.getPhrases();
    if (phraseIndex < 0
        || phraseIndex >= static_cast<int>(phrases.size()))
        return;
    const auto& phrase = phrases[static_cast<size_t>(phraseIndex)];
    const double durationScale = calculateVocalDurationScale();
    vocalDurationScale.store(durationScale, std::memory_order_relaxed);
    const double targetDuration =
        std::max(0.02, phrase.sourceEndSeconds - phrase.sourceStartSeconds)
        * durationScale;
    const auto transport = predictiveTransport.getSnapshot();
    double transitionSeconds = 0.035;
    if (phraseIndex > 0)
    {
        const auto& previous =
            phrases[static_cast<size_t>(phraseIndex - 1)];
        const double writtenGap =
            phrase.sourceStartSeconds - previous.sourceEndSeconds;
        transitionSeconds = writtenGap > 0.055 ? 0.006 : 0.035;
    }

    const double leadInSeconds = std::max(
        0.0,
        phrase.contentOffsetSeconds);
    const auto leadInSamples = static_cast<std::int64_t>(std::llround(
        leadInSeconds * controllerSampleRate));
    const auto requestedStartSample =
        targetSample - outputLatencySamplesV2 - leadInSamples;
    const int delaySamples = static_cast<int>(std::clamp<std::int64_t>(
        requestedStartSample - blockStartSample,
        0,
        std::numeric_limits<int>::max()));
    phrasePlayer.requestPhrase(
        phraseIndex,
        0.5f,
        targetDuration,
        transitionSeconds,
        delaySamples,
        leadInSeconds);
    lastStartedPhrase.store(phraseIndex);
    if (realtimeTraceEnabled.load(std::memory_order_relaxed))
    {
        realtimeTrace.push({
            RealtimeTraceType::phraseRequested,
            requestedStartSample,
            targetSample,
            phraseIndex,
            static_cast<float>(leadInSeconds * 1000.0),
            2u,
            static_cast<float>(
                transport.performanceSecondsPerScoreSecond)
        });
    }
}

double Mode1Controller::calculateVocalDurationScale() const noexcept
{
    if (!followPerformanceTempoEnabled.load(std::memory_order_relaxed))
        return 1.0;

    const auto mode = followerMode.load(std::memory_order_relaxed);
    if (mode == FollowerMode::activeV2
        || mode == FollowerMode::activeV2Shadow)
    {
        return juce::jlimit(
            0.80,
            1.25,
            predictiveTransport.getSnapshot()
                .performanceSecondsPerScoreSecond);
    }

    return 1.0 / juce::jlimit(
        0.80,
        1.25,
        scheduler.getTempoScale());
}

void Mode1Controller::startAutomaticPlayback() noexcept
{
    chordMismatchGate.reset();
    chordMismatchPausedForUi.store(false);
    releaseChordMismatchPauseRequested.store(false);
    lastChordOnsetScoreEventForAnalysis = -1;
    scheduler.reset();
    beatClock.reset();
    predictiveScheduler.reset();
    predictiveTransport.reset();
    introChromaAligner.reset();
    continuousChromaExtractor.reset();
    introAlignmentApplied = false;
    lastPhysicalOnsetForChordAnalysis = -1;
    beatClockObservedScoreEventIndex = -1;
    tracedBeatClockState = BeatClockState::disarmed;
    tracedPredictiveTransportState = BeatClockState::disarmed;
    phrasePlayer.stop();
    lastStartedPhrase.store(-1);
    pendingCommittedPhrase = -1;
    pendingCommitBlocks = 0;
    lastOnset.store(false);
    vocalRest.store(false);
    manualTrigger.store(false);
    pendingVirtualChordRoot.store(-1);
    currentChordEventForUi.store(-1);
    performanceRunning.store(true);
    automaticPlayback.store(true);
}

void Mode1Controller::stopAutomaticPlayback() noexcept
{
    chordMismatchGate.reset();
    chordMismatchPausedForUi.store(false);
    releaseChordMismatchPauseRequested.store(false);
    lastChordOnsetScoreEventForAnalysis = -1;
    automaticPlayback.store(false);
    performanceRunning.store(false);
    scheduler.reset();
    beatClock.reset();
    predictiveScheduler.reset();
    predictiveTransport.reset();
    introChromaAligner.reset();
    continuousChromaExtractor.reset();
    introAlignmentApplied = false;
    lastPhysicalOnsetForChordAnalysis = -1;
    beatClockObservedScoreEventIndex = -1;
    tracedBeatClockState = BeatClockState::disarmed;
    tracedPredictiveTransportState = BeatClockState::disarmed;
    phrasePlayer.stop();
    lastStartedPhrase.store(-1);
    pendingCommittedPhrase = -1;
    pendingCommitBlocks = 0;
    lastOnset.store(false);
    vocalRest.store(false);
    manualTrigger.store(false);
    pendingVirtualChordRoot.store(-1);
    currentChordEventForUi.store(-1);
}

void Mode1Controller::startPerformance() noexcept
{
    restartPerformance();
}

void Mode1Controller::restartPerformance() noexcept
{
    chordMismatchGate.reset();
    chordMismatchPausedForUi.store(false);
    releaseChordMismatchPauseRequested.store(false);
    lastChordOnsetScoreEventForAnalysis = -1;
    automaticPlayback.store(false);
    performanceRunning.store(true);
    scheduler.reset();
    beatClock.reset();
    predictiveScheduler.reset();
    predictiveTransport.reset();
    introChromaAligner.reset();
    continuousChromaExtractor.reset();
    introAlignmentApplied = false;
    lastPhysicalOnsetForChordAnalysis = -1;
    beatClockObservedScoreEventIndex = -1;
    tracedBeatClockState = BeatClockState::disarmed;
    tracedPredictiveTransportState = BeatClockState::disarmed;
    phrasePlayer.stop();
    onsetTracker.reset();
    chordTracker.reset();
    manualTrigger.store(false);
    pendingVirtualChordRoot.store(-1);
    lastStartedPhrase.store(-1);
    pendingCommittedPhrase = -1;
    pendingCommitBlocks = 0;
    lastOnset.store(false);
    vocalRest.store(false);
    detectedChordRoot.store(-1);
    detectedChordBass.store(-1);
    detectedChordQuality.store(
        static_cast<int>(ChordQuality::unknown));
    currentChordEventForUi.store(-1);
    pendingPitchShift = 0;
    pendingPitchShiftCount = 0;
    pendingPitchShiftLastEventIndex = -1;
    pendingPitchShiftDistinctEventCount = 0;
    performanceKeyOffset.store(0);
    performanceKeyOffsetLocked.store(false);
}

void Mode1Controller::stopPerformance() noexcept
{
    chordMismatchGate.reset();
    chordMismatchPausedForUi.store(false);
    releaseChordMismatchPauseRequested.store(false);
    lastChordOnsetScoreEventForAnalysis = -1;
    automaticPlayback.store(false);
    performanceRunning.store(false);
    scheduler.reset();
    beatClock.reset();
    predictiveScheduler.reset();
    predictiveTransport.reset();
    introChromaAligner.reset();
    continuousChromaExtractor.reset();
    introAlignmentApplied = false;
    lastPhysicalOnsetForChordAnalysis = -1;
    beatClockObservedScoreEventIndex = -1;
    tracedBeatClockState = BeatClockState::disarmed;
    tracedPredictiveTransportState = BeatClockState::disarmed;
    phrasePlayer.stop();
    manualTrigger.store(false);
    pendingVirtualChordRoot.store(-1);
    lastStartedPhrase.store(-1);
    pendingCommittedPhrase = -1;
    pendingCommitBlocks = 0;
    lastOnset.store(false);
    vocalRest.store(false);
    currentChordEventForUi.store(-1);
}

bool Mode1Controller::setManualKeyShift(
    int semitones,
    juce::String* error)
{
    const int clamped = juce::jlimit(
        getMinimumManualKeyShift(),
        getMaximumManualKeyShift(),
        semitones);
    const int targetKeyShift = getBaseKeyShift() + clamped;
    const int anchor = songPackage.getNearestKeyAnchor(targetKeyShift);

    if (songPackage.isLoaded() && anchor != selectedKeyAnchor.load())
    {
        juce::String loadError;
        if (!phrasePlayer.loadKeyAnchor(
                songPackage,
                anchor,
                loadError,
                expressionStrength.load()))
        {
            if (error != nullptr)
                *error = loadError;
            return false;
        }
        selectedKeyAnchor.store(anchor);
    }

    manualKeyShift.store(clamped);
    performanceKeyOffset.store(0);
    performanceKeyOffsetLocked.store(false);
    manualKeyRevision.fetch_add(1);
    if (error != nullptr)
        error->clear();
    return true;
}

bool Mode1Controller::setExpressionStrength(
    int strength,
    juce::String* error)
{
    const int clamped = juce::jlimit(0, 100, strength);
    if (songPackage.isLoaded() && clamped != expressionStrength.load())
    {
        juce::String loadError;
        if (!phrasePlayer.loadKeyAnchor(
                songPackage,
                selectedKeyAnchor.load(),
                loadError,
                clamped))
        {
            if (error != nullptr)
                *error = loadError;
            return false;
        }
    }
    expressionStrength.store(clamped);
    phrasePlayer.setExpressionStrength(clamped);
    if (error != nullptr)
        error->clear();
    return true;
}

juce::String Mode1Controller::getCurrentLyrics() const
{
    if (chordMismatchPausedForUi.load())
        return juce::String::fromUTF8(
            "코드 오류 · 올바른 코드로 다시 연주하세요");
    if (vocalRest.load())
        return juce::String::fromUTF8("연주 구간 · 보컬 쉼");

    const int index = getCurrentPhraseIndex();
    const auto& phrases = songPackage.getPhrases();
    if (index < 0 || index >= static_cast<int>(phrases.size()))
        return {};
    return phrases[static_cast<size_t>(index)].lyrics;
}

int Mode1Controller::expectedRootForPhrase(int phraseIndex) const noexcept
{
    const auto& phrases = songPackage.getPhrases();
    if (phrases.empty())
        return -1;

    juce::String expectedChord;
    const int chordIndex = scheduler.getCurrentChordEventIndex();
    const auto& timeline = songPackage.getChordTimeline();
    if (chordIndex >= 0 && chordIndex < static_cast<int>(timeline.size()))
        expectedChord = timeline[static_cast<size_t>(chordIndex)].chord;

    if (expectedChord.isEmpty())
    {
        for (int index = juce::jlimit(
                 0,
                 static_cast<int>(phrases.size()) - 1,
                 phraseIndex);
             index >= 0 && expectedChord.isEmpty();
             --index)
        {
            for (const auto& event : phrases[static_cast<size_t>(index)].chords)
                if (event.chord.trim().isNotEmpty()
                    && event.chord.trim().toUpperCase() != "N")
                {
                    expectedChord = event.chord;
                    break;
                }
        }
    }

    const auto chord = expectedChord.trim().toUpperCase();
    if (chord.isEmpty() || chord == "N")
        return -1;

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
        default: break;
    }
    if (root < 0)
        return -1;
    if (chord.length() > 1 && chord[1] == '#')
        root = (root + 1) % 12;
    else if (chord.length() > 1 && chord[1] == 'B')
        root = (root + 11) % 12;
    return (
        root + manualKeyShift.load() + 120)
        % 12;
}

int Mode1Controller::expectedRootForChordEvent(int eventIndex) const noexcept
{
    const auto& timeline = songPackage.getChordTimeline();
    if (eventIndex < 0 || eventIndex >= static_cast<int>(timeline.size()))
        return -1;

    const auto chord = timeline[
        static_cast<size_t>(eventIndex)].chord.trim().toUpperCase();
    if (chord.isEmpty() || chord == "N" || chord == "N.C.")
        return -1;

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
        default: return -1;
    }
    if (chord.length() > 1 && chord[1] == '#')
        root = (root + 1) % 12;
    else if (chord.length() > 1 && chord[1] == 'B')
        root = (root + 11) % 12;
    const int performanceOffset =
        performanceKeyOffsetLocked.load()
        ? performanceKeyOffset.load()
        : 0;
    return (
        root
        + manualKeyShift.load()
        + performanceOffset
        + 120)
        % 12;
}

float Mode1Controller::expectedChordSimilarity(
    int eventIndex,
    const std::array<float, 12>& inputChroma) const noexcept
{
    const auto& timeline = songPackage.getChordTimeline();
    if (eventIndex < 0 || eventIndex >= static_cast<int>(timeline.size()))
        return -1.0f;

    auto chord = timeline[
        static_cast<size_t>(eventIndex)].chord.trim()
            .upToFirstOccurrenceOf("/", false, false);
    const int rootWithoutManualShift =
        expectedRootForChordEvent(eventIndex);
    if (rootWithoutManualShift < 0)
        return -1.0f;
    const int root = rootWithoutManualShift;

    const int accidentalLength =
        chord.length() > 1 && (
            chord[1] == '#' || chord[1] == 'b' || chord[1] == 'B')
        ? 2
        : 1;
    const auto suffix = chord.substring(accidentalLength).toLowerCase();
    const bool minor =
        suffix.startsWith("m") && !suffix.startsWith("maj");
    std::array<int, 4> intervals { 0, minor ? 3 : 4, 7, -1 };
    if (suffix.contains("dim"))
    {
        intervals[1] = 3;
        intervals[2] = 6;
    }
    else if (suffix.contains("sus2"))
        intervals[1] = 2;
    else if (suffix.contains("sus4") || suffix == "sus")
        intervals[1] = 5;
    if (suffix.contains("maj7"))
        intervals[3] = 11;
    else if (suffix.contains("7"))
        intervals[3] = 10;

    std::array<float, 12> expected {};
    for (const int interval : intervals)
        if (interval >= 0)
            expected[static_cast<size_t>((root + interval) % 12)] = 1.0f;

    auto centredInput = inputChroma;
    const auto centreAndNormalize = [](
        std::array<float, 12>& values) noexcept
    {
        float mean = 0.0f;
        for (const float value : values)
            mean += value;
        mean /= 12.0f;
        float norm = 0.0f;
        for (auto& value : values)
        {
            value -= mean;
            norm += value * value;
        }
        norm = std::sqrt(norm);
        if (norm <= 1.0e-6f)
            return false;
        for (auto& value : values)
            value /= norm;
        return true;
    };
    if (!centreAndNormalize(centredInput)
        || !centreAndNormalize(expected))
        return -1.0f;

    float similarity = 0.0f;
    for (size_t pitch = 0; pitch < expected.size(); ++pitch)
        similarity += centredInput[pitch] * expected[pitch];
    return similarity;
}

void Mode1Controller::releaseChordMismatchPause(
    std::int64_t decisionSample,
    int eventIndex) noexcept
{
    chordMismatchGate.reset();
    chordMismatchPausedForUi.store(false);

    const auto& timeline = songPackage.getChordTimeline();
    const double scoreSeconds =
        eventIndex >= 0 && eventIndex < static_cast<int>(timeline.size())
        ? timeline[static_cast<size_t>(eventIndex)].startSeconds
        : scheduler.getSongTimeSeconds();
    predictiveTransport.resumeFromChordMismatch(
        decisionSample, eventIndex, scoreSeconds);
}

void Mode1Controller::updateTransposition(
    const ChordDetection& detection) noexcept
{
    detectedChordRoot.store(detection.rootPitchClass);
    detectedChordBass.store(detection.bassPitchClass);
    detectedChordQuality.store(
        static_cast<int>(detection.quality));
    const int referencePhrase =
        std::max(0, scheduler.getNextPhraseIndex() - 1);
    const int expectedRoot = expectedRootForPhrase(referencePhrase);
    if (expectedRoot < 0)
        return;

    int difference = detection.rootPitchClass - expectedRoot;
    while (difference > 6)
        difference -= 12;
    while (difference < -6)
        difference += 12;

    // A single mismatch is treated as a playing/detection mistake. Two
    // consecutive chord estimates with the same offset confirm a key change.
    if (difference == pendingPitchShift)
    {
        ++pendingPitchShiftCount;
        const int eventIndex = scheduler.getCurrentChordEventIndex();
        if (eventIndex >= 0
            && eventIndex != pendingPitchShiftLastEventIndex)
        {
            pendingPitchShiftLastEventIndex = eventIndex;
            ++pendingPitchShiftDistinctEventCount;
        }
    }
    else
    {
        pendingPitchShift = difference;
        pendingPitchShiftCount = 1;
        pendingPitchShiftLastEventIndex =
            scheduler.getCurrentChordEventIndex();
        pendingPitchShiftDistinctEventCount =
            pendingPitchShiftLastEventIndex >= 0 ? 1 : 0;
    }

    if (pendingPitchShiftCount >= 3
        && pendingPitchShiftDistinctEventCount >= 2
        && std::abs(difference) <= 5)
    {
        performanceKeyOffset.store(difference);
        performanceKeyOffsetLocked.store(true);
    }
}

juce::String Mode1Controller::formatRawDetectedChordName() const
{
    static constexpr const char* names[] = {
        "C", "Db", "D", "Eb", "E", "F",
        "Gb", "G", "Ab", "A", "Bb", "B",
    };
    const int root = detectedChordRoot.load();
    if (root < 0 || root >= 12)
        return "-";

    const auto quality = static_cast<ChordQuality>(
        detectedChordQuality.load());
    juce::String suffix;
    switch (quality)
    {
        case ChordQuality::minor: suffix = "m"; break;
        case ChordQuality::diminished: suffix = "dim"; break;
        case ChordQuality::suspended2: suffix = "sus2"; break;
        case ChordQuality::suspended4: suffix = "sus4"; break;
        case ChordQuality::dominant7: suffix = "7"; break;
        case ChordQuality::major7: suffix = "maj7"; break;
        case ChordQuality::minor7: suffix = "m7"; break;
        case ChordQuality::major:
        case ChordQuality::unknown:
            break;
    }

    juce::String name = juce::String(names[root]) + suffix;
    const int bass = detectedChordBass.load();
    const int bassInterval = (bass - root + 12) % 12;
    bool bassIsChordTone = bassInterval == 0 || bassInterval == 7;
    switch (quality)
    {
        case ChordQuality::major:
        case ChordQuality::dominant7:
        case ChordQuality::major7:
            bassIsChordTone = bassIsChordTone || bassInterval == 4;
            break;
        case ChordQuality::minor:
        case ChordQuality::minor7:
        case ChordQuality::diminished:
            bassIsChordTone =
                bassIsChordTone || bassInterval == 3 || bassInterval == 6;
            break;
        case ChordQuality::suspended2:
            bassIsChordTone = bassIsChordTone || bassInterval == 2;
            break;
        case ChordQuality::suspended4:
            bassIsChordTone = bassIsChordTone || bassInterval == 5;
            break;
        case ChordQuality::unknown:
            break;
    }
    if (quality == ChordQuality::dominant7
        || quality == ChordQuality::minor7)
        bassIsChordTone = bassIsChordTone || bassInterval == 10;
    else if (quality == ChordQuality::major7)
        bassIsChordTone = bassIsChordTone || bassInterval == 11;

    if (bass >= 0 && bass < 12 && bass != root && bassIsChordTone)
        name += "/" + juce::String(names[bass]);
    return name;
}

juce::String Mode1Controller::guidedChordName() const
{
    const int eventIndex = currentChordEventForUi.load();
    const auto& timeline = songPackage.getChordTimeline();
    if (eventIndex < 0 || eventIndex >= static_cast<int>(timeline.size()))
        return {};

    auto chord = timeline[static_cast<size_t>(eventIndex)].chord.trim();
    if (chord.isEmpty() || chord.toUpperCase() == "N")
        return {};

    // Song-guided recognition intentionally stays on the package timeline.
    // The raw FFT estimate is shown separately for diagnostics and is never
    // allowed to relabel the expected chord or retune the vocal.
    const int offset = manualKeyShift.load();
    if (offset == 0)
        return chord;

    static constexpr const char* flatNames[] = {
        "C", "Db", "D", "Eb", "E", "F",
        "Gb", "G", "Ab", "A", "Bb", "B",
    };
    const auto transposeNote = [offset](juce::String token)
    {
        token = token.trim();
        if (token.isEmpty())
            return token;
        int pitchClass = -1;
        switch (juce::CharacterFunctions::toUpperCase(token[0]))
        {
            case 'C': pitchClass = 0; break;
            case 'D': pitchClass = 2; break;
            case 'E': pitchClass = 4; break;
            case 'F': pitchClass = 5; break;
            case 'G': pitchClass = 7; break;
            case 'A': pitchClass = 9; break;
            case 'B': pitchClass = 11; break;
            default: return token;
        }
        if (token.length() > 1 && token[1] == '#')
            ++pitchClass;
        else if (token.length() > 1 && token[1] == 'b')
            --pitchClass;
        return juce::String(flatNames[
            static_cast<size_t>((pitchClass + offset + 120) % 12)]);
    };

    int rootLength = 1;
    if (chord.length() > 1 && (chord[1] == '#' || chord[1] == 'b'))
        rootLength = 2;
    const int slash = chord.indexOfChar('/');
    const auto suffix = slash >= 0
        ? chord.substring(rootLength, slash)
        : chord.substring(rootLength);
    auto transposed = transposeNote(chord.substring(0, rootLength)) + suffix;
    if (slash >= 0)
        transposed += "/" + transposeNote(chord.substring(slash + 1));
    return transposed;
}

juce::String Mode1Controller::getDetectedChordName() const
{
    const auto guided = guidedChordName();
    return guided.isNotEmpty() ? guided : formatRawDetectedChordName();
}

juce::String Mode1Controller::getRawDetectedChordName() const
{
    return formatRawDetectedChordName();
}

} // namespace mode1
