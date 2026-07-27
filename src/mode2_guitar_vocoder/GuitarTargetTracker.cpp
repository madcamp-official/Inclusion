#include "GuitarTargetTracker.h"
#include "params/Mode2Params.h"

#include <cmath>

namespace
{
    float computeRms(const float* samples, int numSamples)
    {
        if (numSamples <= 0)
            return 0.0f;

        double sum = 0.0;
        for (int i = 0; i < numSamples; ++i)
            sum += static_cast<double>(samples[i]) * samples[i];
        return static_cast<float>(std::sqrt(sum / numSamples));
    }
}

void GuitarTargetTracker::prepare(double sampleRateIn)
{
    onsetDetector.prepare(sampleRateIn);
    pitchDetector.prepare(sampleRateIn, mode2::params::guitarPitchMinFrequencyHz,
                          mode2::params::guitarPitchMaxFrequencyHz,
                          mode2::params::scalePitchWindowForSampleRate(
                              mode2::params::guitarPitchWindowSize, sampleRateIn));
    pitchStabilizer.prepare(sampleRateIn);
}

void GuitarTargetTracker::reset()
{
    onsetDetector.reset();
    pitchDetector.reset();
    pitchStabilizer.reset();
    haveQuantizedNote = false;
    quantizedNoteMidi = 0.0f;
}

float GuitarTargetTracker::quantizeMidiWithHysteresis(float midi, bool havePreviousNote,
                                                      float previousNote)
{
    if (havePreviousNote)
    {
        // 히스테리시스 비교를 할 때만 직전 음표와 가장 가까운 옥타브로 본다. 이 값을 다음
        // 상태의 기준으로 저장하면 잡음의 피치 클래스가 조금씩 바뀔 때 옥타브가 무한히
        // 누적 이동할 수 있으므로, 실제 새 음표는 아래의 원본 midi에서 다시 만든다.
        const float nearbyMidi = midi + 12.0f * std::round((previousNote - midi) / 12.0f);
        if (std::abs(nearbyMidi - previousNote)
            < mode2::params::guitarNoteHysteresisSemitones)
            return previousNote;

        const float rawQuantized = std::round(midi);
        const float pitchClassDelta =
            std::remainder(rawQuantized - previousNote, 12.0f);
        if (std::abs(pitchClassDelta) < 0.5f
            && std::abs(rawQuantized - previousNote) >= 11.5f)
            return previousNote;
    }

    return std::round(midi);
}

GuitarTargetTracker::Output GuitarTargetTracker::processBlock(const float* guitarSamples, int numSamples)
{
    const bool onsetDetected = onsetDetector.processBlock(guitarSamples, numSamples);
    PitchDetector::Result pitch = pitchDetector.processBlock(guitarSamples, numSamples);
    if (computeRms(guitarSamples, numSamples) < mode2::params::guitarPitchRmsFloor)
        pitch.valid = false;
    if (pitch.valid)
    {
        quantizedNoteMidi = quantizeMidiWithHysteresis(pitch.midiFloat, haveQuantizedNote,
                                                       quantizedNoteMidi);
        haveQuantizedNote = true;
        pitch.midiFloat = quantizedNoteMidi;
    }
    const PitchStabilizer::Output stabilized =
        pitchStabilizer.processBlock(onsetDetected, pitch, numSamples);

    Output out;
    out.hasTarget = stabilized.hasTarget;
    out.targetMidi = stabilized.targetMidi;
    out.fadeGain = stabilized.fadeGain;
    out.state = stabilized.state;
    return out;
}
