// 기타 채널만 GuitarTargetTracker에 통과시켜 "앱이 보는 목표 음정"을 그대로 찍는다.
// 같은 WAV를 다른 방법으로 분석한 결과와 나란히 놓고 오검출을 판정하기 위한 도구다.
#include "mode2_guitar_vocoder/GuitarTargetTracker.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <iostream>
#include <map>

int main(int argc, char* argv[])
{
    if (argc < 2)
    {
        std::cerr << "usage: GuitarTargetProbe <guitar.wav> [channel]\n";
        return 2;
    }

    const juce::File file(juce::String::fromUTF8(argv[1]));
    const int channel = argc >= 3 ? std::atoi(argv[2]) : 0;

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    if (reader == nullptr)
    {
        std::cerr << "could not read: " << file.getFullPathName() << '\n';
        return 1;
    }

    const int numSamples = static_cast<int>(reader->lengthInSamples);
    juce::AudioBuffer<float> audio(static_cast<int>(reader->numChannels), numSamples);
    reader->read(&audio, 0, numSamples, 0, true, true);

    const int useChannel = juce::jlimit(0, audio.getNumChannels() - 1, channel);
    GuitarTargetTracker tracker;
    tracker.prepare(reader->sampleRate);

    constexpr int block = 512;
    std::map<int, int> histogram;
    int framesWithTarget = 0;
    int frames = 0;
    std::cout << "time_sec,target_midi\n";
    for (int start = 0; start + block <= numSamples; start += block)
    {
        const auto out = tracker.processBlock(
            audio.getReadPointer(useChannel, start), block);
        ++frames;
        if (out.hasTarget)
        {
            ++framesWithTarget;
            const int note = juce::roundToInt(out.targetMidi);
            ++histogram[note];
            std::cout << juce::String(start / reader->sampleRate, 3) << ","
                      << juce::String(out.targetMidi, 2) << "\n";
        }
    }

    std::cerr << "frames=" << frames << "  with_target=" << framesWithTarget << "\n";
    std::cerr << "target MIDI histogram (top):\n";
    std::multimap<int, int, std::greater<int>> byCount;
    for (const auto& [note, count] : histogram)
        byCount.emplace(count, note);
    int shown = 0;
    for (const auto& [count, note] : byCount)
    {
        static const char* names[] = { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };
        std::cerr << "  MIDI " << note << " (" << names[note % 12]
                  << (note / 12 - 1) << ")  " << count << "\n";
        if (++shown >= 12) break;
    }
    return 0;
}
