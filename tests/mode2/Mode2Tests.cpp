#include <juce_core/juce_core.h>

#include "mode2_guitar_vocoder/CorrectionCalculator.h"
#include "mode2_guitar_vocoder/GuitarBleedCanceller.h"
#include "mode2_guitar_vocoder/GuitarTargetTracker.h"
#include "core/dsp/PitchStabilizer.h"
#include "core/dsp/NoiseGate.h"
#include "params/Mode2Params.h"

#include <cmath>
#include <vector>

namespace
{
    // 8비트 리터럴을 ASCII로 오해하는 juce::String(const char*) 대신 UTF-8로 명시 변환한다.
    juce::String utf8(const char* text)
    {
        return juce::String(juce::CharPointer_UTF8(text));
    }
}

class CorrectionCalculatorTests : public juce::UnitTest
{
public:
    CorrectionCalculatorTests() : juce::UnitTest("CorrectionCalculator") {}

    void runTest() override
    {
        beginTest(utf8("target 없으면 0 반환"));
        expectEquals(CorrectionCalculator::computeCorrection(60.0f, std::nullopt, 1.0f), 0.0f);

        beginTest(utf8("vocal 없으면 0 반환"));
        expectEquals(CorrectionCalculator::computeCorrection(std::nullopt, 60.0f, 1.0f), 0.0f);

        beginTest(utf8("정상 범위: strength * diff"));
        const float result = CorrectionCalculator::computeCorrection(58.0f, 60.0f, 0.5f);
        expectWithinAbsoluteError(result, 1.0f, 0.001f);

        beginTest(utf8("보정은 접지 않고 목표와의 차이를 그대로 낸다 (정렬은 호출자 책임)"));
        expectWithinAbsoluteError(CorrectionCalculator::computeCorrection(54.0f, 60.0f, 1.0f), 6.0f, 0.001f);
        expectWithinAbsoluteError(CorrectionCalculator::computeCorrection(48.0f, 60.0f, 1.0f), 12.0f, 0.001f);

        beginTest(utf8("피치 클래스 고정: 보컬 옥타브 오검출은 보정량에 영향을 주지 않는다"));
        {
            const float fromC3 = CorrectionCalculator::computePitchClassLockedCorrection(
                48.0f, 64.0f, std::nullopt, 0, 1.0f);
            const float fromC4 = CorrectionCalculator::computePitchClassLockedCorrection(
                60.0f, 64.0f, std::nullopt, 0, 1.0f);
            const float fromC5 = CorrectionCalculator::computePitchClassLockedCorrection(
                72.0f, 64.0f, std::nullopt, 0, 1.0f);
            expectWithinAbsoluteError(fromC3, 4.0f, 0.001f);
            expectWithinAbsoluteError(fromC4, fromC3, 0.001f);
            expectWithinAbsoluteError(fromC5, fromC3, 0.001f);
        }

        beginTest(utf8("피치 클래스 고정: 연속 상승하는 목소리에 반대 보정을 이어 출력음을 유지한다"));
        {
            float correction = 0.0f;
            bool haveCorrection = false;
            for (int vocal = 60; vocal <= 72; ++vocal)
            {
                correction = CorrectionCalculator::computePitchClassLockedCorrection(
                    static_cast<float>(vocal), 60.0f,
                    haveCorrection ? std::optional<float>(correction) : std::nullopt,
                    0, 1.0f);
                haveCorrection = true;
                expectWithinAbsoluteError(static_cast<float>(vocal) + correction, 60.0f, 0.001f);
            }
        }

        beginTest(utf8("피치 클래스 고정: 옥타브 설정은 같은 음이름의 다른 분기를 고른다"));
        {
            const float normal = CorrectionCalculator::computePitchClassLockedCorrection(
                60.0f, 64.0f, std::nullopt, 0, 1.0f);
            const float lower = CorrectionCalculator::computePitchClassLockedCorrection(
                60.0f, 64.0f, std::nullopt, -1, 1.0f);
            expectWithinAbsoluteError(normal, 4.0f, 0.001f);
            expectWithinAbsoluteError(lower, -8.0f, 0.001f);
        }

        beginTest(utf8("옥타브 정렬: 목표를 목소리 근처로 들여온다"));
        {
            // 목표 E1(28), 목소리 E3(52) → 2옥타브 올려야 목소리 옆에 온다.
            const int octaves = CorrectionCalculator::chooseTargetOctaves(28.0f, 52.0f, 0);
            expectEquals(octaves, 2);
            const float aligned = 28.0f + 12.0f * static_cast<float>(octaves);
            expect(std::abs(aligned - 52.0f) <= mode2::params::maxUsableShiftSemitones + 0.001f);
        }

        beginTest(utf8("옥타브 정렬: 상한 이내면 이미 고른 옥타브를 유지한다"));
        // 목표와 목소리가 6반음 차이(상한 안)면 0을 유지해 절대 음높이 추종이 살아난다.
        expectEquals(CorrectionCalculator::chooseTargetOctaves(60.0f, 54.0f, 0), 0);

        beginTest(utf8("옥타브 정렬: 접기 경계에서 히스테리시스로 뒤집히지 않는다"));
        {
            // 목표 28.46, 목소리가 46↔47을 오가는 실측 사례. 히스테리시스가 없으면
            // 최근접 옥타브가 1↔2로 바뀌며 보정이 12반음 뒤집혔다.
            const float target = 28.46f;
            int octaves = CorrectionCalculator::chooseTargetOctaves(target, 46.0f, 0);
            const int settled = octaves;
            for (float vocal : { 47.0f, 46.0f, 47.5f, 45.5f, 47.0f })
                octaves = CorrectionCalculator::chooseTargetOctaves(target, vocal, octaves);
            expectEquals(octaves, settled);
        }

        beginTest(utf8("옥타브 정렬 후 시프트는 항상 사용 가능 범위 + 히스테리시스 안이다"));
        {
            const float limit = mode2::params::maxUsableShiftSemitones
                                + mode2::params::octaveChoiceHysteresisSemitones;
            for (int semitones = -36; semitones <= 36; ++semitones)
            {
                const float vocal = 60.0f;
                const float target = vocal + static_cast<float>(semitones);
                const int octaves = CorrectionCalculator::chooseTargetOctaves(target, vocal, 0);
                const float aligned = target + 12.0f * static_cast<float>(octaves);
                const float correction = CorrectionCalculator::computeCorrection(vocal, aligned, 1.0f);
                expect(std::abs(correction) <= limit + 0.001f,
                       "semitones=" + juce::String(semitones) + " correction=" + juce::String(correction));

                // 정렬은 옥타브 단위로만 움직이므로 음이름은 목표와 같아야 한다.
                const float octaveError = aligned - target;
                expectWithinAbsoluteError(octaveError - 12.0f * std::round(octaveError / 12.0f), 0.0f, 0.001f);
            }
        }
    }
};

static CorrectionCalculatorTests correctionCalculatorTests;

class PitchStabilizerTests : public juce::UnitTest
{
public:
    PitchStabilizerTests() : juce::UnitTest("PitchStabilizer") {}

    void runTest() override
    {
        PitchStabilizer stabilizer;
        stabilizer.prepare(44100.0);
        const int blockSize = 256;

        beginTest("IDLE -> ATTACK -> COLLECT -> STABLE");

        PitchDetector::Result noPitch;
        auto out = stabilizer.processBlock(false, noPitch, blockSize);
        expect(out.state == PitchStabilizer::State::Idle);

        PitchDetector::Result pitch;
        pitch.valid = true;
        pitch.midiFloat = 60.0f;
        pitch.confidence = 0.9f;

        out = stabilizer.processBlock(true, pitch, blockSize);
        expect(out.state == PitchStabilizer::State::Attack);

        for (int i = 0; i < 20 && out.state == PitchStabilizer::State::Attack; ++i)
            out = stabilizer.processBlock(false, pitch, blockSize);
        expect(out.state != PitchStabilizer::State::Attack);

        for (int i = 0; i < 20 && out.state == PitchStabilizer::State::Collect; ++i)
            out = stabilizer.processBlock(false, pitch, blockSize);

        expect(out.state == PitchStabilizer::State::Stable);
        expect(out.hasTarget);
        expectWithinAbsoluteError(out.targetMidi, 60.0f, 0.01f);

        beginTest(utf8("온셋 없이 음이 바뀌면 목표를 다시 잡는다 (레가토·코드 전환)"));
        {
            // 위에서 이미 60.0에 STABLE. 온셋 없이 64.0으로 바뀐 피치만 계속 넣는다.
            PitchDetector::Result moved;
            moved.valid = true;
            moved.midiFloat = 64.0f;
            moved.confidence = 0.9f;

            PitchStabilizer::Output moveOut {};
            for (int i = 0; i < 40; ++i)
                moveOut = stabilizer.processBlock(false, moved, blockSize);

            expect(moveOut.state == PitchStabilizer::State::Stable);
            expectWithinAbsoluteError(moveOut.targetMidi, 64.0f, 0.01f);
            // 재조준 중에도 목표가 유지되어 소리가 끊기지 않아야 한다.
            expect(moveOut.hasTarget);
            expectWithinAbsoluteError(moveOut.fadeGain, 1.0f, 0.001f);
        }

        beginTest(utf8("IDLE에서 온셋 없이 유효한 피치만으로 목표를 잡는다"));
        {
            PitchStabilizer fresh;
            fresh.prepare(44100.0);

            PitchDetector::Result steady;
            steady.valid = true;
            steady.midiFloat = 55.0f;
            steady.confidence = 0.9f;

            PitchStabilizer::Output freshOut {};
            for (int i = 0; i < 40; ++i)
                freshOut = fresh.processBlock(false, steady, blockSize);

            expect(freshOut.hasTarget);
            expectWithinAbsoluteError(freshOut.targetMidi, 55.0f, 0.01f);
        }

        beginTest(utf8("STABLE -> COAST -> HOLD/FADE -> IDLE (신호 소실)"));

        PitchDetector::Result invalidPitch;
        bool sawCoast = false;
        int blocksProcessed = 0;
        while (out.state != PitchStabilizer::State::Idle && blocksProcessed < 200)
        {
            out = stabilizer.processBlock(false, invalidPitch, blockSize);
            if (out.state == PitchStabilizer::State::Coast)
                sawCoast = true;
            ++blocksProcessed;
        }

        expect(sawCoast);
        expect(out.state == PitchStabilizer::State::Idle);
        expect(!out.hasTarget);
    }
};

static PitchStabilizerTests pitchStabilizerTests;

class GuitarNoteQuantizerTests : public juce::UnitTest
{
public:
    GuitarNoteQuantizerTests() : juce::UnitTest("GuitarNoteQuantizer") {}

    void runTest() override
    {
        beginTest(utf8("첫 검출은 최근접 반음으로 양자화한다"));
        expectWithinAbsoluteError(GuitarTargetTracker::quantizeMidiWithHysteresis(57.31f, false, 0.0f),
                                  57.0f, 1.0e-6f);
        expectWithinAbsoluteError(GuitarTargetTracker::quantizeMidiWithHysteresis(57.72f, false, 0.0f),
                                  58.0f, 1.0e-6f);

        beginTest(utf8("반음 경계의 작은 흔들림에는 직전 음표를 유지한다"));
        expectWithinAbsoluteError(GuitarTargetTracker::quantizeMidiWithHysteresis(57.58f, true, 57.0f),
                                  57.0f, 1.0e-6f);
        expectWithinAbsoluteError(GuitarTargetTracker::quantizeMidiWithHysteresis(56.42f, true, 57.0f),
                                  57.0f, 1.0e-6f);

        beginTest(utf8("히스테리시스를 충분히 넘은 새 음은 즉시 바꾼다"));
        expectWithinAbsoluteError(GuitarTargetTracker::quantizeMidiWithHysteresis(57.71f, true, 57.0f),
                                  58.0f, 1.0e-6f);
        expectWithinAbsoluteError(GuitarTargetTracker::quantizeMidiWithHysteresis(55.9f, true, 57.0f),
                                  56.0f, 1.0e-6f);

        beginTest(utf8("같은 음의 옥타브 오검출은 직전 목표 근처로 되접는다"));
        expectWithinAbsoluteError(GuitarTargetTracker::quantizeMidiWithHysteresis(69.08f, true, 57.0f),
                                  57.0f, 1.0e-6f);
        expectWithinAbsoluteError(GuitarTargetTracker::quantizeMidiWithHysteresis(32.95f, true, 57.0f),
                                  57.0f, 1.0e-6f);
    }
};

static GuitarNoteQuantizerTests guitarNoteQuantizerTests;

class NoiseGateTests : public juce::UnitTest
{
public:
    NoiseGateTests() : juce::UnitTest("NoiseGate") {}

    void runTest() override
    {
        const int blockSize = 512;
        const double sampleRate = 44100.0;
        const float threshold = 0.02f;

        auto makeGate = [&]
        {
            auto gate = std::make_unique<NoiseGate>();
            gate->prepare(sampleRate, blockSize);
            gate->setThreshold(threshold);
            return gate;
        };

        // 게이트 게인은 시간 상수를 따라 움직이므로, 여러 블록 돌려 수렴시킨 뒤 확인한다.
        auto runBlocks = [&](NoiseGate& gate, float keyRms, int numBlocks)
        {
            std::vector<float> buffer;
            for (int b = 0; b < numBlocks; ++b)
            {
                buffer.assign(static_cast<size_t>(blockSize), 1.0f);
                gate.processBlock(buffer.data(), blockSize, keyRms);
            }
            return buffer.back();
        };

        beginTest(utf8("임계 이상이면 열리고 게인이 1에 수렴"));
        {
            auto gate = makeGate();
            const float last = runBlocks(*gate, threshold * 2.0f, 20);
            expect(gate->isOpen());
            expectWithinAbsoluteError(last, 1.0f, 0.02f);
        }

        beginTest(utf8("무음이 이어지면 닫히고 게인이 0으로 감쇠"));
        {
            auto gate = makeGate();
            runBlocks(*gate, threshold * 2.0f, 20);
            const float last = runBlocks(*gate, 0.0f, 200);
            expect(! gate->isOpen());
            expectWithinAbsoluteError(last, 0.0f, 0.02f);
        }

        beginTest(utf8("히스테리시스: 여는 임계와 닫는 임계 사이에서는 열린 채 유지"));
        {
            auto gate = makeGate();
            runBlocks(*gate, threshold * 2.0f, 20);
            expect(gate->isOpen());

            // 여는 임계 아래지만 닫는 임계(threshold * hysteresisRatio)보다는 위인 레벨.
            const float between = threshold
                                  * (1.0f + mode2::params::noiseGateHysteresisRatio) * 0.5f;
            runBlocks(*gate, between, 200);
            expect(gate->isOpen(), utf8("히스테리시스 구간에서 닫히면 안 된다"));
        }

        beginTest(utf8("비활성화하면 항상 통과"));
        {
            auto gate = makeGate();
            gate->setEnabled(false);
            const float last = runBlocks(*gate, 0.0f, 5);
            expectWithinAbsoluteError(last, 1.0f, 0.0001f);
        }
    }
};

static NoiseGateTests noiseGateTests;

class GuitarBleedCancellerTests : public juce::UnitTest
{
public:
    GuitarBleedCancellerTests() : juce::UnitTest("GuitarBleedCanceller") {}

    void runTest() override
    {
        const int blockSize = 512;
        const double sampleRate = 44100.0;

        // 결정적인 의사난수. 백색에 가까운 참조는 NLMS가 가장 빨리 수렴하는 조건이라
        // "메커니즘이 동작하는가"를 흔들림 없이 확인할 수 있다.
        auto noise = [](int seed)
        {
            juce::Random rng(seed);
            return [rng]() mutable { return rng.nextFloat() * 2.0f - 1.0f; };
        };

        auto rms = [](const std::vector<float>& v)
        {
            double s = 0.0;
            for (float x : v) s += static_cast<double>(x) * x;
            return v.empty() ? 0.0f : static_cast<float>(std::sqrt(s / v.size()));
        };

        beginTest(utf8("참조(기타)가 무음이면 목소리를 그대로 통과시킨다"));
        {
            GuitarBleedCanceller canceller;
            canceller.prepare(sampleRate);
            canceller.setEnabled(true);   // 기본값이 꺼짐이므로 동작 검증에는 명시적으로 켠다.

            auto gen = noise(1);
            std::vector<float> vocal(static_cast<size_t>(blockSize));
            std::vector<float> ref(static_cast<size_t>(blockSize), 0.0f);
            std::vector<float> out(static_cast<size_t>(blockSize));

            float maxDiff = 0.0f;
            for (int b = 0; b < 20; ++b)
            {
                for (auto& v : vocal) v = gen() * 0.1f;
                canceller.processBlock(vocal.data(), ref.data(), out.data(), blockSize);
                for (int i = 0; i < blockSize; ++i)
                    maxDiff = std::max(maxDiff, std::abs(out[static_cast<size_t>(i)] - vocal[static_cast<size_t>(i)]));
            }
            expectLessThan(maxDiff, 1.0e-6f, utf8("기타가 없으면 아무것도 빼지 않아야 한다"));
            expect(! canceller.isAdapting());
        }

        beginTest(utf8("비활성화하면 통과"));
        {
            GuitarBleedCanceller canceller;
            canceller.prepare(sampleRate);
            canceller.setEnabled(false);

            auto gen = noise(2);
            std::vector<float> vocal(static_cast<size_t>(blockSize));
            std::vector<float> ref(static_cast<size_t>(blockSize));
            std::vector<float> out(static_cast<size_t>(blockSize));
            for (auto& v : vocal) v = gen() * 0.1f;
            for (auto& r : ref) r = gen() * 0.3f;

            canceller.processBlock(vocal.data(), ref.data(), out.data(), blockSize);
            for (int i = 0; i < blockSize; ++i)
                expectWithinAbsoluteError(out[static_cast<size_t>(i)], vocal[static_cast<size_t>(i)], 1.0e-9f);
        }

        beginTest(utf8("지연·감쇠된 기타 유입을 학습해서 지운다"));
        {
            GuitarBleedCanceller canceller;
            canceller.prepare(sampleRate);
            canceller.setEnabled(true);

            // 유입 경로: 40샘플(≈0.9ms) 지연 + 0.5배, 그리고 0.25배의 두 번째 반사.
            const int delay1 = 40, delay2 = 130;
            const float gain1 = 0.5f, gain2 = 0.25f;
            expect(delay2 < mode2::params::bleedCancelFilterTaps,
                   utf8("반사가 필터 길이 안에 있어야 학습 가능하다"));

            auto gen = noise(3);
            std::vector<float> refHistory;
            std::vector<float> ref(static_cast<size_t>(blockSize));
            std::vector<float> vocal(static_cast<size_t>(blockSize));
            std::vector<float> out(static_cast<size_t>(blockSize));

            // 목소리는 넣지 않는다. 이 테스트는 "경로를 추정할 수 있는가"만 본다.
            std::vector<float> lastIn, lastOut;
            const int totalBlocks = 300;
            for (int b = 0; b < totalBlocks; ++b)
            {
                for (int i = 0; i < blockSize; ++i)
                {
                    const float x = gen() * 0.3f;
                    ref[static_cast<size_t>(i)] = x;
                    refHistory.push_back(x);

                    const int n = static_cast<int>(refHistory.size()) - 1;
                    float bleed = 0.0f;
                    if (n - delay1 >= 0) bleed += gain1 * refHistory[static_cast<size_t>(n - delay1)];
                    if (n - delay2 >= 0) bleed += gain2 * refHistory[static_cast<size_t>(n - delay2)];
                    vocal[static_cast<size_t>(i)] = bleed;
                }

                canceller.processBlock(vocal.data(), ref.data(), out.data(), blockSize);

                if (b >= totalBlocks - 5)   // 수렴 후 마지막 몇 블록만 평가
                {
                    lastIn.insert(lastIn.end(), vocal.begin(), vocal.end());
                    lastOut.insert(lastOut.end(), out.begin(), out.end());
                }
            }

            expect(canceller.isAdapting());
            const float before = rms(lastIn);
            const float after = rms(lastOut);
            expectGreaterThan(before, 0.01f, utf8("테스트 신호 자체가 유효해야 한다"));
            // 20dB(1/10) 이상 줄어들면 경로를 제대로 추정한 것으로 본다.
            expectLessThan(after, before * 0.1f,
                           utf8("유입이 20dB 이상 줄어야 한다. 실제 ")
                               + juce::String(20.0f * std::log10(before / std::max(after, 1.0e-9f)), 1) + "dB");
        }

        beginTest(utf8("유입이 없으면 목소리를 거의 깎지 않는다"));
        {
            GuitarBleedCanceller canceller;
            canceller.prepare(sampleRate);
            canceller.setEnabled(true);

            // 기타는 울리지만 마이크에는 전혀 안 들어오는 상황. 상쇄기는 뺄 것이 없으므로
            // 목소리를 그대로 두어야 한다(가변 스텝이 없으면 여기서 목소리를 깎는다).
            auto refGen = noise(4);
            auto vocalGen = noise(5);
            std::vector<float> ref(static_cast<size_t>(blockSize));
            std::vector<float> vocal(static_cast<size_t>(blockSize));
            std::vector<float> out(static_cast<size_t>(blockSize));

            std::vector<float> lastIn, lastOut;
            const int totalBlocks = 300;
            for (int b = 0; b < totalBlocks; ++b)
            {
                for (int i = 0; i < blockSize; ++i)
                {
                    ref[static_cast<size_t>(i)] = refGen() * 0.3f;
                    vocal[static_cast<size_t>(i)] = vocalGen() * 0.1f;
                }
                canceller.processBlock(vocal.data(), ref.data(), out.data(), blockSize);
                if (b >= totalBlocks - 5)
                {
                    lastIn.insert(lastIn.end(), vocal.begin(), vocal.end());
                    lastOut.insert(lastOut.end(), out.begin(), out.end());
                }
            }

            const float before = rms(lastIn);
            const float after = rms(lastOut);
            // 1dB(약 0.89배) 이내면 실질적으로 손대지 않은 것으로 본다.
            expectGreaterThan(after, before * 0.89f,
                              utf8("뺄 것이 없는데 목소리를 깎으면 안 된다. 실제 ")
                                  + juce::String(20.0f * std::log10(before / std::max(after, 1.0e-9f)), 2) + "dB " + utf8("감쇠"));
        }
    }
};

static GuitarBleedCancellerTests guitarBleedCancellerTests;

int main()
{
    juce::UnitTestRunner runner;
    runner.runAllTests();

    for (int i = 0; i < runner.getNumResults(); ++i)
    {
        const auto* result = runner.getResult(i);
        if (result != nullptr && result->failures > 0)
            return 1;
    }

    return 0;
}
