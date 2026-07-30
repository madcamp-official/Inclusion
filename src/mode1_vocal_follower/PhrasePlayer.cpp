#include "PhrasePlayer.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

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
    // A 20 ms tail is short enough to still click on a sustained vowel, so
    // phrase ends get their own, longer ramp.
    endFadeSamples = std::max(1, juce::roundToInt(outputSampleRate * 0.035));
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
        // Quickseek picks cheaper correlation points; on sustained vocal
        // material that shows up as periodic roughness, and we only ever
        // run two mono voices so the CPU saving is not needed.
        voice.soundTouch.setSetting(SETTING_USE_QUICKSEEK, 0);
#endif
    }
}

bool PhrasePlayer::load(const SongPackage& package, juce::String& error)
{
    clear();
    return loadKeyAnchor(package, package.getBaseKeyShift(), error);
}

bool PhrasePlayer::loadKeyAnchor(
    const SongPackage& package,
    int keyShift,
    juce::String& error,
    int expressionStrength)
{
    error.clear();
    auto loaded = std::make_shared<PhraseBank>();
    loaded->keyShift = keyShift;
    loaded->phrases.reserve(package.getPhrases().size());
    std::unordered_map<
        std::string,
        std::shared_ptr<juce::AudioBuffer<float>>> audioCache;

    for (const auto& phrase : package.getPhrases())
    {
        auto item = std::make_unique<LoadedPhrase>();
        const VocalVariant* selectedVariant = nullptr;
        int selectedDistance = 101;
        if (expressionStrength >= 0)
        {
            for (const auto& candidate : phrase.vocalVariants)
            {
                if (candidate.keyShift != keyShift)
                    continue;
                const int distance =
                    std::abs(candidate.strength - expressionStrength);
                if (distance < selectedDistance)
                {
                    selectedVariant = &candidate;
                    selectedDistance = distance;
                }
            }
        }
        for (const auto& sourceVariant : phrase.vocalVariants)
        {
            if (sourceVariant.keyShift != keyShift)
                continue;
            if (selectedVariant != nullptr
                && &sourceVariant != selectedVariant)
                continue;
            const auto cacheKey =
                sourceVariant.vocalFile.getFullPathName().toStdString();
            auto cached = audioCache.find(cacheKey);
            std::shared_ptr<juce::AudioBuffer<float>> resampled;
            double sourceSampleRate = outputSampleRate;
            if (cached == audioCache.end())
            {
                std::unique_ptr<juce::AudioFormatReader> reader(
                    formatManager.createReaderFor(sourceVariant.vocalFile));
                if (reader == nullptr)
                {
                    error = "Could not read vocal variant: "
                        + sourceVariant.vocalFile.getFullPathName();
                    return false;
                }
                juce::AudioBuffer<float> source;
                source.setSize(
                    1, static_cast<int>(reader->lengthInSamples));
                if (!reader->read(
                        &source,
                        0,
                        static_cast<int>(reader->lengthInSamples),
                        0,
                        true,
                        true))
                {
                    error = "Failed while reading vocal variant: "
                        + sourceVariant.vocalFile.getFullPathName();
                    return false;
                }

                sourceSampleRate = reader->sampleRate;
                const double ratio = outputSampleRate / sourceSampleRate;
                const int outputLength = std::max(
                    2,
                    juce::roundToInt(reader->lengthInSamples * ratio));
                resampled =
                    std::make_shared<juce::AudioBuffer<float>>(1, outputLength);
                const auto* input = source.getReadPointer(0);
                auto* output = resampled->getWritePointer(0);
                if (std::abs(sourceSampleRate - outputSampleRate) < 0.5)
                {
                    juce::FloatVectorOperations::copy(
                        output, input, outputLength);
                }
                else
                {
                    const double inputStep =
                        sourceSampleRate / outputSampleRate;
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
                        const float fraction =
                            static_cast<float>(position - index);
                        output[sample] = input[index]
                            + fraction * (input[next] - input[index]);
                    }
                }
                audioCache.emplace(cacheKey, resampled);
            }
            else
            {
                resampled = cached->second;
            }

            LoadedPhrase::Variant variant;
            variant.strength = sourceVariant.strength;
            variant.audio = std::move(resampled);
            variant.contentOffsetSamples = juce::jlimit(
                0,
                std::max(0, variant.audio->getNumSamples() - 1),
                juce::roundToInt((
                    sourceVariant.playbackStartSeconds >= 0.0
                        ? sourceVariant.playbackStartSeconds
                        : sourceVariant.contentOffsetSeconds)
                    * outputSampleRate));
            variant.playbackEndSamples =
                sourceVariant.playbackEndSeconds >= 0.0
                ? juce::jlimit(
                    variant.contentOffsetSamples + 1,
                    variant.audio->getNumSamples(),
                    juce::roundToInt(
                        sourceVariant.playbackEndSeconds
                        * outputSampleRate))
                : variant.audio->getNumSamples();
            item->variants.push_back(std::move(variant));
        }
        if (item->variants.empty())
        {
            error = "Phrase " + phrase.id
                + " has no vocal audio for key anchor "
                + juce::String(keyShift) + " st.";
            return false;
        }
        loaded->phrases.push_back(std::move(item));
    }

    currentBank = std::move(loaded);
    return currentBank != nullptr && !currentBank->phrases.empty();
}

void PhrasePlayer::clear()
{
    stop();
    currentBank.reset();
}

void PhrasePlayer::requestPhrase(
    int phraseIndex,
    float accent,
    double targetDurationSeconds,
    double transitionSeconds,
    int startDelaySamples,
    double leadInSeconds) noexcept
{
    requestedAccent.store(juce::jlimit(0.0f, 1.0f, accent));
    requestedDurationSeconds.store(std::max(0.0, targetDurationSeconds));
    requestedTransitionSeconds.store(
        juce::jlimit(0.004, 0.060, transitionSeconds));
    requestedStartDelaySamples.store(std::max(0, startDelaySamples));
    requestedLeadInSeconds.store(std::max(0.0, leadInSeconds));
    requestedPhraseIndex.store(phraseIndex);
}

void PhrasePlayer::stop() noexcept
{
    requestedPhraseIndex.store(-1);
    requestedStartDelaySamples.store(0);
    delayedPhraseIndex = -1;
    delayedStartSamplesRemaining = 0;
    currentPhraseIndex.store(-1);
    playing.store(false);
    pendingFirstOutputPhraseIndex = -1;
    firstOutputPhraseIndex = -1;
    firstOutputSampleOffset = -1;
    newestVoiceIndex = -1;
    for (auto& voice : voices)
        resetVoice(voice);
}

void PhrasePlayer::fadeOutAndCancel(double fadeSeconds) noexcept
{
    requestedPhraseIndex.store(-1);
    requestedStartDelaySamples.store(0);
    delayedPhraseIndex = -1;
    delayedStartSamplesRemaining = 0;
    pendingFirstOutputPhraseIndex = -1;

    const int requestedFadeSamples = std::max(
        1,
        juce::roundToInt(
            juce::jlimit(0.010, 0.100, fadeSeconds) * outputSampleRate));
    for (auto& voice : voices)
    {
        if (!voice.active)
            continue;
        voice.fadeOutLength = requestedFadeSamples;
        voice.fadeOutRemaining = requestedFadeSamples;
    }
}

void PhrasePlayer::resetVoice(PlaybackVoice& voice) noexcept
{
    voice.bank.reset();
    voice.phraseIndex = -1;
    voice.variantIndex = 0;
    voice.sourcePosition = 0;
    voice.sourceStartPosition = 0;
    voice.samplesSinceStart = 0;
    voice.fadeInLength = 1;
    voice.fadeOutLength = 1;
    voice.fadeOutRemaining = 0;
    voice.sourceFlushed = false;
    voice.active = false;
    voice.currentPitchSemitones = 0.0f;
    voice.accentGain = 1.0f;
    voice.elasticTempo = 1.0f;
    voice.attackEndSample = 0;
    voice.releaseStartSample = 0;
#if HAVE_SOUNDTOUCH
    voice.soundTouch.clear();
#endif
}

void PhrasePlayer::startRequestedPhrase(int phraseIndex) noexcept
{
    const auto bank = currentBank;
    if (bank == nullptr
        || phraseIndex < 0
        || phraseIndex >= static_cast<int>(bank->phrases.size()))
        return;

    const bool hasPreviousVoice =
        newestVoiceIndex >= 0 && voices[static_cast<size_t>(newestVoiceIndex)].active;
    const int requestedTransitionSamples = std::max(
        1,
        juce::roundToInt(
            requestedTransitionSeconds.load() * outputSampleRate));
    if (hasPreviousVoice)
    {
        voices[static_cast<size_t>(newestVoiceIndex)].fadeOutRemaining =
            requestedTransitionSamples;
        voices[static_cast<size_t>(newestVoiceIndex)].fadeOutLength =
            requestedTransitionSamples;
    }

    const int nextVoiceIndex = newestVoiceIndex == 0 ? 1 : 0;
    auto& voice = voices[static_cast<size_t>(nextVoiceIndex)];
    resetVoice(voice);
    voice.bank = bank;
    const auto& phrase =
        *bank->phrases[static_cast<size_t>(phraseIndex)];
    const int requestedStrength = targetExpressionStrength.load();
    int bestDistance = 101;
    for (int index = 0; index < static_cast<int>(phrase.variants.size()); ++index)
    {
        const int distance =
            std::abs(phrase.variants[static_cast<size_t>(index)].strength
                - requestedStrength);
        if (distance < bestDistance)
        {
            bestDistance = distance;
            voice.variantIndex = index;
        }
    }
    const auto& variant =
        phrase.variants[static_cast<size_t>(voice.variantIndex)];
    currentExpressionStrength.store(variant.strength);
    voice.phraseIndex = phraseIndex;
    const int requestedLeadInSamples = juce::jlimit(
        0,
        variant.contentOffsetSamples,
        juce::roundToInt(requestedLeadInSeconds.load() * outputSampleRate));
    voice.sourcePosition =
        variant.contentOffsetSamples - requestedLeadInSamples;
    voice.sourceStartPosition = voice.sourcePosition;
    voice.fadeInLength =
        hasPreviousVoice ? requestedTransitionSamples : fadeInSamples;
    voice.currentPitchSemitones = targetPitchSemitones.load();
    // Subtle range (+/-8%): audible dynamics without pumping or drawing
    // attention to itself between neighbouring phrases.
    voice.accentGain = 0.92f + 0.16f * requestedAccent.load();
    // Stage 2: preserve the consonant attack and release, and let only the
    // stable middle of a mora absorb modest performance-tempo differences.
    // The bounds intentionally avoid the metallic artifacts caused by using
    // a micro clip as an unlimited time-stretch loop.
    const int playableSamples =
        variant.playbackEndSamples - variant.contentOffsetSamples;
    const double naturalSeconds = playableSamples / outputSampleRate;
    const double targetSeconds = requestedDurationSeconds.load();
    if (targetSeconds > 0.02 && naturalSeconds > 0.02)
        voice.elasticTempo = static_cast<float>(juce::jlimit(
            0.86, 1.16, naturalSeconds / targetSeconds));
    voice.attackEndSample = variant.contentOffsetSamples
        + std::min(playableSamples / 3, juce::roundToInt(0.045 * outputSampleRate));
    voice.releaseStartSample = variant.playbackEndSamples
        - std::min(playableSamples / 3, juce::roundToInt(0.035 * outputSampleRate));
    voice.active = true;
#if HAVE_SOUNDTOUCH
    voice.soundTouch.setPitchSemiTones(voice.currentPitchSemitones);
    voice.soundTouch.setTempo(1.0);
#endif
    newestVoiceIndex = nextVoiceIndex;
    currentPhraseIndex.store(phraseIndex);
    playing.store(true);
    pendingFirstOutputPhraseIndex = phraseIndex;
}

void PhrasePlayer::renderVoice(
    PlaybackVoice& voice,
    float* outputLeft,
    float* outputRight,
    int numSamples) noexcept
{
    if (!voice.active
        || voice.bank == nullptr
        || voice.phraseIndex < 0
        || voice.phraseIndex
            >= static_cast<int>(voice.bank->phrases.size()))
        return;

    const auto& phrase =
        *voice.bank->phrases[static_cast<size_t>(voice.phraseIndex)];
    const auto& variant =
        phrase.variants[static_cast<size_t>(voice.variantIndex)];
    const int sourceLength = variant.playbackEndSamples;
    const auto* source = variant.audio->getReadPointer(0);
    const float targetPitch = targetPitchSemitones.load();
    const float maximumPitchChange =
        static_cast<float>(numSamples / outputSampleRate * 16.0);
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
#if HAVE_SOUNDTOUCH
            const bool stableVowel =
                voice.sourcePosition >= voice.attackEndSample
                && voice.sourcePosition < voice.releaseStartSample;
            voice.soundTouch.setTempo(
                stableVowel ? voice.elasticTempo : 1.0f);
#endif
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

    // Equal-power (sin) ramps. Two linear ramps summed across a voice
    // change drop to ~-3 dB at the midpoint for uncorrelated material,
    // which is exactly the momentary dip heard as a cut at every phrase
    // boundary; sin/cos pairs keep the summed power flat instead.
    const auto equalPower = [](float linear) noexcept
    {
        return std::sin(juce::MathConstants<float>::halfPi
            * juce::jlimit(0.0f, 1.0f, linear));
    };

    // Measured from where this voice actually started (which may be earlier
    // than contentOffsetSamples when a lead-in was requested), so the tail
    // edge-fade lands at the clip's real end rather than firing early.
    const int playbackLength =
        variant.playbackEndSamples - voice.sourceStartPosition;

    for (int sample = 0; sample < produced; ++sample)
    {
        const float fadeIn = equalPower(
            static_cast<float>(voice.samplesSinceStart) / voice.fadeInLength);
        const float fadeOut = voice.fadeOutRemaining > 0
            ? equalPower(
                static_cast<float>(voice.fadeOutRemaining)
                    / std::max(1, voice.fadeOutLength))
            : 1.0f;
        // Always ramp the tail down, even for variants without an explicit
        // playback end: running the source buffer dry mid-vowel and simply
        // resetting the voice is a hard discontinuity.
        const int remainingSamples =
            playbackLength - voice.samplesSinceStart;
        const float edgeFade = equalPower(
            static_cast<float>(remainingSamples)
                / std::max(1, endFadeSamples));
        const float value =
            voice.scratch[static_cast<size_t>(sample)]
            * fadeIn * fadeOut * edgeFade * voice.accentGain;
        if (voice.phraseIndex == pendingFirstOutputPhraseIndex
            && std::abs(value) > 1.0e-7f)
        {
            firstOutputPhraseIndex = voice.phraseIndex;
            firstOutputSampleOffset = sample;
            pendingFirstOutputPhraseIndex = -1;
        }
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
    firstOutputPhraseIndex = -1;
    firstOutputSampleOffset = -1;
    std::fill(outputLeft, outputLeft + numSamples, 0.0f);
    std::fill(outputRight, outputRight + numSamples, 0.0f);

    const int requested = requestedPhraseIndex.exchange(-1);
    if (requested >= 0)
    {
        delayedPhraseIndex = requested;
        delayedStartSamplesRemaining =
            requestedStartDelaySamples.exchange(0);
    }

    const int samplesBeforeStart = delayedPhraseIndex >= 0
        ? std::min(numSamples, delayedStartSamplesRemaining)
        : numSamples;
    if (samplesBeforeStart > 0)
    {
        for (auto& voice : voices)
            renderVoice(
                voice, outputLeft, outputRight, samplesBeforeStart);
    }

    if (delayedPhraseIndex >= 0)
    {
        delayedStartSamplesRemaining -= samplesBeforeStart;
        if (delayedStartSamplesRemaining <= 0)
        {
            const int phraseToStart = delayedPhraseIndex;
            delayedPhraseIndex = -1;
            startRequestedPhrase(phraseToStart);
            const int remaining = numSamples - samplesBeforeStart;
            if (remaining > 0)
            {
                for (auto& voice : voices)
                    renderVoice(
                        voice,
                        outputLeft + samplesBeforeStart,
                        outputRight + samplesBeforeStart,
                        remaining);
                if (firstOutputSampleOffset >= 0)
                    firstOutputSampleOffset += samplesBeforeStart;
            }
        }
    }
    else if (samplesBeforeStart < numSamples)
    {
        for (auto& voice : voices)
            renderVoice(
                voice,
                outputLeft + samplesBeforeStart,
                outputRight + samplesBeforeStart,
                numSamples - samplesBeforeStart);
    }

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
