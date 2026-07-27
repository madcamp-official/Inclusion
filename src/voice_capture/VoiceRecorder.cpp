#include "VoiceRecorder.h"

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
}

bool VoiceRecorder::start()
{
    const juce::SpinLock::ScopedLockType scopedLock(lock);
    if (sampleRate <= 0.0 || recordedAudio.getNumSamples() == 0)
        return false;

    writePosition = 0;
    recordedAudio.clear();
    recording.store(true);
    return true;
}

void VoiceRecorder::stop()
{
    recording.store(false);
}

void VoiceRecorder::processBlock(const juce::AudioBuffer<float>& input)
{
    if (!recording.load() || input.getNumChannels() == 0)
        return;

    const juce::SpinLock::ScopedTryLockType scopedLock(lock);
    if (!scopedLock.isLocked())
        return;

    const auto remaining = recordedAudio.getNumSamples() - writePosition;
    const auto samplesToCopy = juce::jmin(remaining, input.getNumSamples());
    if (samplesToCopy > 0)
    {
        recordedAudio.copyFrom(0, writePosition, input, 0, 0, samplesToCopy);
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

juce::Result VoiceRecorder::saveAsWav(const juce::File& destination) const
{
    const juce::SpinLock::ScopedLockType scopedLock(lock);
    if (writePosition == 0 || sampleRate <= 0.0)
        return juce::Result::fail("저장할 녹음이 없습니다.");

    if (destination.existsAsFile() && !destination.deleteFile())
        return juce::Result::fail("기존 파일을 덮어쓸 수 없습니다.");

    std::unique_ptr<juce::OutputStream> stream = destination.createOutputStream();
    if (stream == nullptr)
        return juce::Result::fail("WAV 파일을 만들 수 없습니다.");

    juce::WavAudioFormat format;
    const auto options = juce::AudioFormatWriterOptions()
        .withSampleRate(sampleRate)
        .withNumChannels(1)
        .withBitsPerSample(24);
    auto writer = format.createWriterFor(stream, options);
    if (writer == nullptr)
        return juce::Result::fail("WAV 인코더를 초기화할 수 없습니다.");

    if (!writer->writeFromAudioSampleBuffer(recordedAudio, 0, writePosition))
        return juce::Result::fail("WAV 파일을 쓰는 중 오류가 발생했습니다.");

    return juce::Result::ok();
}
}
