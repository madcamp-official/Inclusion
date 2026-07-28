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
//     --lookahead=21.3 제어 경로와 오디오 경로의 정렬량(ms). 출력 음정 정확도로 스윕해 정한다
//     --shifter=rubberband|soundtouch|world|world-stream  피치 시프터 A/B
//       world        전체 파일을 한 번에 분석·재합성한다(WORLD 방식의 음질 상한)
//       world-stream 앱이 실제로 쓰는 스트리밍 WORLD 백엔드. 워커 스레드가 따라올 수
//                    있어야 결과가 유효하므로 블록을 실시간 속도로 흘려보낸다(녹음 길이만큼 걸린다)

#include <juce_audio_formats/juce_audio_formats.h>

#include "PitchAccuracyMetrics.h"
#include "mode2_guitar_vocoder/Mode2Controller.h"
#include "mode2_guitar_vocoder/WorldVoiceTransformer.h"
#include "params/Mode2Params.h"

#include <chrono>
#include <cmath>
#include <iostream>
#include <algorithm>
#include <memory>
#include <thread>
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
        PitchShifterEngine::Backend shifter = PitchShifterEngine::Backend::RubberBand;
        bool useWorld = false;
        // 스트리밍 WORLD는 별도 워커에서 분석한다. 오프라인 루프가 블록을 최대 속도로
        // 밀어 넣으면 워커가 못 따라와 출력이 굶어서, 백엔드 잘못이 아닌 이유로 결과가
        // 나빠진다. 그래서 이 경로만 실시간 속도로 흘린다.
        bool paceRealTime = false;
        float lookaheadMs = 1000.0f * mode2::params::vocalControlLookaheadSeconds;
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
                     "--octave=N --gate=N|off --howlguard=on|off --trip=N --block=N --lookahead=N "
                     "--shifter=rubberband|soundtouch|world|world-stream]\n";
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
        if (matchFloat(a, "lookahead", opt.lookaheadMs)) continue;
        if (a == "--gate=off") { opt.gateEnabled = false; continue; }
        if (matchFloat(a, "gate", opt.gate)) { opt.gateEnabled = true; continue; }
        if (a == "--howlguard=on")  { opt.howlGuard = true;  continue; }
        if (a == "--howlguard=off") { opt.howlGuard = false; continue; }
        if (a == "--bleed=on")      { opt.bleedCancel = true;  continue; }
        if (a == "--bleed=off")     { opt.bleedCancel = false; continue; }
        if (a == "--shifter=rubberband")
        {
            opt.shifter = PitchShifterEngine::Backend::RubberBand;
            opt.useWorld = false;
            continue;
        }
        if (a == "--shifter=soundtouch")
        {
            opt.shifter = PitchShifterEngine::Backend::SoundTouch;
            opt.useWorld = false;
            continue;
        }
        if (a == "--shifter=world")
        {
            // 스트리밍 컨트롤러는 보정량 궤적만 만든다. 아래에서 전체 녹음을 WORLD로
            // 재합성해 출력 채널을 덮어쓴다.
            opt.shifter = PitchShifterEngine::Backend::RubberBand;
            opt.useWorld = true;
            continue;
        }
        if (a == "--shifter=world-stream")
        {
            opt.shifter = PitchShifterEngine::Backend::World;
            opt.useWorld = false;
            opt.paceRealTime = true;
            continue;
        }
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
    controller.setPitchShifterBackend(opt.shifter);
    controller.setVocalControlLookahead(0.001f * opt.lookaheadMs);
    controller.prepare(sampleRate, block);
    if (! opt.useWorld && controller.getPitchShifterBackend() != opt.shifter)
    {
        std::cerr << "요청한 시프터(" << PitchShifterEngine::getBackendName(opt.shifter)
                  << ")가 빌드에 없어 "
                  << PitchShifterEngine::getBackendName(controller.getPitchShifterBackend())
                  << "로 폴백합니다.\n";
    }
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
    std::vector<float> appliedShiftTrace(static_cast<size_t>(numSamples), 0.0f);
    std::vector<float> absoluteTargetF0Trace(static_cast<size_t>(numSamples), 0.0f);
    const auto runStart = std::chrono::steady_clock::now();
    for (int pos = 0; pos < numSamples; pos += block)
    {
        if (opt.paceRealTime)
        {
            const auto due = runStart + std::chrono::microseconds(
                static_cast<long long>(1.0e6 * pos / sampleRate));
            std::this_thread::sleep_until(due);
        }

        const int n = juce::jmin(block, numSamples - pos);
        controller.processBlock(input.getReadPointer(0, pos),
                                input.getReadPointer(1, pos),
                                outL.data(), outR.data(), n);
        const auto blockState = controller.getDisplayState();
        std::fill(appliedShiftTrace.begin() + pos,
                  appliedShiftTrace.begin() + pos + n,
                  blockState.appliedShiftSemitones);
        const float absoluteTargetF0 =
            blockState.hasGuitarTarget
                ? 440.0f * std::pow(2.0f,
                                    (blockState.guitarTargetMidi
                                     + 12.0f * static_cast<float>(opt.octave) - 69.0f)
                                        / 12.0f)
                : 0.0f;
        std::fill(absoluteTargetF0Trace.begin() + pos,
                  absoluteTargetF0Trace.begin() + pos + n,
                  absoluteTargetF0);

        if (rms(input.getReadPointer(1, pos), n) > 0.005f)
        {
            ++activeBlocks;
            if (opt.bleedCancel && blockState.bleedCancelAdapting)
                cancelDbs.push_back(blockState.bleedCancelledDb);
            // 보정량 궤적을 블록 단위로 모은다. 시프트가 블록마다 크게 움직이면 WSOLA가
            // 매번 다시 이어붙여야 해서 비배음이 생긴다(합성 실험: ±0.5반음 변조 → 13~32%).
            if (blockState.hasVocalPitch)
                corrections.push_back(blockState.correctedMidi - blockState.vocalMidi);
            const float wet = blockState.wetness;
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

    if (opt.useWorld)
    {
        std::cout << "WORLD 분석·재합성 중...\n";
        const auto world = WorldVoiceTransformer::transform(
            input.getReadPointer(1), numSamples, sampleRate, appliedShiftTrace,
            absoluteTargetF0Trace, opt.boost, opt.volume, opt.gateEnabled, opt.gate);
        if (static_cast<int>(world.audio.size()) != numSamples)
        {
            std::cerr << "WORLD 재합성 실패\n";
            return 1;
        }
        output.copyFrom(0, 0, world.audio.data(), numSamples);
        clipped = 0;
        for (float sample : world.audio)
            if (std::abs(sample) >= mode2::params::outputLimitPeak * 0.9999f)
                ++clipped;
        std::cout << "WORLD 프레임 " << world.totalFrames
                  << "개 (유성 " << world.voicedFrames << "), FFT "
                  << world.fftSize << "\n";
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
              << "  유입상쇄=" << (opt.bleedCancel ? "on" : "off")
              << "  정렬=" << juce::String(opt.lookaheadMs, 1) << "ms"
              << "  시프터="
              << (opt.useWorld ? "world-offline"
                               : PitchShifterEngine::getBackendName(
                                     controller.getPitchShifterBackend())) << "\n"
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
        std::vector<float> sortedCorrections = corrections;
        std::sort(sortedCorrections.begin(), sortedCorrections.end());
        auto correctionPct = [&sortedCorrections](double p)
        {
            return sortedCorrections[static_cast<size_t>(
                p * static_cast<double>(sortedCorrections.size() - 1))];
        };

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
                  << "   실제 보정량 하위10% " << juce::String(correctionPct(0.1), 2)
                  << "  중앙값 " << juce::String(correctionPct(0.5), 2)
                  << "  상위10% " << juce::String(correctionPct(0.9), 2) << " 반음\n"
                  << "   블록간 변화 |Δ| 중앙값 " << juce::String(pct(0.5), 3)
                  << "  상위10% " << juce::String(pct(0.9), 2)
                  << "  최대 " << juce::String(deltas.back(), 2) << " 반음\n"
                  << "   0.25반음 초과 " << juce::String(100.0 * over[0] / deltas.size(), 1)
                  << "%   0.5반음 초과 " << juce::String(100.0 * over[1] / deltas.size(), 1)
                  << "%   1.0반음 초과 " << juce::String(100.0 * over[2] / deltas.size(), 1) << "%\n";
    }

    // 출력을 다시 F0 분석해 "의도한 목표"가 아니라 "실제로 나간 음정"을 잰다.
    //
    // 정렬 지연에 보컬 지연 버퍼(vocalControlLookahead)는 넣지 않는다. 그건 보컬 오디오를
    // 늦춰 피치 분석 시점과 맞추는 것이고, 목표는 지연되지 않은 기타 경로에서 오기 때문이다.
    // 출력 t의 음정을 정한 것은 시프터가 그 오디오를 소비하던 시점의 보정량이므로, 목표
    // 궤적과의 정렬 지연은 시프터 고유 지연뿐이다.
    //
    // 그마저도 상한에 가깝다. PitchShifterEngine::reset()이 Rubber Band의 start delay만큼을
    // 미리 버려 보상하므로 실제 지연은 이보다 짧게 나온다. WORLD 오프라인 경로는 입력과
    // 샘플 정렬된 결과를 만들므로 0이다.
    const int nominalLag = opt.useWorld ? 0 : controller.getPitchShifterLatencySamples();
    const int searchRadius = static_cast<int>(std::lround(0.120 * sampleRate));
    const auto accuracy = PitchAccuracyMetrics::measure(output.getReadPointer(0), numSamples,
                                                        sampleRate, absoluteTargetF0Trace,
                                                        nominalLag, searchRadius);

    if (accuracy.valid)
    {
        const double toMs = 1000.0 / sampleRate;
        std::cout << "출력 음정 정확도 (출력을 다시 F0 분석해 기타 목표와 비교):\n"
                  << "   표본 " << accuracy.comparedFrames << "프레임"
                  << " (유성 " << accuracy.voicedFrames << " / 전체 " << accuracy.totalFrames << ")"
                  // juce::String(const char*)는 UTF-8 리터럴을 ASCII로 오해해 한글을 깨뜨린다.
                  // 한글은 std::cout으로 직접 내보내고 숫자만 juce::String으로 만든다.
                  << "  정렬 지연 ";
        if (accuracy.lagIdentifiable)
            std::cout << "실측 " << juce::String(accuracy.bestLagSamples * toMs, 1) << "ms";
        else
            std::cout << "식별 불가(이론값 사용)";

        std::cout << " (이론 " << juce::String(accuracy.nominalLagSamples * toMs, 1) << "ms)\n"
                  << "   ±50 cents 이내 " << juce::String(accuracy.withinFiftyCentsPercent, 1)
                  << "%   ±20 cents 이내 " << juce::String(accuracy.withinTwentyCentsPercent, 1) << "%\n"
                  << "   |오차| 중앙값 " << juce::String(accuracy.medianAbsCents, 1)
                  << " cents  상위10% " << juce::String(accuracy.p90AbsCents, 1) << " cents\n"
                  << "   계통 오차(부호 있는 중앙값) " << juce::String(accuracy.medianSignedCents, 1)
                  << " cents   옥타브 오류(|오차|>600c) "
                  << juce::String(accuracy.octaveErrorPercent, 1) << "%\n";

        // 이론값은 상한이므로 실측이 그보다 짧은 것은 정상이다(start delay 보상). 반대로
        // 이론값을 넘어서면 어디선가 계산에 없는 지연이 붙었다는 뜻이라 확인이 필요하다.
        // 지연을 식별할 수 없는 녹음(같은 음을 오래 끄는 연주)에서는 판정하지 않는다.
        const double lagExcessMs = (accuracy.bestLagSamples - accuracy.nominalLagSamples) * toMs;
        if (accuracy.lagIdentifiable && lagExcessMs > 15.0)
            std::cout << "   경고: 실측 지연이 이론 상한보다 "
                      << juce::String(lagExcessMs, 1) << "ms 길다 — 계산에 없는 지연이 있다\n";
        else if (! accuracy.lagIdentifiable)
            std::cout << "   참고: 목표 음이 오래 유지돼 지연을 식별할 수 없다(동점 구간 "
                      << juce::String(accuracy.lagPlateauSamples * toMs, 0)
                      << "ms). 음이 자주 바뀌는 녹음이면 실측된다\n";
    }
    else
    {
        std::cout << "출력 음정 정확도: 비교 가능한 프레임이 부족해 측정하지 못했다"
                     " (출력이 무음이거나 기타 목표가 잡히지 않음)\n";
    }

    std::cout << "-> " << outFile.getFullPathName() << "\n";
    return 0;
}
