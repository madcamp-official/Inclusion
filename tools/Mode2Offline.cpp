// 모드 2 오프라인 재현 도구.
//
// 앱이 녹음한 3채널 WAV(ch0=기타 입력, ch1=목소리 입력(부스트 전), ch2=당시 최종 출력)를 읽어,
// 앱과 똑같은 Mode2Controller에 같은 블록 크기로 통과시켜 출력 WAV를 만든다.
//
// 왜 필요한가: 파라미터를 바꿀 때마다 다시 연주하면 "좋아진 것"과 "연주가 달라진 것"을 구분할
// 수 없다. 같은 녹음을 반복 재생하면 변수가 파라미터 하나만 남아 A/B 비교가 성립한다.
//
// 컴파일 시간 상수(params/Mode2Params.h)는 재빌드가 필요하고, Mode2Controller의 setter로
// 노출된 값은 아래 옵션으로 바꿀 수 있다.
//
// 사용법:
//   Mode2Offline <입력.wav> <출력.wav> [옵션]
//     --glide=250      글라이드 속도(반음/초)
//     --boost=4.0      목소리 입력 부스트
//     --volume=1.5     최종 출력 볼륨
//     --octave=-1      목표 옥타브 이동
//     --gate=0.048     노이즈 게이트 임계(부스트 후 RMS). --gate=off 로 끌 수 있다
//     --howlguard=off  하울링 억제(기본 off — 되먹임 없는 오프라인이므로)
//     --bleed=on|off   기타 유입 상쇄(목소리 채널에서 기타를 지운다)
//     --trip=0.35      출력 트립 임계
//     --block=512      블록 크기

#include <juce_audio_formats/juce_audio_formats.h>

#include "mode2_guitar_vocoder/Mode2Controller.h"
#include "params/Mode2Params.h"

#include <cmath>
#include <iostream>
#include <algorithm>
#include <memory>
#include <vector>

namespace
{
    struct Options
    {
        float glide = mode2::params::defaultGlideSemitonesPerSecond;
        float boost = mode2::params::defaultVocalInputGain;
        float volume = mode2::params::defaultOutputVolume;
        int octave = mode2::params::targetOctaveShift;
        float gate = mode2::params::vocalNoiseGateThreshold;
        bool gateEnabled = true;
        // 오프라인에는 스피커→마이크 되먹임이 없으므로 기본으로 끈다. 켜면 정상 신호를
        // 하울링으로 오판해 게인을 눌러버려 파라미터 비교가 오염된다.
        bool howlGuard = false;
        bool bleedCancel = mode2::params::defaultBleedCancelEnabled;
        float trip = mode2::params::outputTripThreshold;
        int blockSize = 512;
    };

    bool matchFloat(const juce::String& arg, const char* key, float& out)
    {
        const juce::String prefix = juce::String("--") + key + "=";
        if (! arg.startsWith(prefix))
            return false;
        out = arg.substring(prefix.length()).getFloatValue();
        return true;
    }

    bool matchInt(const juce::String& arg, const char* key, int& out)
    {
        const juce::String prefix = juce::String("--") + key + "=";
        if (! arg.startsWith(prefix))
            return false;
        out = arg.substring(prefix.length()).getIntValue();
        return true;
    }

    float rms(const float* data, int n)
    {
        if (n <= 0)
            return 0.0f;
        double sum = 0.0;
        for (int i = 0; i < n; ++i)
            sum += static_cast<double>(data[i]) * data[i];
        return static_cast<float>(std::sqrt(sum / n));
    }
}

int main(int argc, char* argv[])
{
    if (argc < 3)
    {
        std::cout << "usage: Mode2Offline <input.wav> <output.wav> [--glide=N --boost=N --volume=N "
                     "--octave=N --gate=N|off --howlguard=on|off --trip=N --block=N]\n";
        return 1;
    }

    const juce::File inFile { juce::File::getCurrentWorkingDirectory().getChildFile(argv[1]) };
    const juce::File outFile { juce::File::getCurrentWorkingDirectory().getChildFile(argv[2]) };

    Options opt;
    for (int i = 3; i < argc; ++i)
    {
        const juce::String a { argv[i] };
        if (matchFloat(a, "glide", opt.glide)) continue;
        if (matchFloat(a, "boost", opt.boost)) continue;
        if (matchFloat(a, "volume", opt.volume)) continue;
        if (matchFloat(a, "trip", opt.trip)) continue;
        if (matchInt(a, "octave", opt.octave)) continue;
        if (matchInt(a, "block", opt.blockSize)) continue;
        if (a == "--gate=off") { opt.gateEnabled = false; continue; }
        if (matchFloat(a, "gate", opt.gate)) { opt.gateEnabled = true; continue; }
        if (a == "--howlguard=on")  { opt.howlGuard = true;  continue; }
        if (a == "--howlguard=off") { opt.howlGuard = false; continue; }
        if (a == "--bleed=on")      { opt.bleedCancel = true;  continue; }
        if (a == "--bleed=off")     { opt.bleedCancel = false; continue; }
        std::cerr << "알 수 없는 옵션: " << argv[i] << "\n";
        return 1;
    }

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader { formats.createReaderFor(inFile) };
    if (reader == nullptr)
    {
        std::cerr << "입력 WAV를 열 수 없습니다: " << inFile.getFullPathName() << "\n";
        return 1;
    }
    if (reader->numChannels < 2)
    {
        std::cerr << "입력에 최소 2채널(기타, 목소리)이 필요합니다. 실제: " << reader->numChannels << "\n";
        return 1;
    }

    const int numSamples = static_cast<int>(reader->lengthInSamples);
    const double sampleRate = reader->sampleRate;
    const int block = juce::jmax(32, opt.blockSize);

    juce::AudioBuffer<float> input(static_cast<int>(reader->numChannels), numSamples);
    reader->read(&input, 0, numSamples, 0, true, true);

    Mode2Controller controller;
    controller.prepare(sampleRate, block);
    controller.setVocalInputGain(opt.boost);
    controller.setOutputVolume(opt.volume);
    controller.setTargetOctaveShift(opt.octave);
    controller.setGlideRate(opt.glide);
    controller.setNoiseGateEnabled(opt.gateEnabled);
    controller.setNoiseGateThreshold(opt.gate);
    controller.setHowlGuardEnabled(opt.howlGuard);
    controller.setBleedCancelEnabled(opt.bleedCancel);
    controller.setOutputTripThreshold(opt.trip);

    // 출력은 3채널로 남긴다: 재현 출력 / 기타 입력 / 목소리 입력.
    // 분석 스크립트가 입력과 나란히 놓고 비교할 수 있어야 하기 때문이다.
    juce::AudioBuffer<float> output(3, numSamples);
    output.clear();

    std::vector<float> outL(static_cast<size_t>(block), 0.0f);
    std::vector<float> outR(static_cast<size_t>(block), 0.0f);

    int clipped = 0;
    // wet 분포를 센다. wet이 낮은 블록은 보정 안 된 원본 목소리가 그만큼 섞여 나가므로,
    // "두 음정이 동시에 들린다"는 증상의 직접 원인이 된다.
    int wetBuckets[5] = { 0, 0, 0, 0, 0 };   // 0, 0~0.25, 0.25~0.5, 0.5~0.9, 0.9~1
    int activeBlocks = 0;
    std::vector<float> corrections;
    // 유입 상쇄가 목소리 채널에서 실제로 얼마나 걷어내고 있는지. 값이 크면 뺄 게 있다는 뜻이고,
    // 유입이 없는 녹음에서도 이 값이 크면 목소리를 깎고 있다는 신호다.
    std::vector<float> cancelDbs;
    for (int pos = 0; pos < numSamples; pos += block)
    {
        const int n = juce::jmin(block, numSamples - pos);
        controller.processBlock(input.getReadPointer(0, pos),
                                input.getReadPointer(1, pos),
                                outL.data(), outR.data(), n);

        if (rms(input.getReadPointer(1, pos), n) > 0.005f)
        {
            ++activeBlocks;
            const auto st = controller.getDisplayState();
            if (opt.bleedCancel && st.bleedCancelAdapting)
                cancelDbs.push_back(st.bleedCancelledDb);
            // 보정량 궤적을 블록 단위로 모은다. 시프트가 블록마다 크게 움직이면 WSOLA가
            // 매번 다시 이어붙여야 해서 비배음이 생긴다(합성 실험: ±0.5반음 변조 → 13~32%).
            if (st.hasVocalPitch)
                corrections.push_back(st.correctedMidi - st.vocalMidi);
            const float wet = st.wetness;
            if (wet <= 0.001f)      ++wetBuckets[0];
            else if (wet < 0.25f)   ++wetBuckets[1];
            else if (wet < 0.5f)    ++wetBuckets[2];
            else if (wet < 0.9f)    ++wetBuckets[3];
            else                    ++wetBuckets[4];
        }

        output.copyFrom(0, pos, outL.data(), n);
        output.copyFrom(1, pos, input.getReadPointer(0, pos), n);
        output.copyFrom(2, pos, input.getReadPointer(1, pos), n);

        for (int i = 0; i < n; ++i)
            if (std::abs(outL[static_cast<size_t>(i)]) >= mode2::params::outputLimitPeak * 0.9999f)
                ++clipped;
    }

    outFile.deleteFile();
    if (auto stream = std::unique_ptr<juce::FileOutputStream>(outFile.createOutputStream()))
    {
        juce::WavAudioFormat wav;
        if (auto* writer = wav.createWriterFor(stream.get(), sampleRate, 3, 24, {}, 0))
        {
            stream.release();
            std::unique_ptr<juce::AudioFormatWriter> owned { writer };
            owned->writeFromAudioSampleBuffer(output, 0, numSamples);
        }
        else
        {
            std::cerr << "출력 WAV writer를 만들 수 없습니다\n";
            return 1;
        }
    }

    std::cout << "입력 " << inFile.getFileName() << "  " << sampleRate << "Hz  "
              << juce::String(numSamples / sampleRate, 2) << "초  블록 " << block << "\n"
              << "설정  글라이드=" << opt.glide << "  부스트=" << opt.boost
              << "  볼륨=" << opt.volume << "  옥타브=" << opt.octave
              << "  게이트=" << (opt.gateEnabled ? juce::String(opt.gate) : juce::String("off"))
              << "  하울링억제=" << (opt.howlGuard ? "on" : "off")
              << "  유입상쇄=" << (opt.bleedCancel ? "on" : "off") << "\n"
              << "출력 RMS " << juce::String(rms(output.getReadPointer(0), numSamples), 5)
              << "  피크 " << juce::String(output.getMagnitude(0, 0, numSamples), 5)
              << "  리미터 접촉 " << juce::String(100.0 * clipped / numSamples, 2) << "%\n";

    if (activeBlocks > 0)
    {
        const char* labels[5] = { "wet=0 (원본 100%)", "wet 0~0.25", "wet 0.25~0.5", "wet 0.5~0.9", "wet 0.9~1 (보정만)" };
        std::cout << "목소리가 있는 블록 " << activeBlocks << "개의 wet 분포:\n";
        for (int i = 0; i < 5; ++i)
            std::cout << "   " << labels[i] << " : " << wetBuckets[i]
                      << " (" << juce::String(100.0 * wetBuckets[i] / activeBlocks, 1) << "%)\n";
    }

    if (! cancelDbs.empty())
    {
        std::sort(cancelDbs.begin(), cancelDbs.end());
        std::cout << "기타 유입 상쇄: 목소리 채널에서 걷어낸 양 중앙값 "
                  << juce::String(cancelDbs[cancelDbs.size() / 2], 2) << "dB  최대 "
                  << juce::String(cancelDbs.back(), 2) << "dB\n";
    }

    if (corrections.size() > 2)
    {
        std::vector<float> deltas;
        deltas.reserve(corrections.size() - 1);
        for (size_t i = 1; i < corrections.size(); ++i)
            deltas.push_back(std::abs(corrections[i] - corrections[i - 1]));
        std::sort(deltas.begin(), deltas.end());
        auto pct = [&deltas](double p)
        {
            return deltas[static_cast<size_t>(juce::jlimit(0.0, deltas.size() - 1.0, p * (deltas.size() - 1)))];
        };
        const double blockMs = 1000.0 * block / sampleRate;
        int over[3] = { 0, 0, 0 };
        for (float d : deltas)
        {
            if (d > 0.25f) ++over[0];
            if (d > 0.5f)  ++over[1];
            if (d > 1.0f)  ++over[2];
        }
        std::cout << "보정량 궤적 (블록 " << juce::String(blockMs, 1) << "ms 단위, " << deltas.size() + 1 << "개 표본):\n"
                  << "   블록간 변화 |Δ| 중앙값 " << juce::String(pct(0.5), 3)
                  << "  상위10% " << juce::String(pct(0.9), 2)
                  << "  최대 " << juce::String(deltas.back(), 2) << " 반음\n"
                  << "   0.25반음 초과 " << juce::String(100.0 * over[0] / deltas.size(), 1)
                  << "%   0.5반음 초과 " << juce::String(100.0 * over[1] / deltas.size(), 1)
                  << "%   1.0반음 초과 " << juce::String(100.0 * over[2] / deltas.size(), 1) << "%\n";
    }

    std::cout << "-> " << outFile.getFullPathName() << "\n";
    return 0;
}
