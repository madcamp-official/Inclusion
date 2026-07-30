#pragma once

#include <algorithm>

namespace mode1
{

enum class ChordMismatchDecision
{
    none,
    pause,
    resume
};

// Audio-thread-only state machine. Low-confidence FFT guesses are ignored,
// two consecutive usable mismatches pause, and one usable match resumes.
// Keeping this separate from score advancement means the feature adds no
// confirmation wait to the normal predictive playback path.
class ChordMismatchGate
{
public:
    void reset() noexcept
    {
        paused = false;
        consecutiveMismatches = 0;
        mismatchExpectedRoot = -1;
        mismatchDetectedRoot = -1;
        firstMismatchTimeSeconds = -1.0;
    }

    [[nodiscard]] bool isPaused() const noexcept { return paused; }

    ChordMismatchDecision observe(
        int expectedRootPitchClass,
        int detectedRootPitchClass,
        float confidence,
        float expectedChordSimilarity = -1.0f,
        double observationTimeSeconds = -1.0) noexcept
    {
        if (expectedRootPitchClass < 0
            || expectedRootPitchClass >= 12
            || detectedRootPitchClass < 0
            || detectedRootPitchClass >= 12
            || confidence < minimumConfidence)
        {
            return ChordMismatchDecision::none;
        }

        // Root-template classification is unstable on arpeggios: an FFT
        // window containing only two notes can be labelled as a different
        // chord even though its chroma is compatible with the written chord.
        const bool matches =
            expectedRootPitchClass == detectedRootPitchClass
            || expectedChordSimilarity >= minimumCompatibleSimilarity;
        if (paused)
        {
            if (!matches)
                return ChordMismatchDecision::none;

            paused = false;
            consecutiveMismatches = 0;
            mismatchExpectedRoot = -1;
            mismatchDetectedRoot = -1;
            firstMismatchTimeSeconds = -1.0;
            return ChordMismatchDecision::resume;
        }

        if (matches)
        {
            consecutiveMismatches = 0;
            mismatchExpectedRoot = -1;
            mismatchDetectedRoot = -1;
            firstMismatchTimeSeconds = -1.0;
            return ChordMismatchDecision::none;
        }

        if (mismatchExpectedRoot != expectedRootPitchClass
            || mismatchDetectedRoot != detectedRootPitchClass)
        {
            mismatchExpectedRoot = expectedRootPitchClass;
            mismatchDetectedRoot = detectedRootPitchClass;
            consecutiveMismatches = 1;
            firstMismatchTimeSeconds = observationTimeSeconds;
        }
        else
        {
            consecutiveMismatches = std::min(
                requiredConsecutiveMismatches,
                consecutiveMismatches + 1);
        }
        if (consecutiveMismatches < requiredConsecutiveMismatches)
            return ChordMismatchDecision::none;
        if (observationTimeSeconds >= 0.0
            && firstMismatchTimeSeconds >= 0.0
            && observationTimeSeconds - firstMismatchTimeSeconds
                < minimumMismatchDurationSeconds)
        {
            return ChordMismatchDecision::none;
        }

        paused = true;
        consecutiveMismatches = 0;
        return ChordMismatchDecision::pause;
    }

private:
    static constexpr int requiredConsecutiveMismatches = 1;
    // GuitarChordTracker already rejects weak absolute template scores via
    // ChordDetection::valid. Its remaining best-vs-second margin is normally
    // only a few thousandths even for a clean synthetic triad.
    static constexpr float minimumConfidence = 0.004f;
    static constexpr float minimumCompatibleSimilarity = 0.10f;
    static constexpr double minimumMismatchDurationSeconds = 0.120;

    bool paused = false;
    int consecutiveMismatches = 0;
    int mismatchExpectedRoot = -1;
    int mismatchDetectedRoot = -1;
    double firstMismatchTimeSeconds = -1.0;
};

} // namespace mode1
