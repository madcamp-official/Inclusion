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

    if (!phrasePlayer.load(candidate, error))
        return false;

    songPackage = std::move(candidate);
    scheduler.setSong(&songPackage);
    lastStartedPhrase.store(-1);
    return true;
}

void Mode1Controller::reset() noexcept
{
    scheduler.reset();
    phrasePlayer.stop();
    onsetTracker.reset();
    chordTracker.reset();
    manualTrigger.store(false);
    guitarActive.store(false);
    lastOnset.store(false);
    guitarRms.store(0.0f);
    lastStartedPhrase.store(-1);
    detectedChordRoot.store(-1);
    stablePitchShift.store(0);
    pendingPitchShift = 0;
    pendingPitchShiftCount = 0;
}

void Mode1Controller::processBlock(
    const float* guitarInput,
    float* outputLeft,
    float* outputRight,
    int numSamples) noexcept
{
    const bool onset = onsetTracker.processBlock(guitarInput, numSamples);
    ChordDetection chordDetection;
    const bool hasChordDetection =
        chordTracker.processBlock(guitarInput, numSamples, onset, chordDetection);
    if (hasChordDetection && chordDetection.valid)
        updateTransposition(chordDetection);
    const bool manual = manualTrigger.exchange(false);
    guitarActive.store(onsetTracker.isActive());
    guitarRms.store(onsetTracker.getCurrentRms());
    lastOnset.store(onset);

    const int phraseToStart =
        scheduler.processBlock(numSamples, onset, manual);
    if (phraseToStart >= 0)
    {
        phrasePlayer.requestPhrase(phraseToStart);
        lastStartedPhrase.store(phraseToStart);
    }

    phrasePlayer.setPitchSemitones(
        static_cast<float>(stablePitchShift.load()));
    phrasePlayer.processBlock(outputLeft, outputRight, numSamples);
}

juce::String Mode1Controller::getCurrentLyrics() const
{
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

    for (int index = juce::jlimit(
             0,
             static_cast<int>(phrases.size()) - 1,
             phraseIndex);
         index >= 0;
         --index)
    {
        for (const auto& event : phrases[static_cast<size_t>(index)].chords)
        {
            auto chord = event.chord.trim().toUpperCase();
            if (chord.isEmpty() || chord == "N")
                continue;

            const auto rootLetter = chord[0];
            int root = -1;
            switch (rootLetter)
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
                continue;
            if (chord.length() > 1 && chord[1] == '#')
                root = (root + 1) % 12;
            else if (chord.length() > 1 && chord[1] == 'B')
                root = (root + 11) % 12;
            return root;
        }
    }
    return -1;
}

void Mode1Controller::updateTransposition(
    const ChordDetection& detection) noexcept
{
    detectedChordRoot.store(detection.rootPitchClass);
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

    if (pendingPitchShiftCount >= 2)
        stablePitchShift.store(juce::jlimit(-6, 6, difference));
}

juce::String Mode1Controller::getDetectedChordName() const
{
    static constexpr const char* names[] = {
        "C", "C#", "D", "D#", "E", "F",
        "F#", "G", "G#", "A", "A#", "B",
    };
    const int root = detectedChordRoot.load();
    return root >= 0 && root < 12 ? names[root] : "-";
}

} // namespace mode1
