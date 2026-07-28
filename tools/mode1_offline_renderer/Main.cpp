#include "mode1_vocal_follower/Mode1Controller.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <vector>

namespace
{
std::unique_ptr<juce::AudioFormatWriter> createWriter(
    const juce::File& file,
    double sampleRate)
{
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();
    if (stream == nullptr)
        return {};

    juce::WavAudioFormat wav;
    const auto options = juce::AudioFormatWriterOptions {}
        .withSampleRate(sampleRate)
        .withNumChannels(2)
        .withBitsPerSample(24);
    return wav.createWriterFor(stream, options);
}

float peakMagnitude(const juce::AudioBuffer<float>& audio)
{
    float peak = 0.0f;
    for (int channel = 0; channel < audio.getNumChannels(); ++channel)
        peak = std::max(
            peak,
            audio.getMagnitude(channel, 0, audio.getNumSamples()));
    return peak;
}

void applySafetyGain(juce::AudioBuffer<float>& audio)
{
    const float peak = peakMagnitude(audio);
    if (peak > 0.98f)
        audio.applyGain(0.98f / peak);
}
} // namespace

int main(int argc, char* argv[])
{
    if (argc < 4 || argc > 6)
    {
        std::cerr
            << "usage: Mode1OfflineRenderer <song_package.json> "
               "<guitar.wav> <output-directory> [block-size] [guitar-gain]\n";
        return 2;
    }

    const juce::File packageFile(
        juce::String::fromUTF8(argv[1]));
    const juce::File guitarFile(
        juce::String::fromUTF8(argv[2]));
    const juce::File outputDirectory(
        juce::String::fromUTF8(argv[3]));
    const int blockSize = argc >= 5
        ? std::max(16, std::atoi(argv[4]))
        : 480;
    const float guitarGain = argc >= 6
        ? std::max(0.0f, static_cast<float>(std::atof(argv[5])))
        : 2.0f;

    if (!packageFile.existsAsFile())
    {
        std::cerr << "song package not found: "
                  << packageFile.getFullPathName() << '\n';
        return 1;
    }
    if (!guitarFile.existsAsFile())
    {
        std::cerr << "guitar WAV not found: "
                  << guitarFile.getFullPathName() << '\n';
        return 1;
    }
    if (!outputDirectory.createDirectory())
    {
        std::cerr << "could not create output directory: "
                  << outputDirectory.getFullPathName() << '\n';
        return 1;
    }

    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> guitarReader(
        formatManager.createReaderFor(guitarFile));
    if (guitarReader == nullptr)
    {
        std::cerr << "could not read guitar WAV\n";
        return 1;
    }
    if (guitarReader->numChannels < 1)
    {
        std::cerr << "guitar WAV has no channels\n";
        return 1;
    }

    const double sampleRate = guitarReader->sampleRate;
    const auto inputSamples64 = guitarReader->lengthInSamples;
    if (inputSamples64 <= 0
        || inputSamples64 > std::numeric_limits<int>::max() - sampleRate * 6.0)
    {
        std::cerr << "unsupported guitar WAV length\n";
        return 1;
    }

    juce::AudioBuffer<float> guitar(
        1, static_cast<int>(inputSamples64));
    if (!guitarReader->read(
            &guitar,
            0,
            guitar.getNumSamples(),
            0,
            true,
            false))
    {
        std::cerr << "failed while reading guitar WAV\n";
        return 1;
    }

    constexpr double tailSeconds = 5.0;
    const int totalSamples = guitar.getNumSamples()
        + juce::roundToInt(sampleRate * tailSeconds);
    juce::AudioBuffer<float> vocals(2, totalSamples);
    juce::AudioBuffer<float> combined(2, totalSamples);
    vocals.clear();
    combined.clear();

    mode1::Mode1Controller controller;
    controller.prepare(sampleRate, blockSize);
    controller.setOutputLatencySeconds(blockSize / sampleRate);
    juce::String loadError;
    if (!controller.loadSongPackage(packageFile, loadError))
    {
        std::cerr << "failed to load song package: "
                  << loadError << '\n';
        return 1;
    }
    controller.startPerformance();

    std::vector<float> inputBlock(static_cast<size_t>(blockSize), 0.0f);
    std::vector<float> left(static_cast<size_t>(blockSize), 0.0f);
    std::vector<float> right(static_cast<size_t>(blockSize), 0.0f);
    juce::StringArray eventRows;
    eventRows.add("time_sec,phrase_index,lyrics");
    juce::StringArray trackingRows;
    trackingRows.add(
        "time_sec,chord_event_index,tempo_scale,recovered_skipped_chords");
    juce::StringArray detectionRows;
    detectionRows.add("time_sec,raw_detected_chord");
    int previousPhrase = -1;
    int previousChordEvent = -1;
    juce::String previousRawChord;
    const double renderStartMs =
        juce::Time::getMillisecondCounterHiRes();

    for (int start = 0; start < totalSamples; start += blockSize)
    {
        const int count = std::min(blockSize, totalSamples - start);
        std::fill(inputBlock.begin(), inputBlock.end(), 0.0f);
        if (start < guitar.getNumSamples())
        {
            const int inputCount =
                std::min(count, guitar.getNumSamples() - start);
            std::copy(
                guitar.getReadPointer(0, start),
                guitar.getReadPointer(0, start) + inputCount,
                inputBlock.begin());
        }
        std::fill(left.begin(), left.end(), 0.0f);
        std::fill(right.begin(), right.end(), 0.0f);
        controller.processBlock(
            inputBlock.data(),
            left.data(),
            right.data(),
            count);

        vocals.copyFrom(0, start, left.data(), count);
        vocals.copyFrom(1, start, right.data(), count);
        combined.copyFrom(0, start, left.data(), count);
        combined.copyFrom(1, start, right.data(), count);
        if (start < guitar.getNumSamples())
        {
            const int inputCount =
                std::min(count, guitar.getNumSamples() - start);
            combined.addFrom(
                0, start, guitar, 0, start, inputCount, guitarGain);
            combined.addFrom(
                1, start, guitar, 0, start, inputCount, guitarGain);
        }

        const int phrase = controller.getCurrentPhraseIndex();
        if (phrase >= 0 && phrase != previousPhrase)
        {
            auto lyrics = controller.getCurrentLyrics()
                .replace("\"", "\"\"");
            eventRows.add(
                juce::String(start / sampleRate, 6)
                + "," + juce::String(phrase)
                + ",\"" + lyrics + "\"");
            previousPhrase = phrase;
        }
        const int chordEvent = controller.getCurrentChordEventIndex();
        if (chordEvent >= 0 && chordEvent != previousChordEvent)
        {
            trackingRows.add(
                juce::String(start / sampleRate, 6)
                + "," + juce::String(chordEvent)
                + "," + juce::String(
                    controller.getPerformanceTempoScale(), 6)
                + "," + juce::String(
                    controller.getRecoveredSkippedChordCount()));
            previousChordEvent = chordEvent;
        }
        const auto rawChord = controller.getRawDetectedChordName();
        if (rawChord.isNotEmpty()
            && rawChord != "-"
            && rawChord != previousRawChord)
        {
            detectionRows.add(
                juce::String(start / sampleRate, 6)
                + ",\"" + rawChord.replace("\"", "\"\"") + "\"");
            previousRawChord = rawChord;
        }
    }
    const double renderCpuMs =
        juce::Time::getMillisecondCounterHiRes() - renderStartMs;

    applySafetyGain(vocals);
    applySafetyGain(combined);
    const auto vocalFile =
        outputDirectory.getChildFile("app_vocals_from_guitar.wav");
    const auto combinedFile =
        outputDirectory.getChildFile("guitar_plus_app_vocals.wav");
    auto vocalWriter = createWriter(vocalFile, sampleRate);
    auto combinedWriter = createWriter(combinedFile, sampleRate);
    if (vocalWriter == nullptr || combinedWriter == nullptr)
    {
        std::cerr << "could not create output WAV files\n";
        return 1;
    }
    if (!vocalWriter->writeFromAudioSampleBuffer(
            vocals, 0, vocals.getNumSamples())
        || !combinedWriter->writeFromAudioSampleBuffer(
            combined, 0, combined.getNumSamples()))
    {
        std::cerr << "failed while writing output WAV files\n";
        return 1;
    }
    vocalWriter.reset();
    combinedWriter.reset();

    const auto eventsFile =
        outputDirectory.getChildFile("phrase_events.csv");
    if (!eventsFile.replaceWithText(
            eventRows.joinIntoString("\n") + "\n"))
    {
        std::cerr << "could not write phrase event log\n";
        return 1;
    }
    const auto trackingFile =
        outputDirectory.getChildFile("score_tracking_events.csv");
    if (!trackingFile.replaceWithText(
            trackingRows.joinIntoString("\n") + "\n"))
    {
        std::cerr << "could not write score tracking log\n";
        return 1;
    }
    const auto detectionFile =
        outputDirectory.getChildFile("raw_chord_detections.csv");
    if (!detectionFile.replaceWithText(
            detectionRows.joinIntoString("\n") + "\n"))
    {
        std::cerr << "could not write raw chord detection log\n";
        return 1;
    }

    std::cout << "sample_rate=" << sampleRate << '\n'
              << "block_size=" << blockSize << '\n'
              << "input_duration_sec="
              << guitar.getNumSamples() / sampleRate << '\n'
              << "render_duration_sec=" << totalSamples / sampleRate << '\n'
              << "render_cpu_ms=" << renderCpuMs << '\n'
              << "realtime_factor="
              << (totalSamples / sampleRate)
                    / std::max(0.001, renderCpuMs / 1000.0)
              << '\n'
              << "phrase_events=" << eventRows.size() - 1 << '\n'
              << "tempo_scale=" << controller.getPerformanceTempoScale() << '\n'
              << "recovered_skipped_chords="
              << controller.getRecoveredSkippedChordCount() << '\n'
              << "vocal_peak=" << peakMagnitude(vocals) << '\n'
              << "combined_peak=" << peakMagnitude(combined) << '\n'
              << "vocal_output=" << vocalFile.getFullPathName() << '\n'
              << "combined_output=" << combinedFile.getFullPathName() << '\n'
              << "event_log=" << eventsFile.getFullPathName() << '\n'
              << "tracking_log=" << trackingFile.getFullPathName() << '\n';
    return 0;
}
