#include "mode1_vocal_follower/GuitarOnsetTracker.h"
#include "mode1_vocal_follower/GuitarChordTracker.h"
#include "mode1_vocal_follower/Mode1Controller.h"
#include "mode1_vocal_follower/PhrasePlayer.h"
#include "mode1_vocal_follower/PhraseScheduler.h"
#include "mode1_vocal_follower/SongPackage.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <iostream>
#include <cmath>
#include <memory>
#include <numeric>
#include <vector>

namespace
{
bool require(bool condition, const char* message)
{
    if (!condition)
        std::cerr << "FAIL: " << message << '\n';
    return condition;
}

juce::File createFixturePackage()
{
    const auto fixtureDirectory =
        juce::File::getSpecialLocation(juce::File::tempDirectory)
            .getChildFile(
                "VocalGuitarMode1Test_"
                + juce::String::toHexString(
                    juce::Random::getSystemRandom().nextInt64()));
    fixtureDirectory.createDirectory();

    const auto toneFile = fixtureDirectory.getChildFile("tone.wav");
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> outputStream =
        toneFile.createOutputStream();
    const auto writerOptions = juce::AudioFormatWriterOptions {}
        .withSampleRate(48'000.0)
        .withNumChannels(1)
        .withBitsPerSample(16);
    auto writer = wav.createWriterFor(outputStream, writerOptions);
    juce::AudioBuffer<float> tone(1, 24'000);
    for (int sample = 0; sample < tone.getNumSamples(); ++sample)
    {
        const auto phase =
            juce::MathConstants<double>::twoPi * 220.0 * sample / 48'000.0;
        tone.setSample(0, sample, static_cast<float>(0.1 * std::sin(phase)));
    }
    writer->writeFromAudioSampleBuffer(tone, 0, tone.getNumSamples());
    writer.reset();

    const auto packageFile = fixtureDirectory.getChildFile("song_package.json");
    packageFile.replaceWithText(
        juce::String::fromUTF8(R"json(
{
  "schema_version": 4,
  "song": "fixture",
  "score_bpm": 120.0,
  "base_key_shift": -17,
  "key_style": {
    "default_key_shift": -17,
    "available_key_shifts": [-17, 0]
  },
  "vocal_style": {
    "range_warnings": [
      { "type": "above_comfortable_range", "semitones": 1.0 }
    ]
  },
  "chord_timeline": [
    { "start_sec": 0.0, "chord": "C", "raw_chord": "C" },
    { "start_sec": 0.5, "chord": "F", "raw_chord": "F" },
    { "start_sec": 1.0, "chord": "G", "raw_chord": "G" },
    { "start_sec": 1.5, "chord": "C", "raw_chord": "C" },
    { "start_sec": 2.0, "chord": "C", "raw_chord": "C" },
    { "start_sec": 2.5, "chord": "F", "raw_chord": "F" },
    { "start_sec": 3.0, "chord": "G", "raw_chord": "G" }
  ],
  "micro_phrases": [
    {
      "phrase_id": "micro_000",
      "lyrics": "君",
      "score": { "start_sec": 2.0, "end_sec": 2.45 },
      "source": { "start_sec": 2.0, "end_sec": 2.45 },
      "chords": [{ "start_sec": 2.0, "chord": "C", "raw_chord": "C" }],
      "vocal": { "directory": ".", "file": "tone.wav", "content_offset_sec": 0.0 },
      "vocal_key_variants": {
        "0": {
          "25": { "directory": ".", "file": "tone.wav", "content_offset_sec": 0.0 }
        }
      }
    },
    {
      "phrase_id": "micro_001",
      "lyrics": "を",
      "score": { "start_sec": 2.5, "end_sec": 2.95 },
      "source": { "start_sec": 2.5, "end_sec": 2.95 },
      "chords": [{ "start_sec": 2.5, "chord": "F", "raw_chord": "F" }],
      "vocal": { "directory": ".", "file": "tone.wav", "content_offset_sec": 0.0 },
      "vocal_key_variants": {
        "0": {
          "25": { "directory": ".", "file": "tone.wav", "content_offset_sec": 0.0 }
        }
      }
    },
    {
      "phrase_id": "micro_002",
      "lyrics": "歌",
      "score": { "start_sec": 3.0, "end_sec": 3.45 },
      "source": { "start_sec": 3.0, "end_sec": 3.45 },
      "chords": [{ "start_sec": 3.0, "chord": "G", "raw_chord": "G" }],
      "vocal": { "directory": ".", "file": "tone.wav", "content_offset_sec": 0.0 },
      "vocal_key_variants": {
        "0": {
          "25": { "directory": ".", "file": "tone.wav", "content_offset_sec": 0.0 }
        }
      }
    }
  ]
}
)json"));
    return packageFile;
}
} // namespace

int main(int argc, char* argv[])
{
    if (argc > 2)
    {
        std::cerr << "usage: Mode1CoreTests [song_package.json]\n";
        return 2;
    }

    bool passed = true;
    const juce::File packageFile = argc == 2
        ? juce::File(juce::String::fromUTF8(argv[1]))
        : createFixturePackage();
    juce::String error;

    mode1::SongPackage package;
    passed &= require(
        package.loadFromFile(packageFile, error),
        error.toRawUTF8());
    passed &= require(package.getPhrases().size() >= 3, "expected at least 3 phrases");
    passed &= require(
        package.getBaseKeyShift() == -17,
        "style-adapted Bansanka package should use a -17 st base key");
    passed &= require(
        package.getRangeWarning().isNotEmpty(),
        "out-of-range notes should be exposed as a package warning");
    passed &= require(
        package.getPhrases().front().lyrics
            .startsWith(juce::String::fromUTF8("君")),
        "UTF-8 lyrics were not preserved");

    mode1::PhraseScheduler scheduler;
    scheduler.prepare(48'000.0);
    scheduler.setSong(&package);
    for (int introChord = 0; introChord < 4; ++introChord)
        passed &= require(
            scheduler.processBlock(512, false, true) == -1,
            "intro chords must not trigger vocals");
    passed &= require(
        scheduler.processBlock(512, false, true) == 0,
        "the first vocal chord should trigger phrase zero");
    passed &= require(
        scheduler.processBlock(512, false, true) == -1,
        "an early chord must not compress the source vocal interval");
    int preservedGapResult = -1;
    for (int block = 0; block < 60 && preservedGapResult < 0; ++block)
        preservedGapResult =
            scheduler.processBlock(512, false, false);
    passed &= require(
        preservedGapResult == 1,
        "the next phrase should start after its preserved source interval");

    mode1::PhraseScheduler guitarDrivenScheduler;
    guitarDrivenScheduler.prepare(48'000.0);
    guitarDrivenScheduler.setSong(&package);
    for (int introChord = 0; introChord < 4; ++introChord)
    {
        passed &= require(
            guitarDrivenScheduler.processBlock(512, true, false) == -1,
            "guitar intro must remain a vocal rest");
        const int heldChordIndex =
            guitarDrivenScheduler.getCurrentChordEventIndex();
        passed &= require(
            guitarDrivenScheduler.processBlock(512, true, false) == -1
                && guitarDrivenScheduler.getCurrentChordEventIndex()
                    == heldChordIndex,
            "an early repeated strum on a held chord must not advance");
        for (int waitBlock = 0; waitBlock < 47; ++waitBlock)
            guitarDrivenScheduler.processBlock(512, false, false);
    }
    passed &= require(
        guitarDrivenScheduler.processBlock(512, true, false) == 0,
        "guitar-driven scheduler should start at the first vocal chord");
    const double nextTargetSeconds =
        package.getPhrases()[1].scoreStartSeconds
        - package.getPhrases()[0].scoreStartSeconds;
    double waitingElapsedSeconds = 0.0;
    int waitingResult = -1;
    while (waitingElapsedSeconds < nextTargetSeconds + 0.5)
    {
        waitingResult =
            guitarDrivenScheduler.processBlock(512, false, false);
        waitingElapsedSeconds += 512.0 / 48'000.0;
        if (waitingResult >= 0)
            break;
    }
    passed &= require(
        waitingResult == -1,
        "a missing guitar onset must not auto-advance the lyrics");
    passed &= require(
        guitarDrivenScheduler.processBlock(512, true, false) == 1,
        "a late guitar onset should advance the waiting phrase");

    mode1::PhraseScheduler adaptiveTempoScheduler;
    adaptiveTempoScheduler.prepare(48'000.0);
    adaptiveTempoScheduler.setSong(&package);
    passed &= require(
        adaptiveTempoScheduler.processBlock(
            480, true, false, false, true) == -1,
        "the first score event should arm adaptive tracking");
    int adaptivePhrase = -1;
    for (int event = 1; event <= 4; ++event)
    {
        // The fixture score changes every 500 ms, while this simulated
        // performance changes every 400 ms (125% tempo).
        for (int block = 0; block < 39; ++block)
            adaptiveTempoScheduler.processBlock(
                480, false, false, false, true);
        adaptivePhrase = adaptiveTempoScheduler.processBlock(
            480, true, false, false, true);
    }
    passed &= require(
        adaptiveTempoScheduler.getTempoScale() > 1.10,
        "score follower did not adapt to a faster performance");
    passed &= require(
        adaptivePhrase == 0,
        "first vocal should start on the score anchor at an adapted tempo");

    mode1::PhraseScheduler subdivisionScheduler;
    subdivisionScheduler.prepare(48'000.0);
    subdivisionScheduler.setSong(&package);
    subdivisionScheduler.processBlock(
        480, true, false, false, true);
    int subdivisionPhrase = -1;
    for (int event = 1; event <= 4; ++event)
    {
        for (int block = 0; block < 24; ++block)
            subdivisionScheduler.processBlock(
                480, false, false, false, true);
        subdivisionPhrase = subdivisionScheduler.processBlock(
            480, true, false, false, true, 5.0f);
        passed &= require(
            subdivisionScheduler.getCurrentChordEventIndex() == event - 1,
            "a half-beat subdivision must not advance the score cursor");

        for (int block = 0; block < 24; ++block)
            subdivisionScheduler.processBlock(
                480, false, false, false, true);
        subdivisionPhrase = subdivisionScheduler.processBlock(
            480, true, false, false, true);
    }
    passed &= require(
        subdivisionScheduler.getCurrentChordEventIndex() == 4
            && subdivisionPhrase == 0,
        "subdivision strums must not start vocals before the score anchor");

    mode1::PhraseScheduler slowerTempoScheduler;
    slowerTempoScheduler.prepare(48'000.0);
    slowerTempoScheduler.setSong(&package);
    slowerTempoScheduler.processBlock(
        480, true, false, false, true);
    int slowerPhrase = -1;
    for (int event = 1; event <= 4; ++event)
    {
        for (int block = 0; block < 62; ++block)
            slowerTempoScheduler.processBlock(
                480, false, false, false, true);
        slowerPhrase = slowerTempoScheduler.processBlock(
            480, true, false, false, true);
    }
    passed &= require(
        slowerTempoScheduler.getTempoScale() < 0.92,
        "score follower did not adapt to a slower performance");
    passed &= require(
        slowerPhrase == 0,
        "slower playing should still start vocals at the score anchor");

    const int heldAdaptiveEvent =
        adaptiveTempoScheduler.getCurrentChordEventIndex();
    for (int block = 0; block < 9; ++block)
        adaptiveTempoScheduler.processBlock(
            480, false, false, false, true);
    adaptiveTempoScheduler.processBlock(
        480, true, false, false, true);
    passed &= require(
        adaptiveTempoScheduler.getCurrentChordEventIndex()
            == heldAdaptiveEvent,
        "an extra subdivision strum must not move the score cursor");

    mode1::PhraseScheduler recoveryScheduler;
    recoveryScheduler.prepare(48'000.0);
    recoveryScheduler.setSong(&package);
    recoveryScheduler.processBlock(480, true, false, false, true);
    for (int block = 0; block < 49; ++block)
        recoveryScheduler.processBlock(
            480, false, false, false, true);
    recoveryScheduler.processBlock(480, true, false, false, true);
    for (int block = 0; block < 99; ++block)
        recoveryScheduler.processBlock(
            480, false, false, false, true);
    recoveryScheduler.processBlock(480, true, false, false, true);
    passed &= require(
        recoveryScheduler.getCurrentChordEventIndex() == 3
            && recoveryScheduler.getRecoveredSkippedChordCount() == 1,
        "continuous playing should recover across one missed chord event");

    mode1::PhraseScheduler pausedScheduler;
    pausedScheduler.prepare(48'000.0);
    pausedScheduler.setSong(&package);
    pausedScheduler.processBlock(480, true, false, false, true);
    for (int block = 0; block < 99; ++block)
        pausedScheduler.processBlock(
            480, false, false, false, false);
    pausedScheduler.processBlock(480, true, false, false, true);
    passed &= require(
        pausedScheduler.getCurrentChordEventIndex() == 1
            && pausedScheduler.getRecoveredSkippedChordCount() == 0,
        "a real performance pause must resume without skipping the score");

    mode1::PhraseScheduler automaticScheduler;
    automaticScheduler.prepare(48'000.0);
    automaticScheduler.setSong(&package);
    passed &= require(
        automaticScheduler.processBlock(512, false, false, true) == 0,
        "automatic playback should start phrase zero");
    double automaticElapsedSeconds = 0.0;
    int automaticResult = -1;
    while (automaticElapsedSeconds < nextTargetSeconds + 0.1)
    {
        automaticResult =
            automaticScheduler.processBlock(512, false, false, true);
        automaticElapsedSeconds += 512.0 / 48'000.0;
        if (automaticResult >= 0)
            break;
    }
    passed &= require(
        automaticResult == 1,
        "automatic playback should follow the score phrase timing");

    mode1::PhrasePlayer phrasePlayer;
    phrasePlayer.prepare(48'000.0, 512);
    passed &= require(
        phrasePlayer.load(package, error),
        error.toRawUTF8());
    mode1::PhrasePlayer latencyPlayer;
    latencyPlayer.prepare(48'000.0, 512);
    passed &= require(
        latencyPlayer.load(package, error),
        error.toRawUTF8());
    latencyPlayer.requestPhrase(0);
    int firstAudibleBlock = -1;
    std::vector<float> latencyLeft(512);
    std::vector<float> latencyRight(512);
    for (int block = 0; block < 40 && firstAudibleBlock < 0; ++block)
    {
        latencyPlayer.processBlock(
            latencyLeft.data(), latencyRight.data(), 512);
        const double energy = std::inner_product(
            latencyLeft.begin(),
            latencyLeft.end(),
            latencyLeft.begin(),
            0.0);
        if (energy > 1.0e-8)
            firstAudibleBlock = block;
    }
    const double measuredPlaybackLatencySeconds =
        std::max(0, firstAudibleBlock) * 512.0 / 48'000.0;
    std::cout << "PhrasePlayer first-audio latency: "
              << measuredPlaybackLatencySeconds * 1000.0 << " ms\n";
    passed &= require(
        firstAudibleBlock >= 0
            && measuredPlaybackLatencySeconds <= 0.20,
        "phrase player latency exceeded 200 ms");
    phrasePlayer.requestPhrase(0);
    std::vector<float> crossfadeLeft(512);
    std::vector<float> crossfadeRight(512);
    phrasePlayer.processBlock(
        crossfadeLeft.data(), crossfadeRight.data(), 512);
    phrasePlayer.requestPhrase(1);
    phrasePlayer.processBlock(
        crossfadeLeft.data(), crossfadeRight.data(), 512);
    passed &= require(
        phrasePlayer.getActiveVoiceCount() == 2,
        "phrase transition should overlap two playback voices");
    if (package.getExpressionStrengths().size() >= 5)
    {
        phrasePlayer.setExpressionStrength(100);
        phrasePlayer.requestPhrase(2);
        phrasePlayer.processBlock(
            crossfadeLeft.data(), crossfadeRight.data(), 512);
        passed &= require(
            phrasePlayer.getCurrentExpressionStrength() == 100,
            "expression slider should select the 100% vocal variant");
    }

    mode1::GuitarOnsetTracker onsetTracker;
    onsetTracker.prepare(48'000.0);
    std::vector<float> silence(512, 0.0f);
    std::vector<float> attack(512, 0.08f);
    passed &= require(
        !onsetTracker.processBlock(
            silence.data(),
            static_cast<int>(silence.size())),
        "silence must not trigger an onset");
    passed &= require(
        onsetTracker.processBlock(
            attack.data(),
            static_cast<int>(attack.size())),
        "a clear attack should trigger an onset");
    passed &= require(
        onsetTracker.getLastOnsetStrength() >= 3.0f,
        "onset tracker should expose a strong relative attack");
    for (int ringingBlock = 0; ringingBlock < 40; ++ringingBlock)
    {
        const float level =
            0.07f * std::exp(-0.05f * static_cast<float>(ringingBlock));
        std::vector<float> ringing(512, level);
        passed &= require(
            !onsetTracker.processBlock(
                ringing.data(),
                static_cast<int>(ringing.size())),
            "one decaying strum must not retrigger multiple onsets");
    }

    mode1::GuitarChordTracker chordTracker;
    chordTracker.prepare(48'000.0);
    mode1::ChordDetection chordDetection;
    bool chordReady = false;
    mode1::ChordDetection lastChordDetection;
    double phase = 0.0;
    for (int block = 0; block < 12; ++block)
    {
        std::vector<float> cMajor(512);
        for (int sample = 0; sample < 512; ++sample)
        {
            const double time = phase++ / 48'000.0;
            cMajor[static_cast<size_t>(sample)] = static_cast<float>(
                0.05 * std::sin(juce::MathConstants<double>::twoPi * 130.813 * time)
                + 0.05 * std::sin(juce::MathConstants<double>::twoPi * 164.814 * time)
                + 0.05 * std::sin(juce::MathConstants<double>::twoPi * 195.998 * time));
        }
        const bool readyNow = chordTracker.processBlock(
            cMajor.data(),
            static_cast<int>(cMajor.size()),
            block == 0,
            chordDetection);
        if (readyNow)
        {
            chordReady = true;
            lastChordDetection = chordDetection;
        }
    }
    passed &= require(chordReady, "chord tracker did not produce an estimate");
    if (chordReady)
        std::cout << "Synthetic C major detection: root="
                  << lastChordDetection.rootPitchClass
                  << " valid=" << lastChordDetection.valid
                  << " confidence=" << lastChordDetection.confidence << '\n';
    passed &= require(
        lastChordDetection.valid && lastChordDetection.rootPitchClass == 0,
        "C major triad was not detected as a C-root chord");

    const auto detectSyntheticChord =
        [](const std::vector<double>& frequencies)
    {
        mode1::GuitarChordTracker tracker;
        tracker.prepare(48'000.0);
        mode1::ChordDetection result;
        std::vector<double> phases(frequencies.size(), 0.0);
        for (int block = 0; block < 16; ++block)
        {
            std::vector<float> samples(512, 0.0f);
            for (int sample = 0; sample < 512; ++sample)
                for (size_t tone = 0; tone < frequencies.size(); ++tone)
                {
                    phases[tone] +=
                        juce::MathConstants<double>::twoPi
                        * frequencies[tone] / 48'000.0;
                    samples[static_cast<size_t>(sample)] +=
                        static_cast<float>(
                            0.045 * std::sin(phases[tone]));
                }
            mode1::ChordDetection candidate;
            if (tracker.processBlock(
                    samples.data(),
                    static_cast<int>(samples.size()),
                    block == 0,
                    candidate))
                result = candidate;
        }
        return result;
    };

    const auto cMinor = detectSyntheticChord({ 130.813, 155.563, 195.998 });
    passed &= require(
        cMinor.valid
            && cMinor.rootPitchClass == 0
            && cMinor.quality == mode1::ChordQuality::minor,
        "C minor quality was not detected");
    const auto cDiminished =
        detectSyntheticChord({ 130.813, 155.563, 184.997 });
    passed &= require(
        cDiminished.valid
            && cDiminished.rootPitchClass == 0
            && cDiminished.quality == mode1::ChordQuality::diminished,
        "C diminished quality was not detected");
    const auto cSus4 = detectSyntheticChord({ 130.813, 174.614, 195.998 });
    passed &= require(
        cSus4.valid
            && cSus4.rootPitchClass == 0
            && cSus4.quality == mode1::ChordQuality::suspended4,
        "C sus4 quality was not detected");
    const auto cDominant7 =
        detectSyntheticChord({ 130.813, 164.814, 195.998, 233.082 });
    std::cout << "Chord qualities: Cm="
              << cMinor.rootPitchClass << "/"
              << static_cast<int>(cMinor.quality)
              << " Cdim=" << cDiminished.rootPitchClass << "/"
              << static_cast<int>(cDiminished.quality)
              << " Csus4=" << cSus4.rootPitchClass << "/"
              << static_cast<int>(cSus4.quality)
              << " C7=" << cDominant7.rootPitchClass << "/"
              << static_cast<int>(cDominant7.quality) << '\n';
    passed &= require(
        cDominant7.valid
            && cDominant7.rootPitchClass == 0
            && cDominant7.quality == mode1::ChordQuality::dominant7,
        "C dominant seventh quality was not detected");
    const auto cMajorOverE =
        detectSyntheticChord({ 164.814, 261.626, 329.628, 391.995 });
    std::cout << "C/E detection: root=" << cMajorOverE.rootPitchClass
              << " bass=" << cMajorOverE.bassPitchClass
              << " quality=" << static_cast<int>(cMajorOverE.quality)
              << '\n';
    passed &= require(
        cMajorOverE.valid
            && cMajorOverE.rootPitchClass == 0
            && cMajorOverE.bassPitchClass == 4,
        "C/E slash-chord bass was not detected");

    mode1::Mode1Controller controller;
    controller.prepare(48'000.0, 512);
    const double packageLoadStart = juce::Time::getMillisecondCounterHiRes();
    passed &= require(
        controller.loadSongPackage(packageFile, error),
        error.toRawUTF8());
    std::cout << "Controller package load: "
              << juce::Time::getMillisecondCounterHiRes() - packageLoadStart
              << " ms\n";
    passed &= require(
        !controller.isPerformanceRunning(),
        "a loaded song should remain stopped until Start is pressed");
    controller.startPerformance();
    passed &= require(
        controller.isPerformanceRunning(),
        "Start should arm guitar-follow playback");
    controller.stopPerformance();
    passed &= require(
        !controller.isPerformanceRunning(),
        "Stop should disarm playback");
    controller.restartPerformance();
    passed &= require(
        controller.isPerformanceRunning()
            && controller.getCurrentPhraseIndex() < 0,
        "Restart should return to the beginning and arm playback");
    controller.startAutomaticPlayback();
    controller.setManualKeyShift(3);
    passed &= require(
        controller.getSelectedKeyAnchor()
                + controller.getPitchShiftSemitones()
            == controller.getEffectiveBaseKeyShift(),
        "anchor plus residual pitch should equal the requested key");
    controller.stopAutomaticPlayback();
    std::vector<float> boundedPitchLeft(512);
    std::vector<float> boundedPitchRight(512);
    for (const int simulatedRoot : { 5, 5, 10 })
    {
        controller.triggerVirtualChord(simulatedRoot);
        controller.triggerNextPhrase();
        controller.processBlock(
            silence.data(),
            boundedPitchLeft.data(),
            boundedPitchRight.data(),
            static_cast<int>(silence.size()));
    }
    passed &= require(
        controller.getPitchShiftSemitones() == 3,
        "raw chord misclassification must not push vocal pitch past +3 st");
    const double anchorLoadStart = juce::Time::getMillisecondCounterHiRes();
    controller.setManualKeyShift(17);
    std::cout << "Original-key anchor load: "
              << juce::Time::getMillisecondCounterHiRes() - anchorLoadStart
              << " ms\n";
    std::cout << "Original-key selection: max="
              << controller.getMaximumManualKeyShift()
              << " effective=" << controller.getEffectiveBaseKeyShift()
              << " anchor=" << controller.getSelectedKeyAnchor()
              << " residual=" << controller.getResidualKeyShift()
              << " realtime=" << controller.getPitchShiftSemitones()
              << '\n';
    const bool hasOriginalKeyAnchor =
        std::find(
            package.getKeyAnchors().begin(),
            package.getKeyAnchors().end(),
            0) != package.getKeyAnchors().end();
    passed &= require(
        controller.getMaximumManualKeyShift() == 17
            && controller.getEffectiveBaseKeyShift() == 0
            && (!hasOriginalKeyAnchor
                || (controller.getSelectedKeyAnchor() == 0
                    && controller.getResidualKeyShift() == 0
                    && controller.getPitchShiftSemitones() == 0)),
        "original-key anchor should avoid a +17 real-time pitch shift");
    controller.startAutomaticPlayback();

    std::vector<float> outputLeft(512);
    std::vector<float> outputRight(512);
    double outputEnergy = 0.0;
    for (int block = 0; block < 200; ++block)
    {
        controller.processBlock(
            silence.data(),
            outputLeft.data(),
            outputRight.data(),
            static_cast<int>(silence.size()));
        for (float sample : outputLeft)
            outputEnergy += static_cast<double>(sample) * sample;
    }
    passed &= require(outputEnergy > 1.0e-5, "phrase player produced silence");
    passed &= require(
        controller.getCurrentLyrics().isNotEmpty(),
        "controller did not expose current phrase lyrics");

    if (!passed)
        return 1;

    std::cout
        << "PASS: loaded phrases, scheduled playback, detected onset/chord, "
           "and rendered pitch-capable non-silent audio.\n";
    return 0;
}
