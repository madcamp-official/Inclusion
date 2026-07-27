#include "PitchShifterEngine.h"
#include "params/Mode2Params.h"

#include <algorithm>

void PitchShifterEngine::prepare(double sampleRateIn, int maxBlockSize)
{
    sampleRate = sampleRateIn;
    const float ratePerSecond = mode2::params::maxCorrectionSemitones / mode2::params::shiftRampSeconds;
    maxShiftChangePerSample = ratePerSecond / static_cast<float>(sampleRate);

#ifdef HAVE_SOUNDTOUCH
    soundTouch.setSampleRate(static_cast<uint>(sampleRateIn));
    soundTouch.setChannels(1);
    soundTouch.setPitchSemiTones(0.0);
    receiveScratch.assign(static_cast<size_t>(maxBlockSize) * 4, 0.0f);
#else
    (void) maxBlockSize;
#endif

    reset();
}

void PitchShifterEngine::reset()
{
    currentShiftSemitones = 0.0f;
#ifdef HAVE_SOUNDTOUCH
    soundTouch.clear();
    outputQueue.clear();
#endif
}

void PitchShifterEngine::processBlock(const float* input, float* output, int numSamples, float requestedSemitones)
{
    const float maxDelta = maxShiftChangePerSample * static_cast<float>(numSamples);
    const float diff = requestedSemitones - currentShiftSemitones;
    if (diff > maxDelta)
        currentShiftSemitones += maxDelta;
    else if (diff < -maxDelta)
        currentShiftSemitones -= maxDelta;
    else
        currentShiftSemitones = requestedSemitones;

#ifdef HAVE_SOUNDTOUCH
    soundTouch.setPitchSemiTones(static_cast<double>(currentShiftSemitones));
    soundTouch.putSamples(input, static_cast<uint>(numSamples));

    for (;;)
    {
        const uint received = soundTouch.receiveSamples(receiveScratch.data(), static_cast<uint>(receiveScratch.size()));
        if (received == 0)
            break;
        outputQueue.insert(outputQueue.end(), receiveScratch.begin(), receiveScratch.begin() + received);
        if (received < receiveScratch.size())
            break;
    }

    const int available = static_cast<int>(outputQueue.size());
    const int toCopy = std::min(available, numSamples);
    for (int i = 0; i < toCopy; ++i)
        output[i] = outputQueue[static_cast<size_t>(i)];
    for (int i = toCopy; i < numSamples; ++i)
        output[i] = 0.0f; // SoundTouch 초기 지연으로 아직 안 채워진 구간은 무음으로 둔다.
    outputQueue.erase(outputQueue.begin(), outputQueue.begin() + toCopy);
#else
    for (int i = 0; i < numSamples; ++i)
        output[i] = input[i];
#endif
}
