#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include <algorithm>
#include <iostream>
#include <limits>
#include <memory>

int main(int argc, char* argv[])
{
    if (argc != 3)
    {
        std::cerr << "usage: AudioToWav <input-audio> <output.wav>\n";
        return 2;
    }

    const juce::File input(juce::String::fromUTF8(argv[1]));
    const juce::File output(juce::String::fromUTF8(argv[2]));
    juce::AudioFormatManager manager;
    manager.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(
        manager.createReaderFor(input));
    if (reader == nullptr)
    {
        std::cerr << "could not decode: " << input.getFullPathName() << '\n';
        return 1;
    }
    if (reader->lengthInSamples <= 0
        || reader->lengthInSamples > std::numeric_limits<int>::max())
    {
        std::cerr << "unsupported input length\n";
        return 1;
    }

    const int channels = std::clamp(
        static_cast<int>(reader->numChannels), 1, 2);
    juce::AudioBuffer<float> audio(
        channels, static_cast<int>(reader->lengthInSamples));
    if (!reader->read(
            &audio,
            0,
            audio.getNumSamples(),
            0,
            true,
            channels > 1))
    {
        std::cerr << "failed while decoding input\n";
        return 1;
    }

    output.getParentDirectory().createDirectory();
    output.deleteFile();
    std::unique_ptr<juce::OutputStream> stream = output.createOutputStream();
    if (stream == nullptr)
    {
        std::cerr << "could not create output\n";
        return 1;
    }
    juce::WavAudioFormat wav;
    auto writer = wav.createWriterFor(
        stream,
        juce::AudioFormatWriterOptions {}
            .withSampleRate(reader->sampleRate)
            .withNumChannels(channels)
            .withBitsPerSample(24));
    if (writer == nullptr
        || !writer->writeFromAudioSampleBuffer(
            audio, 0, audio.getNumSamples()))
    {
        std::cerr << "failed while writing WAV\n";
        return 1;
    }
    writer.reset();

    std::cout << "sample_rate=" << reader->sampleRate << '\n'
              << "channels=" << channels << '\n'
              << "duration_sec="
              << audio.getNumSamples() / reader->sampleRate << '\n'
              << "output=" << output.getFullPathName() << '\n';
    return 0;
}
