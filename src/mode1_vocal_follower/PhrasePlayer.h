#pragma once

#include "SongPackage.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <atomic>
#include <array>
#include <memory>
#include <vector>

#if HAVE_SOUNDTOUCH
#include <SoundTouch.h>
#endif

namespace mode1
{

class PhrasePlayer
{
public:
    PhrasePlayer();

    void prepare(double outputSampleRate, int maximumBlockSize);
    bool load(const SongPackage& package, juce::String& error);
    void clear();

    void requestPhrase(int phraseIndex) noexcept;
    void setPitchSemitones(float semitones) noexcept
    {
        targetPitchSemitones.store(semitones);
    }
    void stop() noexcept;
    void processBlock(float* outputLeft, float* outputRight, int numSamples) noexcept;

    [[nodiscard]] int getCurrentPhraseIndex() const noexcept
    {
        return currentPhraseIndex.load();
    }
    [[nodiscard]] bool isPlaying() const noexcept { return playing.load(); }
    [[nodiscard]] int getActiveVoiceCount() const noexcept;

private:
    struct LoadedPhrase
    {
        juce::AudioBuffer<float> audio;
        int contentOffsetSamples = 0;
    };

    struct PlaybackVoice
    {
        int phraseIndex = -1;
        int sourcePosition = 0;
        int samplesSinceStart = 0;
        int fadeInLength = 1;
        int fadeOutRemaining = 0;
        bool sourceFlushed = false;
        bool active = false;
        float currentPitchSemitones = 0.0f;
        std::vector<float> scratch;
#if HAVE_SOUNDTOUCH
        soundtouch::SoundTouch soundTouch;
#endif
    };

    void startRequestedPhrase(int phraseIndex) noexcept;
    void resetVoice(PlaybackVoice& voice) noexcept;
    void renderVoice(
        PlaybackVoice& voice,
        float* outputLeft,
        float* outputRight,
        int numSamples) noexcept;

    juce::AudioFormatManager formatManager;
    std::vector<std::unique_ptr<LoadedPhrase>> phrases;

    double outputSampleRate = 48'000.0;
    int fadeInSamples = 1;
    int crossfadeSamples = 1;
    int newestVoiceIndex = -1;
    std::array<PlaybackVoice, 2> voices;
    std::atomic<float> targetPitchSemitones { 0.0f };

    std::atomic<int> requestedPhraseIndex { -1 };
    std::atomic<int> currentPhraseIndex { -1 };
    std::atomic<bool> playing { false };

};

} // namespace mode1
