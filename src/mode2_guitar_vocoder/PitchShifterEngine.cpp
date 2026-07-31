#include "PitchShifterEngine.h"
#include "params/Mode2Params.h"

#include <algorithm>
#include <cmath>

bool PitchShifterEngine::isBackendAvailable(Backend backend)
{
    switch (backend)
    {
        case Backend::World:
#if HAVE_WORLD
            return true;
#else
            return false;
#endif
        case Backend::RubberBand:
        case Backend::RubberBandLowLatency:
#if HAVE_RUBBERBAND
            return true;
#else
            return false;
#endif
        case Backend::SoundTouch:
#if HAVE_SOUNDTOUCH
            return true;
#else
            return false;
#endif
    }
    return false;
}

const char* PitchShifterEngine::getBackendName(Backend backend)
{
    switch (backend)
    {
        case Backend::World: return "world";
        case Backend::RubberBand: return "rubberband-r3";
        case Backend::RubberBandLowLatency: return "rubberband-r2-low-latency";
        case Backend::SoundTouch: return "soundtouch";
    }
    return "unknown";
}

void PitchShifterEngine::setBackend(Backend requested)
{
    if (isBackendAvailable(requested))
        activeBackend = requested;
    else if (isBackendAvailable(Backend::World))
        activeBackend = Backend::World;
    else if (isBackendAvailable(Backend::RubberBand))
        activeBackend = Backend::RubberBand;
    else
        activeBackend = Backend::SoundTouch;
}

void PitchShifterEngine::prepare(double sampleRateIn, int maxBlockSize)
{
    sampleRate = sampleRateIn;
    latencySamples = 0;

#if HAVE_WORLD
    world.prepare(sampleRateIn, maxBlockSize);
#endif

#if HAVE_RUBBERBAND
    using Stretcher = RubberBand::RubberBandStretcher;
    const bool useLowLatencyRubberBand =
        activeBackend == Backend::RubberBandLowLatency;
    const auto options = Stretcher::OptionProcessRealTime
                       | (useLowLatencyRubberBand
                              ? Stretcher::OptionEngineFaster
                              : Stretcher::OptionEngineFiner)
                       | Stretcher::OptionWindowShort
                       | Stretcher::OptionFormantPreserved
                       | Stretcher::OptionPitchHighConsistency;
    rubberBand = std::make_unique<Stretcher>(
        static_cast<size_t>(sampleRateIn), 1, options, 1.0, 1.0);
    // R3 요청을 모르는 구버전 라이브러리는 옵션을 조용히 무시할 수 있다. 이 호출을
    // 남겨 최소 Rubber Band 3.x 심볼을 링크 단계에서 강제한다.
    (void) rubberBand->getEngineVersion();
    rubberBand->setMaxProcessSize(static_cast<size_t>(maxBlockSize));
    rubberBandReceiveScratch.assign(static_cast<size_t>(maxBlockSize) * 4, 0.0f);
    rubberBandSilentPad.assign(static_cast<size_t>(maxBlockSize), 0.0f);
    rubberBandStartDelaySamples = rubberBand->getStartDelay();
    // R2는 처리량이 평균적으로는 1:1이어도 개별 콜백에서 최대 수십~수백 샘플 적게
    // 내놓을 수 있다. R2에만 콜백 한 블록의 탄력 큐를 둔다. 드물게 이것까지 바닥나면
    // 아래의 짧은 페이드가 급격한 0↔파형 경계를 감춘다. R3는 기존 지연을 늘리지 않는다.
    rubberBandSafetyDelaySamples =
        useLowLatencyRubberBand ? static_cast<size_t>(maxBlockSize) : 0;
#endif

#if HAVE_SOUNDTOUCH
    soundTouch.setSampleRate(static_cast<uint>(sampleRateIn));
    soundTouch.setChannels(1);
    soundTouch.setPitchSemiTones(0.0);

    // auto 모드를 쓰지 않고 짧은 시퀀스를 명시한다. auto는 tempo=1.0에서 sequence를 73ms로
    // 잡는데, 시프트량 변경이 시퀀스 경계에서만 반영되므로 빠른 음 변화가 뭉개진다.
    soundTouch.setSetting(SETTING_SEQUENCE_MS, mode2::params::pitchShiftSequenceMs);
    soundTouch.setSetting(SETTING_SEEKWINDOW_MS, mode2::params::pitchShiftSeekWindowMs);
    soundTouch.setSetting(SETTING_OVERLAP_MS, mode2::params::pitchShiftOverlapMs);
    soundTouch.setSetting(SETTING_USE_QUICKSEEK, mode2::params::pitchShiftUseQuickSeek ? 1 : 0);

    soundTouchReceiveScratch.assign(static_cast<size_t>(maxBlockSize) * 4, 0.0f);
#endif

#if HAVE_RUBBERBAND
    if ((activeBackend == Backend::RubberBand
         || activeBackend == Backend::RubberBandLowLatency)
        && rubberBand != nullptr)
        latencySamples = static_cast<int>(
            rubberBandStartDelaySamples + rubberBandSafetyDelaySamples);
#endif
#if HAVE_WORLD
    if (activeBackend == Backend::World)
        latencySamples = world.getLatencySamples();
#endif
#if HAVE_SOUNDTOUCH
    if (activeBackend == Backend::SoundTouch)
        latencySamples = soundTouch.getSetting(SETTING_INITIAL_LATENCY);
#endif

#if !HAVE_RUBBERBAND && !HAVE_SOUNDTOUCH
    (void) maxBlockSize;
#endif

    reset();
}

void PitchShifterEngine::reset()
{
    currentShiftSemitones = 0.0f;

#if HAVE_RUBBERBAND
    if (rubberBand != nullptr)
    {
        rubberBand->reset();
        rubberBand->setPitchScale(1.0);
        rubberBandOutputQueue.clear();
        rubberBandRecoveryFadeSamples = 0;
        lastRubberBandOutputSample = 0.0f;

        // 실시간 모드는 시작 패딩/지연 보상을 호출자가 해야 한다. prepare/reset 시 무음을
        // 미리 넣고, 그 출력의 start delay만 버려 실제 첫 음절이 잘리지 않게 한다.
        size_t padRemaining = rubberBand->getPreferredStartPad();
        rubberBandSamplesToDiscard = rubberBandStartDelaySamples;
        while (padRemaining > 0)
        {
            const size_t chunk = std::min(padRemaining, rubberBandSilentPad.size());
            const float* inputs[] = { rubberBandSilentPad.data() };
            rubberBand->process(inputs, chunk, false);
            padRemaining -= chunk;

            while (rubberBand->available() > 0)
            {
                const size_t wanted = std::min(
                    static_cast<size_t>(rubberBand->available()),
                    rubberBandReceiveScratch.size());
                float* outputs[] = { rubberBandReceiveScratch.data() };
                const size_t received = rubberBand->retrieve(outputs, wanted);
                const size_t discard = std::min(received, rubberBandSamplesToDiscard);
                rubberBandSamplesToDiscard -= discard;
                rubberBandOutputQueue.insert(
                    rubberBandOutputQueue.end(),
                    rubberBandReceiveScratch.begin() + static_cast<std::ptrdiff_t>(discard),
                    rubberBandReceiveScratch.begin() + static_cast<std::ptrdiff_t>(received));
            }
        }

        // 실제 입력보다 앞에 안전 여유분을 둔다. 이후 R2가 한 콜백에서 조금 덜
        // 출력해도 이 큐가 흡수하므로 오디오 콜백에 0 샘플을 노출하지 않는다.
        rubberBandOutputQueue.insert(rubberBandOutputQueue.begin(),
                                     rubberBandSafetyDelaySamples, 0.0f);
    }
#endif

#if HAVE_SOUNDTOUCH
    soundTouch.clear();
    soundTouchOutputQueue.clear();
#endif

#if HAVE_WORLD
    world.reset();
#endif
}

void PitchShifterEngine::processBlock(const float* input, float* output, int numSamples,
                                      float requestedSemitones, float absoluteTargetF0Hz)
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

#if HAVE_WORLD
    if (activeBackend == Backend::World)
    {
        world.processBlock(input, output, numSamples, absoluteTargetF0Hz,
                           currentShiftSemitones);
        return;
    }
#endif

#if HAVE_RUBBERBAND
    if ((activeBackend == Backend::RubberBand
         || activeBackend == Backend::RubberBandLowLatency)
        && rubberBand != nullptr)
    {
        processRubberBand(input, output, numSamples);
        return;
    }
#endif

#if HAVE_SOUNDTOUCH
    if (activeBackend == Backend::SoundTouch)
    {
        processSoundTouch(input, output, numSamples);
        return;
    }
#endif

    std::copy(input, input + numSamples, output);
}

void PitchShifterEngine::processRubberBand(const float* input, float* output, int numSamples)
{
#if HAVE_RUBBERBAND
    rubberBand->setPitchScale(std::pow(2.0, static_cast<double>(currentShiftSemitones) / 12.0));
    const float* inputs[] = { input };
    rubberBand->process(inputs, static_cast<size_t>(numSamples), false);

    while (rubberBand->available() > 0)
    {
        const size_t wanted = std::min(
            static_cast<size_t>(rubberBand->available()),
            rubberBandReceiveScratch.size());
        float* outputs[] = { rubberBandReceiveScratch.data() };
        const size_t received = rubberBand->retrieve(outputs, wanted);
        const size_t discard = std::min(received, rubberBandSamplesToDiscard);
        rubberBandSamplesToDiscard -= discard;
        rubberBandOutputQueue.insert(
            rubberBandOutputQueue.end(),
            rubberBandReceiveScratch.begin() + static_cast<std::ptrdiff_t>(discard),
            rubberBandReceiveScratch.begin() + static_cast<std::ptrdiff_t>(received));
    }

    const int toCopy = std::min(static_cast<int>(rubberBandOutputQueue.size()), numSamples);
    for (int i = 0; i < toCopy; ++i)
    {
        float sample = rubberBandOutputQueue[static_cast<size_t>(i)];
        if (rubberBandRecoveryFadeSamples > 0)
        {
            constexpr int recoveryLength = 64;
            const float gain =
                1.0f - static_cast<float>(rubberBandRecoveryFadeSamples)
                           / static_cast<float>(recoveryLength);
            sample *= std::clamp(gain, 0.0f, 1.0f);
            --rubberBandRecoveryFadeSamples;
        }
        output[i] = sample;
    }

    const int missing = numSamples - toCopy;
    if (missing > 0)
    {
        // 0으로 즉시 자르면 마지막 실제 샘플과 0 사이의 수직 경계가 클릭이 된다.
        // 마지막 값에서 0까지 짧게 내려가고, 다음 실제 출력의 앞 64샘플도 올려 붙인다.
        const float start = toCopy > 0 ? output[toCopy - 1] : lastRubberBandOutputSample;
        for (int i = 0; i < missing; ++i)
        {
            const float gain =
                1.0f - static_cast<float>(i + 1) / static_cast<float>(missing);
            output[toCopy + i] = start * gain;
        }
        rubberBandRecoveryFadeSamples = 64;
    }

    if (numSamples > 0)
        lastRubberBandOutputSample = output[numSamples - 1];
    rubberBandOutputQueue.erase(rubberBandOutputQueue.begin(),
                                rubberBandOutputQueue.begin() + toCopy);
#else
    std::copy(input, input + numSamples, output);
#endif
}

void PitchShifterEngine::processSoundTouch(const float* input, float* output, int numSamples)
{
#if HAVE_SOUNDTOUCH
    soundTouch.setPitchSemiTones(static_cast<double>(currentShiftSemitones));
    soundTouch.putSamples(input, static_cast<uint>(numSamples));

    for (;;)
    {
        const uint received = soundTouch.receiveSamples(soundTouchReceiveScratch.data(),
                                                        static_cast<uint>(soundTouchReceiveScratch.size()));
        if (received == 0)
            break;
        soundTouchOutputQueue.insert(soundTouchOutputQueue.end(),
                                     soundTouchReceiveScratch.begin(),
                                     soundTouchReceiveScratch.begin() + received);
        if (received < soundTouchReceiveScratch.size())
            break;
    }

    const int available = static_cast<int>(soundTouchOutputQueue.size());
    const int toCopy = std::min(available, numSamples);
    for (int i = 0; i < toCopy; ++i)
        output[i] = soundTouchOutputQueue[static_cast<size_t>(i)];
    for (int i = toCopy; i < numSamples; ++i)
        output[i] = 0.0f; // SoundTouch 초기 지연으로 아직 안 채워진 구간은 무음으로 둔다.
    soundTouchOutputQueue.erase(soundTouchOutputQueue.begin(),
                                soundTouchOutputQueue.begin() + toCopy);
#else
    std::copy(input, input + numSamples, output);
#endif
}
