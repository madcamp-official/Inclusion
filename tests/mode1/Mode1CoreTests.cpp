#include "mode1_vocal_follower/GuitarOnsetTracker.h"
#include "mode1_vocal_follower/GuitarChordTracker.h"
#include "mode1_vocal_follower/Mode1Controller.h"
#include "mode1_vocal_follower/PhrasePlayer.h"
#include "mode1_vocal_follower/PhraseScheduler.h"
#include "mode1_vocal_follower/SongPackage.h"

#include <juce_core/juce_core.h>

#include <iostream>
#include <cmath>
#include <memory>
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
  "schema_version": 2,
  "song": "fixture",
  "score_bpm": 120.0,
  "base_key_shift": -17,
  "vocal_style": {
    "range_warnings": [
      { "type": "above_comfortable_range", "semitones": 1.0 }
    ]
  },
  "micro_phrases": [
    {
      "phrase_id": "micro_000",
      "lyrics": "君",
      "score": { "start_sec": 0.0, "end_sec": 0.45 },
      "source": { "start_sec": 0.0, "end_sec": 0.45 },
      "chords": [{ "start_sec": 0.0, "chord": "C", "raw_chord": "C" }],
      "vocal": { "directory": ".", "file": "tone.wav", "content_offset_sec": 0.0 }
    },
    {
      "phrase_id": "micro_001",
      "lyrics": "を",
      "score": { "start_sec": 0.5, "end_sec": 0.95 },
      "source": { "start_sec": 0.5, "end_sec": 0.95 },
      "chords": [{ "start_sec": 0.5, "chord": "F", "raw_chord": "F" }],
      "vocal": { "directory": ".", "file": "tone.wav", "content_offset_sec": 0.0 }
    },
    {
      "phrase_id": "micro_002",
      "lyrics": "歌",
      "score": { "start_sec": 1.0, "end_sec": 1.45 },
      "source": { "start_sec": 1.0, "end_sec": 1.45 },
      "chords": [{ "start_sec": 1.0, "chord": "G", "raw_chord": "G" }],
      "vocal": { "directory": ".", "file": "tone.wav", "content_offset_sec": 0.0 }
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
    passed &= require(
        scheduler.processBlock(512, false, true) == 0,
        "manual start should trigger phrase zero");
    passed &= require(
        scheduler.processBlock(512, false, true) == 1,
        "second manual trigger should advance to phrase one");

    mode1::PhraseScheduler fallbackScheduler;
    fallbackScheduler.prepare(48'000.0);
    fallbackScheduler.setSong(&package);
    passed &= require(
        fallbackScheduler.processBlock(512, false, true) == 0,
        "fallback scheduler should start phrase zero");
    const double nextTargetSeconds =
        package.getPhrases()[1].scoreStartSeconds
        - package.getPhrases()[0].scoreStartSeconds;
    double fallbackElapsedSeconds = 0.0;
    int fallbackResult = -1;
    while (fallbackElapsedSeconds < nextTargetSeconds + 0.5)
    {
        fallbackResult =
            fallbackScheduler.processBlock(512, false, false);
        fallbackElapsedSeconds += 512.0 / 48'000.0;
        if (fallbackResult >= 0)
            break;
    }
    passed &= require(
        fallbackResult == 1
            && fallbackElapsedSeconds <= nextTargetSeconds + 0.22,
        "missed onset should not create a long silent gap");

    mode1::PhrasePlayer phrasePlayer;
    phrasePlayer.prepare(48'000.0, 512);
    passed &= require(
        phrasePlayer.load(package, error),
        error.toRawUTF8());
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

    mode1::Mode1Controller controller;
    controller.prepare(48'000.0, 512);
    passed &= require(
        controller.loadSongPackage(packageFile, error),
        error.toRawUTF8());
    controller.triggerNextPhrase();

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
