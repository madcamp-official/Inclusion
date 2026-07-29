#include <juce_core/juce_core.h>

#include "mode2_guitar_vocoder/CorrectionCalculator.h"
#include "mode2_guitar_vocoder/GuitarBleedCanceller.h"
#include "mode2_guitar_vocoder/GuitarTargetTracker.h"
#include "mode2_guitar_vocoder/PitchShifterEngine.h"
#include "core/dsp/PitchStabilizer.h"
#include "core/dsp/NoiseGate.h"
#include "params/Mode2Params.h"

#include <cmath>
#include <chrono>
#include <thread>
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

        beginTest(utf8("같은 기타 절대 목표는 보컬 옥타브와 무관하게 같은 출력이 된다"));
        {
            constexpr float absoluteGuitarTarget = 64.0f;
            for (float vocal : { 40.0f, 52.0f, 64.0f, 76.0f })
            {
                const float correction = CorrectionCalculator::computeCorrection(
                    vocal, absoluteGuitarTarget, 1.0f);
                expectWithinAbsoluteError(vocal + correction,
                                          absoluteGuitarTarget, 0.001f);
            }
        }

    }
};

static CorrectionCalculatorTests correctionCalculatorTests;

// 목소리 탐색 범위가 노래하는 음역을 덮는지 지킨다.
//
// 상한이 실제로 부르는 음보다 낮으면 진짜 피크가 탐색 밖이 되고 배주기 피크만 남아
// "한 옥타브 아래"로 확신 있게 오검출한다. 출력은 목표 - 추정 보컬로 시프트하므로
// 그만큼 출력이 한 옥타브 높게 나간다. 신뢰도가 1.00으로 나와서 게이트로도 못 막는다.
class VocalPitchRangeTests : public juce::UnitTest
{
public:
    VocalPitchRangeTests() : juce::UnitTest("VocalPitchRange") {}

    void runTest() override
    {
        // 실제로 쓰는 샘플레이트를 모두 본다. 창 크기는 샘플레이트에 비례해 정해지고
        // 탐색 경계는 lag(정수 샘플)로 계산되므로, 한 레이트에서 통과해도 다른
        // 레이트에서 상한이 어긋날 수 있다. 24k는 에어팟 HFP, 44.1k는 맥북 스피커가
        // 마스터인 통합 기기, 48k는 오디오 인터페이스 기준이다.
        for (double sampleRate : { 24000.0, 44100.0, 48000.0 })
            runAtSampleRate(sampleRate);
    }

    void runAtSampleRate(double sampleRate)
    {
        constexpr int blockSize = 512;

        PitchDetector detector;
        detector.prepare(sampleRate, mode2::params::vocalPitchMinFrequencyHz,
                         mode2::params::vocalPitchMaxFrequencyHz,
                         mode2::params::scalePitchWindowForSampleRate(
                             mode2::params::vocalPitchWindowSize, sampleRate));

        beginTest(utf8("E2~G5를 옥타브 오류 없이 검출한다 @ ")
                  + juce::String(sampleRate / 1000.0, 1) + "kHz");
        // E2(82Hz) ~ G5(784Hz). 양쪽 끝이 각각 다른 방향으로 깨진다:
        //   상한을 넘으면 진짜 피크가 범위 밖 → 배주기가 잡혀 -12반음 (출력은 한 옥타브 위)
        //   하한을 밑돌면 진짜 피크가 범위 밖 → 2배음이 잡혀 +12반음 (출력은 한 옥타브 아래)
        for (int midi : { 40, 43, 45, 52, 57, 64, 69, 72, 76, 79 })
        {
            detector.reset();
            const double f0 = 440.0 * std::pow(2.0, (midi - 69) / 12.0);

            PitchDetector::Result result;
            std::vector<float> block(blockSize, 0.0f);
            int sample = 0;
            for (int b = 0; b < 8; ++b)
            {
                for (int i = 0; i < blockSize; ++i, ++sample)
                {
                    const double t = sample / sampleRate;
                    // 배음이 있는 목소리에 가깝게. 순수 사인파는 실제보다 쉬운 신호다.
                    block[static_cast<size_t>(i)] = static_cast<float>(
                        0.3 * (std::sin(2.0 * juce::MathConstants<double>::pi * f0 * t)
                               + 0.6 * std::sin(4.0 * juce::MathConstants<double>::pi * f0 * t)
                               + 0.35 * std::sin(6.0 * juce::MathConstants<double>::pi * f0 * t)));
                }
                result = detector.processBlock(block.data(), blockSize);
            }

            expect(result.valid, utf8("검출 실패: MIDI ") + juce::String(midi));
            if (result.valid)
                expectWithinAbsoluteError(result.midiFloat, static_cast<float>(midi), 1.0f,
                                          utf8("MIDI ") + juce::String(midi) + utf8(" 오검출"));
        }
    }
};

static VocalPitchRangeTests vocalPitchRangeTests;

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

        beginTest(utf8("연주 중 짧은 온셋과 불안정한 다른 음은 목표를 바꾸지 않는다"));
        {
            PitchStabilizer touchFilter;
            touchFilter.prepare(44100.0);

            PitchDetector::Result steady;
            steady.valid = true;
            steady.midiFloat = 60.0f;
            steady.confidence = 0.9f;

            PitchStabilizer::Output touchOut {};
            for (int i = 0; i < 40; ++i)
                touchOut = touchFilter.processBlock(false, steady, blockSize);
            expect(touchOut.state == PitchStabilizer::State::Stable);

            const float incidentalNotes[] { 67.0f, 72.0f, 65.0f, 69.0f };
            for (int i = 0; i < 4; ++i)
            {
                PitchDetector::Result touch = steady;
                touch.midiFloat = incidentalNotes[i];
                touchOut = touchFilter.processBlock(i == 0, touch, blockSize);
                expect(touchOut.state == PitchStabilizer::State::Stable);
                expectWithinAbsoluteError(touchOut.targetMidi, 60.0f, 0.01f);
            }

            touchOut = touchFilter.processBlock(false, steady, blockSize);
            expectWithinAbsoluteError(touchOut.targetMidi, 60.0f, 0.01f);
        }

        beginTest(utf8("짧은 피치 검출 누락 뒤에는 직전 목표로 즉시 복귀한다"));
        {
            PitchStabilizer dropoutFilter;
            dropoutFilter.prepare(44100.0);

            PitchDetector::Result steady;
            steady.valid = true;
            steady.midiFloat = 60.0f;
            steady.confidence = 0.9f;

            PitchStabilizer::Output dropoutOut {};
            for (int i = 0; i < 40; ++i)
                dropoutOut = dropoutFilter.processBlock(false, steady, blockSize);

            PitchDetector::Result missing;
            dropoutOut = dropoutFilter.processBlock(false, missing, blockSize);
            expect(dropoutOut.state == PitchStabilizer::State::Coast);
            expect(dropoutOut.hasTarget);

            dropoutOut = dropoutFilter.processBlock(false, steady, blockSize);
            expect(dropoutOut.state == PitchStabilizer::State::Stable);
            expect(dropoutOut.hasTarget);
            expectWithinAbsoluteError(dropoutOut.targetMidi, 60.0f, 0.01f);
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

        beginTest(utf8("확인되지 않은 옥타브 검출은 오검출로 보고 직전 목표로 되접는다"));
        expectWithinAbsoluteError(GuitarTargetTracker::quantizeMidiWithHysteresis(69.08f, true, 57.0f),
                                  57.0f, 1.0e-6f);
        expectWithinAbsoluteError(GuitarTargetTracker::quantizeMidiWithHysteresis(32.95f, true, 57.0f),
                                  57.0f, 1.0e-6f);

        beginTest(utf8("확인된 옥타브 이동은 되접지 않고 그대로 따라간다"));
        expectWithinAbsoluteError(
            GuitarTargetTracker::quantizeMidiWithHysteresis(69.08f, true, 57.0f, true),
            69.0f, 1.0e-6f);
        expectWithinAbsoluteError(
            GuitarTargetTracker::quantizeMidiWithHysteresis(32.95f, true, 57.0f, true),
            33.0f, 1.0e-6f);
        // 확인 플래그는 옥타브 되접기만 건너뛴다. 반음 히스테리시스는 그대로 걸려야 한다.
        expectWithinAbsoluteError(
            GuitarTargetTracker::quantizeMidiWithHysteresis(57.31f, true, 57.0f, true),
            57.0f, 1.0e-6f);

        beginTest(utf8("되접기 대상 판별은 정확히 ±12·24반음일 때만이다"));
        expect(GuitarTargetTracker::isOctaveFoldCandidate(69.0f, 57.0f));
        expect(GuitarTargetTracker::isOctaveFoldCandidate(33.0f, 57.0f));
        expect(GuitarTargetTracker::isOctaveFoldCandidate(81.0f, 57.0f));
        expect(! GuitarTargetTracker::isOctaveFoldCandidate(68.0f, 57.0f));
        expect(! GuitarTargetTracker::isOctaveFoldCandidate(70.0f, 57.0f));
        expect(! GuitarTargetTracker::isOctaveFoldCandidate(62.0f, 57.0f));
    }
};

static GuitarNoteQuantizerTests guitarNoteQuantizerTests;

// 연주자가 "충분히 뮤트했다"고 느끼는 상태에서 목표가 잔향을 물지 않는지, 그리고 진짜
// 옥타브 이동은 따라가는지. 둘 다 검출기 단독으로는 안 보이고 트래커 전체를 돌려야 한다.
class GuitarTargetTrackerTests : public juce::UnitTest
{
public:
    GuitarTargetTrackerTests() : juce::UnitTest("GuitarTargetTracker") {}

    static double midiToHz(int midi)
    {
        return 440.0 * std::pow(2.0, (midi - 69) / 12.0);
    }

    // 배음이 있는 뜯은 줄 한 음을 blocks만큼 만들어 트래커에 흘린다.
    // decaySeconds가 짧으면 손 뮤트, 길면 자연 감쇠다.
    static GuitarTargetTracker::Output feed(GuitarTargetTracker& tracker, double sampleRate,
                                            int blockSize, int blocks, double f0, double amp,
                                            double decaySeconds, double& timeSeconds)
    {
        std::vector<float> block(static_cast<size_t>(blockSize), 0.0f);
        GuitarTargetTracker::Output out;
        for (int b = 0; b < blocks; ++b)
        {
            for (int i = 0; i < blockSize; ++i)
            {
                const double t = timeSeconds + i / sampleRate;
                const double env = amp * std::exp(-t / decaySeconds);
                double s = 0.0;
                for (int h = 1; h <= 6; ++h)
                    s += std::sin(2.0 * juce::MathConstants<double>::pi * f0 * h * t) / h;
                block[static_cast<size_t>(i)] = static_cast<float>(env * s);
            }
            timeSeconds += blockSize / sampleRate;
            out = tracker.processBlock(block.data(), blockSize);
        }
        return out;
    }

    void runTest() override
    {
        constexpr double sampleRate = 44100.0;
        constexpr int blockSize = 256;
        const int blocksPerSecond = static_cast<int>(sampleRate / blockSize);

        beginTest(utf8("자연 감쇠로 길게 울리는 음은 목표를 유지한다"));
        {
            GuitarTargetTracker tracker;
            tracker.prepare(sampleRate);
            double t = 0.0;
            const auto out = feed(tracker, sampleRate, blockSize, blocksPerSecond,
                                  midiToHz(45), 0.30, 1.8, t);
            expect(out.hasTarget, utf8("1초 뒤에도 목표가 있어야 한다"));
            if (out.hasTarget)
                expectWithinAbsoluteError(out.targetMidi, 45.0f, 0.5f);
        }

        beginTest(utf8("손으로 뮤트하면 잔향을 목표로 물지 않는다"));
        {
            GuitarTargetTracker tracker;
            tracker.prepare(sampleRate);
            double t = 0.0;
            // 0.5초 동안 A2를 정상적으로 울린 뒤,
            auto out = feed(tracker, sampleRate, blockSize, blocksPerSecond / 2,
                            midiToHz(45), 0.30, 1.8, t);
            expect(out.hasTarget, utf8("뮤트 전에는 목표가 있어야 한다"));

            // 같은 음을 60ms 시정수로 급감시킨다(손 뮤트). 0.4초면 피크의 -29dB 아래다.
            double mt = 0.0;
            std::vector<float> block(static_cast<size_t>(blockSize), 0.0f);
            const double f0 = midiToHz(45);
            const double startAmp = 0.30 * std::exp(-t / 1.8);
            for (int b = 0; b < blocksPerSecond * 2 / 5; ++b)
            {
                for (int i = 0; i < blockSize; ++i)
                {
                    const double tt = mt + i / sampleRate;
                    const double env = startAmp * std::exp(-tt / 0.06);
                    double s = 0.0;
                    for (int h = 1; h <= 6; ++h)
                        s += std::sin(2.0 * juce::MathConstants<double>::pi * f0 * (t + tt) * h) / h;
                    block[static_cast<size_t>(i)] = static_cast<float>(env * s);
                }
                mt += blockSize / sampleRate;
                out = tracker.processBlock(block.data(), blockSize);
            }
            // 뮤트가 충분히 진행되면 COAST를 거쳐 목표를 놓아야 한다. 여기서 목표를 계속
            // 들고 있으면 옆 줄 잔향이 그대로 다음 목표가 되는 경로가 열린다.
            expect(out.state == PitchStabilizer::State::Coast
                       || out.state == PitchStabilizer::State::Idle,
                   utf8("뮤트 뒤 COAST/IDLE이어야 하는데 실제: ")
                       + juce::String(static_cast<int>(out.state)));
        }

        beginTest(utf8("진짜 한 옥타브 위 음은 목표로 잡힌다 (A2 -> A3)"));
        {
            GuitarTargetTracker tracker;
            tracker.prepare(sampleRate);
            double t = 0.0;
            auto out = feed(tracker, sampleRate, blockSize, blocksPerSecond / 2,
                            midiToHz(45), 0.30, 1.8, t);
            expect(out.hasTarget && std::abs(out.targetMidi - 45.0f) < 0.5f,
                   utf8("먼저 A2를 잡아야 한다"));

            // 새 음을 새로 뜯는다(시간축을 0부터 다시 시작해 어택이 생기게).
            GuitarTargetTracker::Output after;
            double t2 = 0.0;
            for (int b = 0; b < blocksPerSecond; ++b)
                after = feed(tracker, sampleRate, blockSize, 1, midiToHz(57), 0.30, 1.8, t2);

            expect(after.hasTarget, utf8("A3에서 목표가 있어야 한다"));
            if (after.hasTarget)
                expectWithinAbsoluteError(after.targetMidi, 57.0f, 0.5f,
                                          utf8("A2에 붙잡혀 있으면 옥타브 가드 회귀다"));
        }
    }
};

static GuitarTargetTrackerTests guitarTargetTrackerTests;

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

class PitchShifterEngineTests : public juce::UnitTest
{
public:
    PitchShifterEngineTests() : juce::UnitTest("PitchShifterEngine") {}

    void runTest() override
    {
        for (const auto backend : { PitchShifterEngine::Backend::RubberBand,
                                    PitchShifterEngine::Backend::RubberBandLowLatency,
                                    PitchShifterEngine::Backend::SoundTouch })
        {
            if (! PitchShifterEngine::isBackendAvailable(backend))
                continue;

            beginTest(juce::String(PitchShifterEngine::getBackendName(backend))
                      + " streams finite, non-silent output");

            constexpr double sampleRate = 48000.0;
            constexpr int blockSize = 256;
            constexpr int blocks = 240;
            constexpr float inputHz = 220.0f;

            PitchShifterEngine shifter;
            shifter.setBackend(backend);
            shifter.setGlideRate(100000.0f);
            shifter.prepare(sampleRate, blockSize);

            std::vector<float> input(static_cast<size_t>(blockSize));
            std::vector<float> output(static_cast<size_t>(blockSize));
            double phase = 0.0;
            double sumSquares = 0.0;
            int measuredSamples = 0;
            int silentBlocksAfterWarmup = 0;

            for (int block = 0; block < blocks; ++block)
            {
                for (int i = 0; i < blockSize; ++i)
                {
                    input[static_cast<size_t>(i)] = 0.2f * std::sin(static_cast<float>(phase));
                    phase += 2.0 * juce::MathConstants<double>::pi * inputHz / sampleRate;
                }

                shifter.processBlock(input.data(), output.data(), blockSize, 7.0f);

                float peak = 0.0f;
                for (float sample : output)
                {
                    expect(std::isfinite(sample), "backend emitted NaN/Inf");
                    peak = std::max(peak, std::abs(sample));
                }

                // R3/short의 정상 시작 지연보다 넉넉히 기다린 뒤에는 push 스트림이 매
                // 콜백마다 끊김 없이 데이터를 내야 한다.
                if (block > 24)
                {
                    if (peak < 1.0e-6f)
                        ++silentBlocksAfterWarmup;
                    for (float sample : output)
                    {
                        sumSquares += static_cast<double>(sample) * sample;
                        ++measuredSamples;
                    }
                }
            }

            const float outputRms = measuredSamples > 0
                ? static_cast<float>(std::sqrt(sumSquares / measuredSamples))
                : 0.0f;
            expectGreaterThan(outputRms, 0.01f, "steady-state output should be audible");
            expectEquals(silentBlocksAfterWarmup, 0, "steady-state callback dropout");
        }

        if (PitchShifterEngine::isBackendAvailable(PitchShifterEngine::Backend::RubberBand))
        {
            beginTest("rubberband R2 low-latency mode reports less delay than R3");

            PitchShifterEngine r3;
            r3.setBackend(PitchShifterEngine::Backend::RubberBand);
            r3.prepare(48000.0, 256);

            PitchShifterEngine r2;
            r2.setBackend(PitchShifterEngine::Backend::RubberBandLowLatency);
            r2.prepare(48000.0, 256);

            expectGreaterThan(r3.getLatencySamples(), r2.getLatencySamples());

            beginTest("rubberband R2 dynamic shifts do not expose zero-filled queue underruns");

            r2.setGlideRate(100000.0f);
            r2.prepare(48000.0, 256);

            std::vector<float> input(256);
            std::vector<float> output(256);
            double phase = 0.0;
            int consecutiveExactZeros = 0;
            int longestExactZeroRun = 0;
            for (int block = 0; block < 300; ++block)
            {
                for (int i = 0; i < 256; ++i)
                {
                    input[static_cast<size_t>(i)] =
                        0.2f * std::sin(static_cast<float>(phase));
                    phase += 2.0 * juce::MathConstants<double>::pi * 220.0 / 48000.0;
                }

                const float shift = (block / 7) % 2 == 0 ? -12.0f : 7.0f;
                r2.processBlock(input.data(), output.data(), 256, shift);

                if (block < 30)
                    continue;

                for (const float sample : output)
                {
                    if (sample == 0.0f)
                    {
                        ++consecutiveExactZeros;
                        longestExactZeroRun =
                            std::max(longestExactZeroRun, consecutiveExactZeros);
                    }
                    else
                    {
                        consecutiveExactZeros = 0;
                    }
                }
            }

            expectLessOrEqual(longestExactZeroRun, 2,
                              "queue underrun must not be exposed as a zero run");
        }

        if (PitchShifterEngine::isBackendAvailable(PitchShifterEngine::Backend::World))
        {
            beginTest("world worker keeps up with a real-time callback");

            constexpr double sampleRate = 48000.0;
            constexpr int blockSize = 256;
            constexpr int blocks = 180;
            PitchShifterEngine shifter;
            shifter.setBackend(PitchShifterEngine::Backend::World);
            shifter.prepare(sampleRate, blockSize);

            std::vector<float> input(static_cast<size_t>(blockSize));
            std::vector<float> output(static_cast<size_t>(blockSize));
            double phase = 0.0;
            double sumSquares = 0.0;
            int measuredSamples = 0;
            int silentBlocks = 0;

            for (int block = 0; block < blocks; ++block)
            {
                for (int i = 0; i < blockSize; ++i)
                {
                    // 단일 사인보다 실제 유성음에 가까운 고조파 입력을 쓴다.
                    input[static_cast<size_t>(i)] =
                        0.12f * std::sin(static_cast<float>(phase))
                      + 0.06f * std::sin(static_cast<float>(phase * 2.0))
                      + 0.03f * std::sin(static_cast<float>(phase * 3.0));
                    phase += 2.0 * juce::MathConstants<double>::pi * 180.0 / sampleRate;
                }

                shifter.processBlock(input.data(), output.data(), blockSize, 0.0f, 240.0f);
                if (block > 45)
                {
                    float peak = 0.0f;
                    for (float sample : output)
                    {
                        expect(std::isfinite(sample), "WORLD emitted NaN/Inf");
                        peak = std::max(peak, std::abs(sample));
                        sumSquares += static_cast<double>(sample) * sample;
                        ++measuredSamples;
                    }
                    if (peak < 1.0e-6f)
                        ++silentBlocks;
                }

                std::this_thread::sleep_for(std::chrono::microseconds(5333));
            }

            const float outputRms = measuredSamples > 0
                ? static_cast<float>(std::sqrt(sumSquares / measuredSamples))
                : 0.0f;
            expectGreaterThan(outputRms, 0.005f, "WORLD steady-state output should be audible");
            expectLessOrEqual(silentBlocks, 2, "WORLD worker fell behind real time");
        }
    }
};

static PitchShifterEngineTests pitchShifterEngineTests;

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
