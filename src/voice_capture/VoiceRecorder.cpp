#include "VoiceRecorder.h"
#include "NoiseReducer.h"

#include <juce_audio_basics/juce_audio_basics.h>

namespace voice_capture
{
void VoiceRecorder::prepare(double newSampleRate)
{
    const juce::SpinLock::ScopedLockType scopedLock(lock);
    sampleRate = newSampleRate;
    writePosition = 0;
    recordedAudio.setSize(1, static_cast<int>(sampleRate * maximumRecordingSeconds), false, true, true);
    recordedAudio.clear();
    recording.store(false);
    inputPeak.store(0.0f);
}

bool VoiceRecorder::start()
{
    const juce::SpinLock::ScopedLockType scopedLock(lock);
    if (sampleRate <= 0.0 || recordedAudio.getNumSamples() == 0)
        return false;

    writePosition = 0;
    recordedAudio.clear();
    inputPeak.store(0.0f);
    recording.store(true);
    return true;
}

void VoiceRecorder::stop()
{
    recording.store(false);
}

void VoiceRecorder::processBlock(const juce::AudioBuffer<float>& input,
                                 int inputChannel)
{
    if (!recording.load() || input.getNumChannels() == 0)
        return;

    const juce::SpinLock::ScopedTryLockType scopedLock(lock);
    if (!scopedLock.isLocked())
        return;

    const int channel = juce::jlimit(0, input.getNumChannels() - 1, inputChannel);
    const auto remaining = recordedAudio.getNumSamples() - writePosition;
    const auto samplesToCopy = juce::jmin(remaining, input.getNumSamples());
    if (samplesToCopy > 0)
    {
        recordedAudio.copyFrom(0, writePosition, input, channel, 0, samplesToCopy);
        const float blockPeak = input.getMagnitude(channel, 0, samplesToCopy);
        inputPeak.store(juce::jmax(blockPeak, inputPeak.load() * 0.82f));
        writePosition += samplesToCopy;
    }

    if (writePosition >= recordedAudio.getNumSamples())
        recording.store(false);
}

double VoiceRecorder::getRecordedSeconds() const
{
    const juce::SpinLock::ScopedLockType scopedLock(lock);
    return sampleRate > 0.0 ? static_cast<double>(writePosition) / sampleRate : 0.0;
}

RecordingQuality VoiceRecorder::getQuality() const
{
    const juce::SpinLock::ScopedLockType scopedLock(lock);
    juce::AudioBuffer<float> snapshot(1, writePosition);
    snapshot.copyFrom(0, 0, recordedAudio, 0, 0, writePosition);
    return RecordingQualityChecker::analyse(snapshot, sampleRate);
}

juce::Result VoiceRecorder::saveAsWav(const juce::File& destination,
                                      bool applyNoiseReduction) const
{
    const juce::SpinLock::ScopedLockType scopedLock(lock);
    if (writePosition == 0 || sampleRate <= 0.0)
        return juce::Result::fail(L"저장할 녹음이 없습니다.");

    if (destination.existsAsFile() && !destination.deleteFile())
        return juce::Result::fail(L"기존 파일을 덮어쓸 수 없습니다.");

    juce::AudioBuffer<float> output;
    if (std::abs(sampleRate - storageSampleRate) < 1.0)
    {
        output.setSize(1, writePosition);
        output.copyFrom(0, 0, recordedAudio, 0, 0, writePosition);
    }
    else
    {
        const double ratio = storageSampleRate / sampleRate;
        const int outputSamples =
            static_cast<int>(std::ceil(static_cast<double>(writePosition) * ratio));
        output.setSize(1, outputSamples);
        juce::LagrangeInterpolator interpolator;
        interpolator.process(
            sampleRate / storageSampleRate,
            recordedAudio.getReadPointer(0),
            output.getWritePointer(0),
            outputSamples);
    }

    if (applyNoiseReduction)
        NoiseReducer::process(output, storageSampleRate);

    std::unique_ptr<juce::OutputStream> stream = destination.createOutputStream();
    if (stream == nullptr)
        return juce::Result::fail(L"WAV 파일을 만들 수 없습니다.");

    juce::WavAudioFormat format;
    const auto options = juce::AudioFormatWriterOptions()
        .withSampleRate(storageSampleRate)
        .withNumChannels(1)
        .withBitsPerSample(24);
    auto writer = format.createWriterFor(stream, options);
    if (writer == nullptr)
        return juce::Result::fail(L"WAV 인코더를 초기화할 수 없습니다.");

    if (!writer->writeFromAudioSampleBuffer(output, 0, output.getNumSamples()))
        return juce::Result::fail(L"WAV 파일을 쓰는 중 오류가 발생했습니다.");

    return juce::Result::ok();
}
}
