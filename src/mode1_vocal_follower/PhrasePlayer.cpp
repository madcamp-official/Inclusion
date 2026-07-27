#include "PhrasePlayer.h"

#include <algorithm>
#include <cmath>

namespace mode1
{

PhrasePlayer::PhrasePlayer()
{
    formatManager.registerBasicFormats();
}

void PhrasePlayer::prepare(double newOutputSampleRate, int maximumBlockSize)
{
    outputSampleRate = std::max(1.0, newOutputSampleRate);
    fadeInSamples = std::max(1, juce::roundToInt(outputSampleRate * 0.020));
    crossfadeSamples = std::max(1, juce::roundToInt(outputSampleRate * 0.045));
    for (auto& voice : voices)
    {
        voice.scratch.assign(
            static_cast<size_t>(std::max(1, maximumBlockSize)),
            0.0f);
#if HAVE_SOUNDTOUCH
        voice.soundTouch.setSampleRate(static_cast<unsigned int>(outputSampleRate));
        voice.soundTouch.setChannels(1);
        voice.soundTouch.setTempo(1.0);
        voice.soundTouch.setRate(1.0);
        voice.soundTouch.setPitchSemiTones(0.0);
        voice.soundTouch.setSetting(SETTING_USE_QUICKSEEK, 1);
#endif
    }
}

bool PhrasePlayer::load(const SongPackage& package, juce::String& error)
{
    clear();
    error.clear();

    std::vector<std::unique_ptr<LoadedPhrase>> loaded;
    loaded.reserve(package.getPhrases().size());

    for (const auto& phrase : package.getPhrases())
    {
        std::unique_ptr<juce::AudioFormatReader> reader(
            formatManager.createReaderFor(phrase.vocalFile));
        if (reader == nullptr)
        {
            error = "Could not read phrase audio: " + phrase.vocalFile.getFullPathName();
            return false;
        }

        juce::AudioBuffer<float> source;
        source.setSize(1, static_cast<int>(reader->lengthInSamples) + 1);
        if (!reader->read(
                &source,
                0,
                static_cast<int>(reader->lengthInSamples),
                0,
                true,
                true))
        {
            error = "Failed while reading phrase audio: " + phrase.vocalFile.getFullPathName();
            return false;
        }

        auto item = std::make_unique<LoadedPhrase>();
        const double ratio = outputSampleRate / reader->sampleRate;
        const int outputLength = std::max(
            2,
            juce::roundToInt(reader->lengthInSamples * ratio));
        item->audio.setSize(1, outputLength);
        const auto* input = source.getReadPointer(0);
        auto* output = item->audio.getWritePointer(0);
        const double inputStep = reader->sampleRate / outputSampleRate;
        for (int sample = 0; sample < outputLength; ++sample)
        {
            const double position = sample * inputStep;
            const int index = juce::jlimit(
                0,
                static_cast<int>(reader->lengthInSamples) - 1,
                static_cast<int>(position));
            const int next = std::min(
                index + 1,
                static_cast<int>(reader->lengthInSamples) - 1);
            const float fraction = static_cast<float>(position - index);
            output[sample] = input[index]
                + fraction * (input[next] - input[index]);
        }

        item->contentOffsetSamples = juce::jlimit(
            0,
            std::max(0, item->audio.getNumSamples() - 1),
            juce::roundToInt(phrase.contentOffsetSeconds * outputSampleRate));
        loaded.push_back(std::move(item));
    }

    phrases = std::move(loaded);
    return !phrases.empty();
}

void PhrasePlayer::clear()
{
    stop();
    phrases.clear();
}

void PhrasePlayer::requestPhrase(int phraseIndex) noexcept
{
    requestedPhraseIndex.store(phraseIndex);
}

void PhrasePlayer::stop() noexcept
{
    requestedPhraseIndex.store(-1);
    currentPhraseIndex.store(-1);
    playing.store(false);
    newestVoiceIndex = -1;
    for (auto& voice : voices)
        resetVoice(voice);
}

void PhrasePlayer::resetVoice(PlaybackVoice& voice) noexcept
{
    voice.phraseIndex = -1;
    voice.sourcePosition = 0;
    voice.samplesSinceStart = 0;
    voice.fadeInLength = 1;
    voice.fadeOutRemaining = 0;
    voice.sourceFlushed = false;
    voice.active = false;
    voice.currentPitchSemitones = 0.0f;
#if HAVE_SOUNDTOUCH
    voice.soundTouch.clear();
#endif
}

void PhrasePlayer::startRequestedPhrase(int phraseIndex) noexcept
{
    if (phraseIndex < 0 || phraseIndex >= static_cast<int>(phrases.size()))
        return;

    const bool hasPreviousVoice =
        newestVoiceIndex >= 0 && voices[static_cast<size_t>(newestVoiceIndex)].active;
    if (hasPreviousVoice)
        voices[static_cast<size_t>(newestVoiceIndex)].fadeOutRemaining =
            crossfadeSamples;

    const int nextVoiceIndex = newestVoiceIndex == 0 ? 1 : 0;
    auto& voice = voices[static_cast<size_t>(nextVoiceIndex)];
    resetVoice(voice);
    const auto& phrase = *phrases[static_cast<size_t>(phraseIndex)];
    voice.phraseIndex = phraseIndex;
    voice.sourcePosition = phrase.contentOffsetSamples;
    voice.fadeInLength = hasPreviousVoice ? crossfadeSamples : fadeInSamples;
    voice.currentPitchSemitones = targetPitchSemitones.load();
    voice.active = true;
#if HAVE_SOUNDTOUCH
    voice.soundTouch.setPitchSemiTones(voice.currentPitchSemitones);
#endif
    newestVoiceIndex = nextVoiceIndex;
    currentPhraseIndex.store(phraseIndex);
    playing.store(true);
}

void PhrasePlayer::renderVoice(
    PlaybackVoice& voice,
    float* outputLeft,
    float* outputRight,
    int numSamples) noexcept
{
    if (!voice.active
        || voice.phraseIndex < 0
        || voice.phraseIndex >= static_cast<int>(phrases.size()))
        return;

    const auto& phrase = *phrases[static_cast<size_t>(voice.phraseIndex)];
    const int sourceLength = phrase.audio.getNumSamples();
    const auto* source = phrase.audio.getReadPointer(0);
    const float targetPitch = targetPitchSemitones.load();
    const float maximumPitchChange =
        static_cast<float>(numSamples / outputSampleRate * 8.0);
    voice.currentPitchSemitones += juce::jlimit(
        -maximumPitchChange,
        maximumPitchChange,
        targetPitch - voice.currentPitchSemitones);

    int produced = 0;
#if HAVE_SOUNDTOUCH
    voice.soundTouch.setPitchSemiTones(voice.currentPitchSemitones);
    for (int iteration = 0; iteration < 32 && produced < numSamples; ++iteration)
    {
        produced += static_cast<int>(voice.soundTouch.receiveSamples(
            voice.scratch.data() + produced,
            static_cast<unsigned int>(numSamples - produced)));
        if (produced >= numSamples)
            break;

        if (voice.sourcePosition < sourceLength)
        {
            const int feedCount =
                std::min(1024, sourceLength - voice.sourcePosition);
            voice.soundTouch.putSamples(
                source + voice.sourcePosition,
                static_cast<unsigned int>(feedCount));
            voice.sourcePosition += feedCount;
            continue;
        }

        if (!voice.sourceFlushed)
        {
            voice.soundTouch.flush();
            voice.sourceFlushed = true;
            continue;
        }
        break;
    }
#else
    produced = std::min(numSamples, sourceLength - voice.sourcePosition);
    std::copy(
        source + voice.sourcePosition,
        source + voice.sourcePosition + produced,
        voice.scratch.begin());
    voice.sourcePosition += produced;
    if (voice.sourcePosition >= sourceLength)
        voice.sourceFlushed = true;
#endif

    for (int sample = 0; sample < produced; ++sample)
    {
        const float fadeIn = juce::jlimit(
            0.0f,
            1.0f,
            static_cast<float>(voice.samplesSinceStart) / voice.fadeInLength);
        const float fadeOut = voice.fadeOutRemaining > 0
            ? static_cast<float>(voice.fadeOutRemaining) / crossfadeSamples
            : 1.0f;
        const float value =
            voice.scratch[static_cast<size_t>(sample)] * fadeIn * fadeOut;
        outputLeft[sample] += value;
        outputRight[sample] += value;
        ++voice.samplesSinceStart;
        if (voice.fadeOutRemaining > 0 && --voice.fadeOutRemaining == 0)
        {
            resetVoice(voice);
            break;
        }
    }

    if (voice.active && voice.sourceFlushed && produced == 0)
        resetVoice(voice);
}

void PhrasePlayer::processBlock(
    float* outputLeft,
    float* outputRight,
    int numSamples) noexcept
{
    std::fill(outputLeft, outputLeft + numSamples, 0.0f);
    std::fill(outputRight, outputRight + numSamples, 0.0f);

    const int requested = requestedPhraseIndex.exchange(-1);
    if (requested >= 0)
        startRequestedPhrase(requested);

    for (auto& voice : voices)
        renderVoice(voice, outputLeft, outputRight, numSamples);

    const bool anyActive = getActiveVoiceCount() > 0;
    playing.store(anyActive);
    if (!anyActive)
    {
        newestVoiceIndex = -1;
        currentPhraseIndex.store(-1);
    }
}

int PhrasePlayer::getActiveVoiceCount() const noexcept
{
    int count = 0;
    for (const auto& voice : voices)
        count += voice.active ? 1 : 0;
    return count;
}

} // namespace mode1
