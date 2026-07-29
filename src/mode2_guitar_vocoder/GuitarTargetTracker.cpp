#include "GuitarTargetTracker.h"
#include "params/Mode2Params.h"

#include <algorithm>
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
    sampleRate = sampleRateIn;
    onsetDetector.prepare(sampleRateIn);
    pitchDetector.prepare(sampleRateIn, mode2::params::guitarPitchMinFrequencyHz,
                          mode2::params::guitarPitchMaxFrequencyHz,
                          mode2::params::scalePitchWindowForSampleRate(
                              mode2::params::guitarPitchWindowSize, sampleRateIn));
    pitchStabilizer.prepare(sampleRateIn);
    octaveConfirmSamples = std::max(
        1LL, static_cast<long long>(mode2::params::octaveChangeConfirmSeconds * sampleRate));
    reset();
}

void GuitarTargetTracker::reset()
{
    onsetDetector.reset();
    pitchDetector.reset();
    pitchStabilizer.reset();
    haveQuantizedNote = false;
    quantizedNoteMidi = 0.0f;
    guitarLevelPeak = 0.0f;
    resetOctaveCandidate();
}

void GuitarTargetTracker::resetOctaveCandidate()
{
    octaveCandidateSamples = 0;
    haveOctaveCandidate = false;
    octaveCandidateMidi = 0.0f;
}

bool GuitarTargetTracker::isOctaveFoldCandidate(float midi, float previousNote)
{
    const float rawQuantized = std::round(midi);
    const float pitchClassDelta = std::remainder(rawQuantized - previousNote, 12.0f);
    return std::abs(pitchClassDelta) < 0.5f
        && std::abs(rawQuantized - previousNote) >= 11.5f;
}

float GuitarTargetTracker::quantizeMidiWithHysteresis(float midi, bool havePreviousNote,
                                                      float previousNote,
                                                      bool octaveChangeConfirmed)
{
    if (havePreviousNote)
    {
        // 반음 경계의 작은 흔들림은 옥타브와 무관하게 같은 음표로 유지한다.
        if (std::abs(midi - previousNote) < mode2::params::guitarNoteHysteresisSemitones)
            return previousNote;

        // 여기부터가 옥타브 되접기다. 확인된 옥타브 이동이면 통째로 건너뛴다.
        //
        // 주의: 되접기는 두 군데서 일어난다. 아래 nearbyMidi 비교도 "직전 음표와 가장 가까운
        // 옥타브"로 접어서 보므로, 정확히 한 옥타브 위/아래 음은 이 비교만으로도 직전 목표로
        // 되돌아간다. 확인 플래그가 pitchClass 검사에만 걸려 있으면 아무 효과가 없다.
        if (! octaveChangeConfirmed)
        {
            // 이 값을 다음 상태의 기준으로 저장하면 잡음의 피치 클래스가 조금씩 바뀔 때
            // 옥타브가 무한히 누적 이동할 수 있으므로, 실제 새 음표는 원본 midi에서 만든다.
            const float nearbyMidi = midi + 12.0f * std::round((previousNote - midi) / 12.0f);
            if (std::abs(nearbyMidi - previousNote)
                < mode2::params::guitarNoteHysteresisSemitones)
                return previousNote;

            if (isOctaveFoldCandidate(midi, previousNote))
                return previousNote;
        }
    }

    return std::round(midi);
}

GuitarTargetTracker::Output GuitarTargetTracker::processBlock(const float* guitarSamples, int numSamples)
{
    const bool onsetDetected = onsetDetector.processBlock(guitarSamples, numSamples);
    PitchDetector::Result pitch = pitchDetector.processBlock(guitarSamples, numSamples);

    // 피크는 즉시 따라 올라가고 아주 느리게만 내려간다. 그래야 자연 감쇠 중에는 피크도 함께
    // 내려가 비율이 유지되고, 손 뮤트처럼 급격히 떨어질 때만 비율이 무너진다.
    const float rms = computeRms(guitarSamples, numSamples);
    if (rms > guitarLevelPeak)
    {
        guitarLevelPeak = rms;
    }
    else
    {
        const float releaseCoeff = 1.0f - std::exp(
            -static_cast<float>(numSamples)
            / static_cast<float>(mode2::params::guitarLevelPeakReleaseSeconds * sampleRate));
        guitarLevelPeak += releaseCoeff * (rms - guitarLevelPeak);
    }

    const float relativeFloor = guitarLevelPeak * mode2::params::guitarPitchRelativeFloorRatio;
    if (rms < std::max(mode2::params::guitarPitchRmsFloor, relativeFloor))
        pitch.valid = false;

    if (pitch.valid)
    {
        // 옥타브 후보의 지속 시간을 센다. 오검출은 몇 프레임 안에 사라지고 진짜 옥타브
        // 이동은 계속 유지되므로, 유지된 경우에만 되접기를 건너뛴다.
        bool octaveChangeConfirmed = false;
        if (haveQuantizedNote && isOctaveFoldCandidate(pitch.midiFloat, quantizedNoteMidi))
        {
            const bool sameCandidate =
                haveOctaveCandidate
                && std::abs(pitch.midiFloat - octaveCandidateMidi)
                       <= mode2::params::collectConvergenceToleranceSemitones;
            if (sameCandidate)
                octaveCandidateSamples += numSamples;
            else
                octaveCandidateSamples = numSamples;

            haveOctaveCandidate = true;
            octaveCandidateMidi = pitch.midiFloat;
            octaveChangeConfirmed = octaveCandidateSamples >= octaveConfirmSamples;
        }
        else
        {
            resetOctaveCandidate();
        }

        quantizedNoteMidi = quantizeMidiWithHysteresis(pitch.midiFloat, haveQuantizedNote,
                                                       quantizedNoteMidi, octaveChangeConfirmed);
        haveQuantizedNote = true;
        pitch.midiFloat = quantizedNoteMidi;

        // 받아들였으면 후보를 비운다. 안 그러면 다음 블록에서 새 목표 기준으로 다시
        // 후보가 아니게 되는데도 낡은 누적이 남는다.
        if (octaveChangeConfirmed)
            resetOctaveCandidate();
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
