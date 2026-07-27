#include "GuitarTargetTracker.h"

void GuitarTargetTracker::prepare(double sampleRateIn)
{
    onsetDetector.prepare(sampleRateIn);
    pitchDetector.prepare(sampleRateIn);
    pitchStabilizer.prepare(sampleRateIn);
}

void GuitarTargetTracker::reset()
{
    onsetDetector.reset();
    pitchDetector.reset();
    pitchStabilizer.reset();
}

GuitarTargetTracker::Output GuitarTargetTracker::processBlock(const float* guitarSamples, int numSamples)
{
    const bool onsetDetected = onsetDetector.processBlock(guitarSamples, numSamples);
    const PitchDetector::Result pitch = pitchDetector.processBlock(guitarSamples, numSamples);
    const PitchStabilizer::Output stabilized = pitchStabilizer.processBlock(onsetDetected, pitch, numSamples);

    Output out;
    out.hasTarget = stabilized.hasTarget;
    out.targetMidi = stabilized.targetMidi;
    out.fadeGain = stabilized.fadeGain;
    out.state = stabilized.state;
    return out;
}
