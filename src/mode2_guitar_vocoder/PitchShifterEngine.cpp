#include "PitchShifterEngine.h"
#include "params/Mode2Params.h"

#include <algorithm>

void PitchShifterEngine::prepare(double sampleRateIn, int maxBlockSize)
{
    sampleRate = sampleRateIn;

#ifdef HAVE_SOUNDTOUCH
    soundTouch.setSampleRate(static_cast<uint>(sampleRateIn));
    soundTouch.setChannels(1);
    soundTouch.setPitchSemiTones(0.0);

    // auto 모드를 쓰지 않고 짧은 시퀀스를 명시한다. auto는 tempo=1.0에서 sequence를 73ms로
    // 잡는데, 시프트량 변경이 시퀀스 경계에서만 반영되므로 빠른 음 변화가 뭉개진다.
    soundTouch.setSetting(SETTING_SEQUENCE_MS, mode2::params::pitchShiftSequenceMs);
    soundTouch.setSetting(SETTING_SEEKWINDOW_MS, mode2::params::pitchShiftSeekWindowMs);
    soundTouch.setSetting(SETTING_OVERLAP_MS, mode2::params::pitchShiftOverlapMs);
    soundTouch.setSetting(SETTING_USE_QUICKSEEK, mode2::params::pitchShiftUseQuickSeek ? 1 : 0);

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
    // 글라이드 속도는 실행 중에 바뀔 수 있으므로 매 블록 읽는다.
    const float perSample = glideSemitonesPerSecond.load() / static_cast<float>(sampleRate);
    const float maxDelta = perSample * static_cast<float>(numSamples);
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
