#include <juce_core/juce_core.h>

#include "mode2_guitar_vocoder/CorrectionCalculator.h"
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

        beginTest(utf8("정확히 한 옥타브 차이는 보정하지 않는다 (같은 음이름)"));
        // C3로 부르면서 기타는 C4 → 이미 같은 음이름이므로 시프트 없음.
        const float octaveApart = CorrectionCalculator::computeCorrection(48.0f, 60.0f, 1.0f);
        expectWithinAbsoluteError(octaveApart, 0.0f, 0.001f);

        beginTest(utf8("옥타브를 넘는 차이는 가까운 옥타브로 접어서 보정"));
        // D3(50)로 부르면서 기타는 C4(60): 접으면 -2반음 → C3(48)로 붙는다.
        const float folded = CorrectionCalculator::computeCorrection(50.0f, 60.0f, 1.0f);
        expectWithinAbsoluteError(folded, -2.0f, 0.001f);

        beginTest(utf8("접은 뒤 잔차는 항상 ±6반음 이내"));
        for (int semitones = -36; semitones <= 36; ++semitones)
        {
            const float vocal = 60.0f;
            const float target = vocal + static_cast<float>(semitones);
            const float correction = CorrectionCalculator::computeCorrection(vocal, target, 1.0f);
            expect(std::abs(correction) <= 6.0f + 0.001f,
                   "semitones=" + juce::String(semitones) + " correction=" + juce::String(correction));
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
