#pragma once

#include <array>
#include <atomic>
#include <cstdint>

namespace mode1
{

enum class RealtimeTraceType : std::uint8_t
{
    onsetDetected,
    scoreEventChanged,
    phraseRequested,
    vocalFirstOutput,
    beatClockObservation,
    beatClockStateChanged,
    introChromaAlignmentLocked,
    predictiveTimingCorrection,
    predictiveTransportState,
    predictivePhraseScheduled,
    predictivePhraseCancelled,
    chordMismatchObservation,
    chordMismatchPaused,
    chordMismatchResumed
};

struct RealtimeTraceEvent
{
    RealtimeTraceType type = RealtimeTraceType::onsetDetected;
    std::int64_t sample = 0;
    std::int64_t relatedSample = 0;
    std::int32_t index = -1;
    float value = 0.0f;
    std::uint32_t flags = 0;
    float value2 = 0.0f;
};

// Single audio-thread producer, single non-realtime consumer.
// The fixed storage avoids allocation, locks, strings and I/O in processBlock.
class RealtimeTraceBuffer
{
public:
    static constexpr std::size_t capacity = 16'384;

    bool push(const RealtimeTraceEvent& event) noexcept
    {
        const auto write = writeIndex.load(std::memory_order_relaxed);
        const auto next = increment(write);
        if (next == readIndex.load(std::memory_order_acquire))
        {
            dropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        events[write] = event;
        writeIndex.store(next, std::memory_order_release);
        return true;
    }

    bool pop(RealtimeTraceEvent& event) noexcept
    {
        const auto read = readIndex.load(std::memory_order_relaxed);
        if (read == writeIndex.load(std::memory_order_acquire))
            return false;

        event = events[read];
        readIndex.store(increment(read), std::memory_order_release);
        return true;
    }

    void reset() noexcept
    {
        readIndex.store(0, std::memory_order_relaxed);
        writeIndex.store(0, std::memory_order_relaxed);
        dropped.store(0, std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t getDroppedCount() const noexcept
    {
        return dropped.load(std::memory_order_relaxed);
    }

private:
    static constexpr std::size_t increment(std::size_t index) noexcept
    {
        return (index + 1) % capacity;
    }

    std::array<RealtimeTraceEvent, capacity> events {};
    std::atomic<std::size_t> readIndex { 0 };
    std::atomic<std::size_t> writeIndex { 0 };
    std::atomic<std::uint64_t> dropped { 0 };
};

} // namespace mode1
