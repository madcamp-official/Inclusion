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
    bool loadKeyAnchor(
        const SongPackage& package,
        int keyShift,
        juce::String& error,
        int expressionStrength = -1);
    void clear();

    // accent (0..1, 0.5 = neutral) subtly scales this phrase's playback gain
    // to reflect how hard the guitar was just strummed.
    void requestPhrase(int phraseIndex, float accent = 0.5f) noexcept;
    void setPitchSemitones(float semitones) noexcept
    {
        targetPitchSemitones.store(semitones);
    }
    void setExpressionStrength(int strength) noexcept
    {
        targetExpressionStrength.store(juce::jlimit(0, 100, strength));
    }
    void stop() noexcept;
    void processBlock(float* outputLeft, float* outputRight, int numSamples) noexcept;

    [[nodiscard]] int getCurrentPhraseIndex() const noexcept
    {
        return currentPhraseIndex.load();
    }
    [[nodiscard]] bool isPlaying() const noexcept { return playing.load(); }
    [[nodiscard]] int getActiveVoiceCount() const noexcept;
    [[nodiscard]] int getCurrentExpressionStrength() const noexcept
    {
        return currentExpressionStrength.load();
    }

private:
    struct LoadedPhrase
    {
        struct Variant
        {
            int strength = 25;
            std::shared_ptr<juce::AudioBuffer<float>> audio;
            int contentOffsetSamples = 0;
            int playbackEndSamples = 0;
        };
        std::vector<Variant> variants;
    };

    struct PhraseBank
    {
        int keyShift = 0;
        std::vector<std::unique_ptr<LoadedPhrase>> phrases;
    };

    struct PlaybackVoice
    {
        std::shared_ptr<const PhraseBank> bank;
        int phraseIndex = -1;
        int variantIndex = 0;
        int sourcePosition = 0;
        int samplesSinceStart = 0;
        int fadeInLength = 1;
        int fadeOutRemaining = 0;
        bool sourceFlushed = false;
        bool active = false;
        float currentPitchSemitones = 0.0f;
        float accentGain = 1.0f;
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
    std::shared_ptr<PhraseBank> currentBank;

    double outputSampleRate = 48'000.0;
    int fadeInSamples = 1;
    int crossfadeSamples = 1;
    int endFadeSamples = 1;
    int newestVoiceIndex = -1;
    std::array<PlaybackVoice, 2> voices;
    std::atomic<float> targetPitchSemitones { 0.0f };
    std::atomic<int> targetExpressionStrength { 25 };
    std::atomic<int> currentExpressionStrength { 25 };

    std::atomic<int> requestedPhraseIndex { -1 };
    std::atomic<float> requestedAccent { 0.5f };
    std::atomic<int> currentPhraseIndex { -1 };
    std::atomic<bool> playing { false };

};

} // namespace mode1
