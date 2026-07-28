#include "voice_capture/GuidedRecordingSession.h"
#include "voice_capture/InputLevelCalibrator.h"
#include "voice_capture/NoiseReducer.h"
#include "voice_capture/RecordingQualityChecker.h"
#include "voice_capture/VoiceActivityTracker.h"
#include "voice_capture/VoiceRecorder.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <vector>

namespace
{
float rms(const juce::AudioBuffer<float>& audio, int start, int count)
{
    double squares = 0.0;
    const auto* samples = audio.getReadPointer(0, start);
    for (int i = 0; i < count; ++i)
        squares += static_cast<double>(samples[i]) * samples[i];
    return static_cast<float>(std::sqrt(squares / count));
}

void appendTone(
    std::vector<float>& samples,
    double sampleRate,
    double seconds,
    float amplitude,
    double frequencyHz,
    juce::Random& random)
{
    const int n = static_cast<int>(sampleRate * seconds);
    for (int i = 0; i < n; ++i)
    {
        const float noise = (random.nextFloat() * 2.0f - 1.0f) * 0.003f;
        const float tone = static_cast<float>(
            amplitude * std::sin(juce::MathConstants<double>::twoPi * frequencyHz * i / sampleRate));
        samples.push_back(noise + tone);
    }
}

void appendSilence(
    std::vector<float>& samples,
    double sampleRate,
    double seconds,
    juce::Random& random,
    float noiseAmplitude = 0.0015f)
{
    const int n = static_cast<int>(sampleRate * seconds);
    for (int i = 0; i < n; ++i)
        samples.push_back((random.nextFloat() * 2.0f - 1.0f) * noiseAmplitude);
}

void feedTrackerInChunks(
    voice_capture::VoiceActivityTracker& tracker,
    const std::vector<float>& samples,
    int chunkSize,
    int* onsetCount = nullptr)
{
    int index = 0;
    while (index < static_cast<int>(samples.size()))
    {
        const int count = std::min(chunkSize, static_cast<int>(samples.size()) - index);
        tracker.processBlock(samples.data() + index, count);
        if (onsetCount != nullptr && tracker.consumeWordOnset())
            ++(*onsetCount);
        index += count;
    }
}

void feedSessionInChunks(
    voice_capture::GuidedRecordingSession& session,
    const std::vector<float>& samples,
    int chunkSize)
{
    int index = 0;
    while (index < static_cast<int>(samples.size()))
    {
        const int count = std::min(chunkSize, static_cast<int>(samples.size()) - index);
        session.processAudioBlock(samples.data() + index, count);
        index += count;
    }
}
}

int main()
{
    constexpr double sampleRate = 48'000.0;
    constexpr int durationSeconds = 10;
    juce::AudioBuffer<float> audio(
        1,
        static_cast<int>(sampleRate * durationSeconds));
    auto* samples = audio.getWritePointer(0);

    juce::Random random(12345);
    for (int i = 0; i < audio.getNumSamples(); ++i)
    {
        const float noise = (random.nextFloat() * 2.0f - 1.0f) * 0.012f;
        const float voice = i < static_cast<int>(sampleRate)
            ? 0.0f
            : static_cast<float>(0.16 * std::sin(
                juce::MathConstants<double>::twoPi * 220.0 * i / sampleRate));
        samples[i] = noise + voice;
    }

    const auto quality =
        voice_capture::RecordingQualityChecker::analyse(audio, sampleRate);
    if (!quality.passed || quality.snrDb < 15.0f)
    {
        std::cerr << "Expected a clean synthetic recording to pass.\n";
        return 1;
    }

    {
        // Regression: real users begin speaking immediately after the
        // countdown. The first second must not be misclassified as noise.
        juce::AudioBuffer<float> immediateSpeech(
            1, static_cast<int>(sampleRate * 8.0));
        immediateSpeech.clear();
        auto* immediate = immediateSpeech.getWritePointer(0);
        for (int i = 0; i < static_cast<int>(sampleRate * 3.0); ++i)
            immediate[i] = static_cast<float>(
                0.18 * std::sin(
                    juce::MathConstants<double>::twoPi * 190.0 * i
                    / sampleRate));
        const auto immediateQuality =
            voice_capture::RecordingQualityChecker::analyse(
                immediateSpeech, sampleRate);
        if (!immediateQuality.passed)
        {
            std::cerr
                << "Immediate speech with trailing silence should pass: "
                << immediateQuality.summary << "\n";
            return 1;
        }
    }

    {
        voice_capture::InputLevelCalibrator calibrator;
        calibrator.prepare(sampleRate);
        calibrator.start();

        std::vector<float> calibrationAudio;
        juce::Random calibrationRandom(2026);
        appendSilence(
            calibrationAudio, sampleRate, 1.5, calibrationRandom, 0.002f);
        appendTone(
            calibrationAudio,
            sampleRate,
            3.0,
            0.7f,
            220.0,
            calibrationRandom);
        for (int start = 0;
             start < static_cast<int>(calibrationAudio.size());
             start += 480)
        {
            calibrator.processBlock(
                calibrationAudio.data() + start,
                std::min(
                    480,
                    static_cast<int>(calibrationAudio.size()) - start));
        }

        const auto calibration = calibrator.getResult();
        if (calibration.phase
                != voice_capture::InputLevelCalibrator::Phase::complete
            || calibration.recommendedGain >= 0.9f)
        {
            std::cerr
                << "Expected a loud calibration voice to produce attenuation.\n";
            return 1;
        }
    }

    const float roomToneBefore = rms(audio, 0, static_cast<int>(sampleRate));
    const float voiceBefore = rms(
        audio,
        static_cast<int>(sampleRate),
        static_cast<int>(sampleRate));
    voice_capture::NoiseReducer::process(audio, sampleRate);
    const float roomToneAfter = rms(audio, 0, static_cast<int>(sampleRate));
    const float voiceAfter = rms(
        audio,
        static_cast<int>(sampleRate),
        static_cast<int>(sampleRate));

    if (roomToneAfter >= roomToneBefore * 0.75f)
    {
        std::cerr << "Noise reducer did not attenuate room tone enough.\n";
        return 1;
    }
    if (voiceAfter <= voiceBefore * 0.65f)
    {
        std::cerr << "Noise reducer damaged the voiced signal.\n";
        return 1;
    }

    voice_capture::VoiceRecorder recorder;
    constexpr double deviceSampleRate = 44'100.0;
    recorder.prepare(deviceSampleRate);
    if (!recorder.start())
        return 1;

    juce::AudioBuffer<float> input(1, 441);
    for (int block = 0; block < 1'000; ++block)
    {
        auto* inputSamples = input.getWritePointer(0);
        for (int i = 0; i < input.getNumSamples(); ++i)
        {
            const int sampleIndex = block * input.getNumSamples() + i;
            const float noise = (random.nextFloat() * 2.0f - 1.0f) * 0.008f;
            const float voice = sampleIndex < static_cast<int>(deviceSampleRate)
                ? 0.0f
                : static_cast<float>(0.15 * std::sin(
                    juce::MathConstants<double>::twoPi
                        * 220.0 * sampleIndex / deviceSampleRate));
            inputSamples[i] = noise + voice;
        }
        recorder.processBlock(input, 0);
    }
    recorder.stop();

    const auto wavFile = juce::File::createTempFile(".wav");
    if (recorder.saveAsWav(wavFile, true).failed())
        return 1;

    juce::WavAudioFormat wav;
    auto inputStream = wavFile.createInputStream();
    std::unique_ptr<juce::AudioFormatReader> reader(
        wav.createReaderFor(inputStream.release(), true));
    const bool formatIsCorrect =
        reader != nullptr
        && reader->sampleRate == voice_capture::VoiceRecorder::storageSampleRate
        && reader->numChannels == 1
        && reader->bitsPerSample == 24;
    wavFile.deleteFile();
    if (!formatIsCorrect)
    {
        std::cerr << "Stored WAV did not match 48 kHz mono 24-bit PCM.\n";
        return 1;
    }

    {
        voice_capture::VoiceActivityTracker tracker;
        tracker.prepare(sampleRate);
        juce::Random vadRandom(777);

        std::vector<float> toneSamples;
        appendSilence(toneSamples, sampleRate, 1.0, vadRandom);
        appendTone(toneSamples, sampleRate, 0.3, 0.2f, 220.0, vadRandom);
        appendSilence(toneSamples, sampleRate, 0.3, vadRandom);
        appendTone(toneSamples, sampleRate, 0.3, 0.2f, 220.0, vadRandom);
        appendSilence(toneSamples, sampleRate, 0.3, vadRandom);

        int onsetCount = 0;
        feedTrackerInChunks(tracker, toneSamples, 480, &onsetCount);
        if (onsetCount != 2)
        {
            std::cerr << "Expected 2 word onsets from 2 tone bursts, got "
                      << onsetCount << ".\n";
            return 1;
        }
    }

    {
        voice_capture::VoiceActivityTracker tracker;
        tracker.prepare(sampleRate);
        juce::Random vadRandom(778);

        std::vector<float> toneSamples;
        appendSilence(toneSamples, sampleRate, 3.0, vadRandom);

        int onsetCount = 0;
        feedTrackerInChunks(tracker, toneSamples, 480, &onsetCount);
        if (onsetCount != 0 || tracker.isVoiced())
        {
            std::cerr << "Continuous low-level noise should not register as voiced.\n";
            return 1;
        }
    }

    {
        voice_capture::GuidedRecordingSession session;
        session.prepare(sampleRate);
        session.start();

        auto state = session.getState();
        if (!state.countdownActive
            || state.stage != voice_capture::GuidedRecordingStage::speaking)
        {
            std::cerr << "Expected the session to start in the speaking stage countdown.\n";
            return 1;
        }

        {
            std::vector<float> countdownSilence(
                static_cast<size_t>(sampleRate * 1.6), 0.0f);
            session.processAudioBlock(
                countdownSilence.data(), static_cast<int>(countdownSilence.size()));
        }
        state = session.getState();
        if (state.countdownActive || !state.recordingActive)
        {
            std::cerr << "Expected recording to be active once the countdown elapses.\n";
            return 1;
        }

        juce::Random speechRandom(999);
        std::vector<float> spokenAudio;
        appendTone(spokenAudio, sampleRate, 2.0, 0.18f, 180.0, speechRandom);
        feedSessionInChunks(session, spokenAudio, 480);
        state = session.getState();
        if (state.highlightedWordIndex != -1 || state.awaitingFinalize)
        {
            std::cerr << "Manual recording must not colour or auto-finalize a prompt.\n";
            return 1;
        }

        session.requestSkip();
        state = session.getState();
        if (state.itemIndexInStage != 1 || !session.shouldBeCapturing())
        {
            std::cerr << "Manual next must keep the continuous take running.\n";
            return 1;
        }

        while (session.getState().stage == voice_capture::GuidedRecordingStage::speaking)
            session.requestSkip();
        while (session.getState().stage == voice_capture::GuidedRecordingStage::vowel)
            session.requestSkip();

        state = session.getState();
        if (state.stage != voice_capture::GuidedRecordingStage::song)
        {
            std::cerr << "Expected the session to reach the song stage after skipping.\n";
            return 1;
        }

        // Song-stage prompts must tokenize per syllable, not per word.
        state = session.getState();
        if (state.words.size() < 2)
        {
            std::cerr << "Expected the song prompt to tokenize into syllables.\n";
            return 1;
        }
        for (const auto& token : state.words)
        {
            if (token.length() != 1)
            {
                std::cerr << "Song-stage tokens should be single syllables, got '"
                          << token << "'.\n";
                return 1;
            }
        }

        // Clear the get-ready countdown, then confirm the take is rolling.
        if (!session.shouldBeCapturing())
        {
            std::cerr << "Expected capture to continue into the song stage.\n";
            return 1;
        }

        // Sing continuously and confirm the recorder is never asked to stop
        // as lyric lines roll over — that stop/start is exactly what used to
        // chop the singer off at every line boundary.
        juce::Random singRandom(4242);
        const int lineAtStart = session.getState().itemIndexInStage;
        session.requestSkip();
        if (session.getState().itemIndexInStage == lineAtStart
            || !session.shouldBeCapturing())
        {
            std::cerr << "Manual lyric advance interrupted the continuous take.\n";
            return 1;
        }

        for (int chunk = 0; chunk < 176; ++chunk)
        {
            std::vector<float> sungAudio;
            appendTone(sungAudio, sampleRate, 1.0, 0.2f, 210.0, singRandom);
            feedSessionInChunks(session, sungAudio, 480);
        }

        session.requestSkip();
        if (session.getState().awaitingFinalize)
        {
            std::cerr << "The complete action must stay locked before 180 seconds.\n";
            return 1;
        }

        std::vector<float> finalAudio;
        appendTone(finalAudio, sampleRate, 3.0, 0.2f, 210.0, singRandom);
        feedSessionInChunks(session, finalAudio, 480);
        session.requestSkip();
        state = session.getState();
        if (!state.awaitingFinalize)
        {
            std::cerr << "Expected explicit completion after the 180-second floor.\n";
            return 1;
        }

        session.notifyItemFinalized();
        if (session.isActive() || !session.getState().sessionFinished)
        {
            std::cerr << "Expected the session to be finished after the song stage.\n";
            return 1;
        }
    }

    std::cout << "Voice capture quality and noise reduction tests passed.\n";
    return 0;
}
