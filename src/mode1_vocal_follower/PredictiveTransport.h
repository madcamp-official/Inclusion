#pragma once

#include "BeatClock.h"
#include "SongPackage.h"

#include <array>
#include <cstdint>

namespace mode1
{

enum class PredictiveTransportEventType : std::uint8_t
{
    phraseScheduled,
    phraseCancelled
};

struct PredictiveTransportEvent
{
    PredictiveTransportEventType type =
        PredictiveTransportEventType::phraseScheduled;
    int phraseIndex = -1;
    std::int64_t targetSample = -1;
    std::int64_t decisionSample = -1;
    double confidence = 0.0;
};

struct PredictiveTransportSnapshot
{
    BeatClockState state = BeatClockState::disarmed;
    double scoreSeconds = 0.0;
    double performanceSecondsPerScoreSecond = 1.0;
    double phaseErrorSeconds = 0.0;
    double confidence = 0.0;
    int acceptedObservations = 0;
    int rejectedObservations = 0;
};

// Output-independent transport used by Active v2 Shadow. It owns a
// continuously advancing score position and emits only predicted/cancelled
// phrase targets. It never starts PhrasePlayer audio.
class PredictiveTransport
{
public:
    void prepare(double sampleRate, double nominalBpm) noexcept;
    void setSong(const SongPackage* package) noexcept;
    void reset() noexcept;
    void applyIntroAlignment(
        std::int64_t sample,
        double alignedPerformanceSecondsPerScoreSecond,
        double alignedOffsetSeconds,
        double alignedConfidence,
        double harmonicMargin) noexcept;
    [[nodiscard]] bool usedReactiveIntroHypothesis() const noexcept
    {
        return reactiveIntroHypothesisUsed;
    }
    bool observeHarmonicOnset(
        std::int64_t onsetSample,
        const std::array<float, 12>& chroma) noexcept;
    void holdForChordMismatch(std::int64_t decisionSample) noexcept;
    void resumeFromChordMismatch(
        std::int64_t decisionSample,
        int observedEventIndex,
        double observedScoreSeconds) noexcept;

    void processBlock(
        std::int64_t blockStartSample,
        int numSamples,
        bool running,
        bool guitarActive,
        bool rawOnset,
        std::int64_t onsetSample,
        bool scoreObservation,
        int observedEventIndex,
        double observedScoreSeconds) noexcept;

    [[nodiscard]] PredictiveTransportSnapshot getSnapshot() const noexcept;
    bool popEvent(PredictiveTransportEvent& event) noexcept;

private:
    struct Reservation
    {
        int phraseIndex = -1;
        std::int64_t targetSample = -1;
        bool active = false;
    };

    void advanceTo(std::int64_t sample) noexcept;
    void observe(
        std::int64_t sample,
        int eventIndex,
        double scoreSeconds) noexcept;
    void updateState(
        std::int64_t blockEndSample,
        bool guitarActive) noexcept;
    void schedulePhrases(std::int64_t decisionSample) noexcept;
    void cancelFutureReservations(std::int64_t decisionSample) noexcept;
    void pushEvent(const PredictiveTransportEvent& event) noexcept;
    static std::array<float, 12> makeChordTemplate(
        const juce::String& chord) noexcept;

    static constexpr int maximumReservations = 8;
    static constexpr int maximumPendingEvents = 16;
    static constexpr int maximumChordTemplates = 256;

    const SongPackage* song = nullptr;
    double sampleRate = 48'000.0;
    double nominalSecondsPerBeat = 0.5;
    double scoreSeconds = 0.0;
    double performanceSecondsPerScoreSecond = 1.0;
    double phaseErrorSeconds = 0.0;
    double confidence = 0.0;
    std::int64_t currentSample = 0;
    std::int64_t lastRawOnsetSample = -1;
    std::int64_t lastObservationSample = -1;
    double lastObservedScoreSeconds = -1.0;
    int lastObservedEventIndex = -1;
    int acceptedObservations = 0;
    int rejectedObservations = 0;
    int nextPhraseIndex = 0;
    BeatClockState state = BeatClockState::disarmed;
    bool introAlignmentLocked = false;
    bool externalChordMismatchHold = false;
    bool reactiveIntroHypothesisUsed = false;
    double introAlignmentRate = 1.0;
    int lastTrustedEventIndex = -1;
    std::int64_t lastTrustedOnsetSample = -1;
    double lastTrustedScoreSeconds = -1.0;
    std::array<double, 5> recentRates {};
    int recentRateCount = 0;
    int recentRateWriteIndex = 0;
    std::array<double, 5> calibrationScoreSeconds {};
    std::array<double, 5> calibrationPerformanceSeconds {};
    int calibrationCount = 0;
    bool calibrationApplied = false;
    double calibratedReferenceRate = 1.0;
    std::array<std::array<float, 12>, maximumChordTemplates> chordTemplates {};

    std::array<Reservation, maximumReservations> reservations {};
    std::array<PredictiveTransportEvent, maximumPendingEvents> pendingEvents {};
    int pendingReadIndex = 0;
    int pendingWriteIndex = 0;

    // VARIANT_E: a phrase's targetSample is frozen the moment it's reserved
    // and never revisited by later phase corrections (only cancellation can
    // remove it). A large lookahead freezes phrases far in advance, giving
    // the free-running score more time to drift before the frozen target
    // fires. Shrinking it means less time for uncorrected drift to bake in,
    // at the cost of a smaller cancellation window on a chord mismatch.
    double lookaheadSeconds = 0.120;
    double commitHorizonSeconds = 0.050;
};

} // namespace mode1
