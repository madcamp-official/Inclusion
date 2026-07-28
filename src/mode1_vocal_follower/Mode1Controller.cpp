#include "Mode1Controller.h"

namespace mode1
{

void Mode1Controller::prepare(double sampleRate, int maximumBlockSize)
{
    phrasePlayer.prepare(sampleRate, maximumBlockSize);
    scheduler.prepare(sampleRate);
    onsetTracker.prepare(sampleRate);
    chordTracker.prepare(sampleRate);
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
}

void Mode1Controller::processBlock(
    const float* guitarInput,
    float* outputLeft,
    float* outputRight,
    int numSamples) noexcept
{
    // Stage 3 commit window. A reservation lives for one callback before it
    // becomes audible, so a score-position correction can replace it without
    // cutting off sound that has already begun. At 48 kHz / 480 samples this
    // is a bounded 10 ms, not a beat-scale recognition delay.
    if (pendingCommittedPhrase >= 0 && --pendingCommitBlocks <= 0)
    {
        const auto& phrases = songPackage.getPhrases();
        const int phraseIndex = pendingCommittedPhrase;
        const auto& phrase = phrases[static_cast<size_t>(phraseIndex)];
        const double targetDuration =
            (phrase.sourceEndSeconds - phrase.sourceStartSeconds)
            / juce::jlimit(0.80, 1.25, scheduler.getTempoScale());
        double transitionSeconds = 0.035;
        if (phraseIndex > 0)
        {
            const auto& previous =
                phrases[static_cast<size_t>(phraseIndex - 1)];
            const double writtenGap =
                phrase.sourceStartSeconds - previous.sourceEndSeconds;
            // Explicit breaths/rests must stay open. Connected vowels use a
            // longer equal-power join; separated boundaries only de-click.
            transitionSeconds = writtenGap > 0.055 ? 0.006 : 0.035;
        }
        phrasePlayer.requestPhrase(
            phraseIndex, 0.5f, targetDuration, transitionSeconds);
        lastStartedPhrase.store(phraseIndex);
        pendingCommittedPhrase = -1;
    }

    const auto keyRevision = manualKeyRevision.load();
    if (keyRevision != observedManualKeyRevision)
    {
        observedManualKeyRevision = keyRevision;
        pendingPitchShift = 0;
        pendingPitchShiftCount = 0;
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
    const bool onset =
        !autoPlaying && (physicalOnset || virtualOnset);
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
    if (hasChordDetection && chordDetection.valid)
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
    const int phraseToStart = transportRunning
        ? scheduler.processBlock(
            numSamples,
            onset,
            manual,
            autoPlaying,
            onsetTracker.isActive(),
            physicalOnset
                ? onsetTracker.getLastOnsetStrength()
                : 1.0f,
            chordEvidence.valid ? &chordEvidence : nullptr)
        : -1;
    currentChordEventForUi.store(
        scheduler.getCurrentChordEventIndex());
    if (phraseToStart >= 0)
    {
        pendingCommittedPhrase = phraseToStart;
        pendingCommitBlocks = 1;
    }

    phrasePlayer.setPitchSemitones(
        static_cast<float>(juce::jlimit(
            -3,
            3,
            getResidualKeyShift())));
    phrasePlayer.processBlock(outputLeft, outputRight, numSamples);

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
}

void Mode1Controller::startAutomaticPlayback() noexcept
{
    scheduler.reset();
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
    automaticPlayback.store(false);
    performanceRunning.store(false);
    scheduler.reset();
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
    automaticPlayback.store(false);
    performanceRunning.store(true);
    scheduler.reset();
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
    performanceKeyOffset.store(0);
    performanceKeyOffsetLocked.store(false);
}

void Mode1Controller::stopPerformance() noexcept
{
    automaticPlayback.store(false);
    performanceRunning.store(false);
    scheduler.reset();
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
        ++pendingPitchShiftCount;
    else
    {
        pendingPitchShift = difference;
        pendingPitchShiftCount = 1;
    }

    if (pendingPitchShiftCount >= 3 && std::abs(difference) <= 5)
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
