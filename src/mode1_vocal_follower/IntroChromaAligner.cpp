#include "IntroChromaAligner.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace mode1
{

void IntroChromaAligner::prepare(double newSampleRate) noexcept
{
    sampleRate = std::max(1.0, newSampleRate);
    reset();
}

void IntroChromaAligner::setSong(const SongPackage* package) noexcept
{
    song = package;
    chordTemplates.fill({});
    if (song != nullptr)
    {
        const auto& chords = song->getChordTimeline();
        const auto count = std::min(
            chords.size(),
            static_cast<size_t>(maximumChordTemplates));
        for (size_t index = 0; index < count; ++index)
            chordTemplates[index] = makeChordTemplate(chords[index].chord);
    }
    reset();
}

void IntroChromaAligner::reset() noexcept
{
    firstOnsetSample = -1;
    lastFrameSequence = 0;
    hypothesisCount = 0;
    evidenceFrames = 0;
    stableWinnerFrames = 0;
    previousWinnerScale = 0.0;
    previousWinnerOffsetSeconds = 0.0;
    frameHistory.fill({});
    frameHistorySamples.fill(0);
    historyWriteIndex = 0;
    historySize = 0;
    smoothingHistory.fill({});
    smoothingSum.fill(0.0f);
    smoothingWriteIndex = 0;
    smoothingSize = 0;
    hypotheses.fill({});
    result = {};
}

void IntroChromaAligner::noteFirstOnset(std::int64_t sample) noexcept
{
    if (firstOnsetSample >= 0 || sample < 0)
        return;
    firstOnsetSample = sample;
    initialiseHypotheses();
    const int oldest =
        (historyWriteIndex - historySize + historyCapacity)
        % historyCapacity;
    for (int item = 0; item < historySize; ++item)
    {
        const int index = (oldest + item) % historyCapacity;
        const auto& frame = frameHistory[static_cast<size_t>(index)];
        if (frame.valid)
            accumulateFrame(
                frameHistorySamples[static_cast<size_t>(index)],
                frame.values);
    }
}

void IntroChromaAligner::processFrame(
    std::int64_t sample,
    const ChromaFrame& frame,
    bool active) noexcept
{
    if (result.locked
        || !active
        || !frame.valid
        || frame.sequence == 0
        || frame.sequence == lastFrameSequence
        || song == nullptr)
        return;

    lastFrameSequence = frame.sequence;
    if (firstOnsetSample < 0)
    {
        frameHistory[static_cast<size_t>(historyWriteIndex)] = frame;
        frameHistorySamples[static_cast<size_t>(historyWriteIndex)] = sample;
        historyWriteIndex = (historyWriteIndex + 1) % historyCapacity;
        historySize = std::min(historyCapacity, historySize + 1);
        return;
    }
    if (hypothesisCount == 0)
        return;

    accumulateFrame(sample, frame.values);
    const double performanceSeconds =
        static_cast<double>(sample) / sampleRate;
    updateResult(performanceSeconds);
}

void IntroChromaAligner::accumulateFrame(
    std::int64_t sample,
    const std::array<float, 12>& values) noexcept
{
    const double performanceSeconds =
        static_cast<double>(sample) / sampleRate;
    auto& outgoing =
        smoothingHistory[static_cast<size_t>(smoothingWriteIndex)];
    if (smoothingSize == smoothingCapacity)
    {
        for (size_t pitch = 0; pitch < smoothingSum.size(); ++pitch)
            smoothingSum[pitch] -= outgoing[pitch];
    }
    else
    {
        ++smoothingSize;
    }
    outgoing = values;
    for (size_t pitch = 0; pitch < smoothingSum.size(); ++pitch)
        smoothingSum[pitch] += values[pitch];
    smoothingWriteIndex =
        (smoothingWriteIndex + 1) % smoothingCapacity;

    std::array<float, 12> centred {};
    for (size_t pitch = 0; pitch < centred.size(); ++pitch)
        centred[pitch] =
            smoothingSum[pitch] / static_cast<float>(smoothingSize);
    float mean = 0.0f;
    for (const auto value : centred)
        mean += value;
    mean /= 12.0f;
    float norm = 0.0f;
    for (auto& value : centred)
    {
        value -= mean;
        norm += value * value;
    }
    norm = std::sqrt(norm);
    if (norm <= 1.0e-6f)
        return;
    for (auto& value : centred)
        value /= norm;

    for (int index = 0; index < hypothesisCount; ++index)
    {
        auto& hypothesis = hypotheses[static_cast<size_t>(index)];
        const double scoreSeconds =
            (performanceSeconds - hypothesis.offsetSeconds)
            / hypothesis.scale;
        if (scoreSeconds < 0.0)
            continue;
        const auto& expected = templateAt(scoreSeconds);
        float similarity = 0.0f;
        for (size_t pitch = 0; pitch < centred.size(); ++pitch)
            similarity += centred[pitch] * expected[pitch];
        hypothesis.score += similarity;
        ++hypothesis.frames;
    }
    ++evidenceFrames;
}

void IntroChromaAligner::initialiseHypotheses() noexcept
{
    if (song == nullptr || firstOnsetSample < 0)
        return;
    const auto& chords = song->getChordTimeline();
    if (chords.empty())
        return;

    const double onsetSeconds =
        static_cast<double>(firstOnsetSample) / sampleRate;
    double firstPlayedScoreSeconds = chords.front().startSeconds;
    for (const auto& chord : chords)
    {
        // Packages can contain a score-zero setup/dummy event. The first
        // positive boundary is the event represented by the first real
        // guitar onset in the current start/arm workflow.
        if (chord.startSeconds > 0.05)
        {
            firstPlayedScoreSeconds = chord.startSeconds;
            break;
        }
    }

    const auto addHypothesis = [this, onsetSeconds, firstPlayedScoreSeconds](
        double scale,
        double jitter) noexcept
    {
        if (hypothesisCount >= maximumHypotheses)
            return;
        auto& hypothesis =
            hypotheses[static_cast<size_t>(hypothesisCount++)];
        hypothesis.scale = scale;
        hypothesis.offsetSeconds =
            onsetSeconds - scale * firstPlayedScoreSeconds + jitter;
    };

    for (double scale = 0.92; scale <= 1.06001; scale += 0.004)
    {
        for (double jitter = -0.12; jitter <= 0.12001; jitter += 0.024)
            addHypothesis(scale, jitter);
    }
}

void IntroChromaAligner::updateResult(double performanceSeconds) noexcept
{
    if (firstOnsetSample < 0 || evidenceFrames < 40)
        return;
    const double observedSeconds =
        performanceSeconds
        - static_cast<double>(firstOnsetSample) / sampleRate;
    if (observedSeconds < 4.5)
        return;

    int best = -1;
    int second = -1;
    double bestMean = -std::numeric_limits<double>::infinity();
    double secondMean = -std::numeric_limits<double>::infinity();
    for (int index = 0; index < hypothesisCount; ++index)
    {
        const auto& hypothesis = hypotheses[static_cast<size_t>(index)];
        if (hypothesis.frames <= 0)
            continue;
        const double mean = hypothesis.score / hypothesis.frames;
        if (mean > bestMean)
        {
            secondMean = bestMean;
            second = best;
            bestMean = mean;
            best = index;
        }
        else if (mean > secondMean)
        {
            secondMean = mean;
            second = index;
        }
    }
    if (best < 0)
        return;

    const auto& winner = hypotheses[static_cast<size_t>(best)];
    const double margin = second >= 0 ? bestMean - secondMean : bestMean;
    const bool sameWinnerNeighbourhood =
        previousWinnerScale > 0.0
        && std::abs(winner.scale - previousWinnerScale) <= 0.0081
        && std::abs(
            winner.offsetSeconds - previousWinnerOffsetSeconds) <= 0.0401;
    stableWinnerFrames =
        sameWinnerNeighbourhood ? stableWinnerFrames + 1 : 1;
    previousWinnerScale = winner.scale;
    previousWinnerOffsetSeconds = winner.offsetSeconds;

    result.performanceSecondsPerScoreSecond = winner.scale;
    result.offsetSeconds = winner.offsetSeconds;
    result.confidence = juce::jlimit(
        0.0,
        1.0,
        std::max(
            margin * 40.0,
            static_cast<double>(stableWinnerFrames) / 25.0));
    result.harmonicMargin = margin;
    result.evidenceFrames = evidenceFrames;
    // Chroma candidates commonly have tiny absolute margins, while the same
    // tempo/phase neighbourhood remains the winner for many consecutive
    // frames. Temporal persistence is therefore the primary lock criterion.
    result.locked = bestMean >= 0.05 && stableWinnerFrames >= 15;
}

std::array<float, 12> IntroChromaAligner::makeChordTemplate(
    const juce::String& input) noexcept
{
    std::array<float, 12> values {};
    auto chord = input.trim().upToFirstOccurrenceOf("/", false, false);
    if (chord.isEmpty() || chord == "N" || chord == "-")
        return values;

    int root = -1;
    switch (chord[0])
    {
        case 'C': root = 0; break;
        case 'D': root = 2; break;
        case 'E': root = 4; break;
        case 'F': root = 5; break;
        case 'G': root = 7; break;
        case 'A': root = 9; break;
        case 'B': root = 11; break;
        default: return values;
    }
    int nameLength = 1;
    if (chord.length() > 1 && chord[1] == '#')
    {
        root = (root + 1) % 12;
        nameLength = 2;
    }
    else if (chord.length() > 1
             && (chord[1] == 'b' || chord[1] == 'B'))
    {
        root = (root + 11) % 12;
        nameLength = 2;
    }
    const auto suffix = chord.substring(nameLength).toLowerCase();
    const bool minor = suffix.startsWith("m") && !suffix.startsWith("maj");
    std::array<int, 4> intervals { 0, minor ? 3 : 4, 7, -1 };
    if (suffix.contains("sus2"))
        intervals[1] = 2;
    else if (suffix.contains("sus4"))
        intervals[1] = 5;
    if (suffix.contains("maj7"))
        intervals[3] = 11;
    else if (suffix.contains("7"))
        intervals[3] = 10;

    int count = 0;
    for (const int interval : intervals)
    {
        if (interval >= 0)
        {
            values[static_cast<size_t>((root + interval) % 12)] = 1.0f;
            ++count;
        }
    }
    const float mean = static_cast<float>(count) / 12.0f;
    float norm = 0.0f;
    for (auto& value : values)
    {
        value -= mean;
        norm += value * value;
    }
    norm = std::sqrt(norm);
    if (norm > 1.0e-6f)
        for (auto& value : values)
            value /= norm;
    return values;
}

const std::array<float, 12>& IntroChromaAligner::templateAt(
    double scoreSeconds) const noexcept
{
    static const std::array<float, 12> empty {};
    if (song == nullptr || scoreSeconds < 0.0)
        return empty;
    const auto& chords = song->getChordTimeline();
    if (chords.empty())
        return empty;
    int index = 0;
    for (int candidate = 1;
         candidate < static_cast<int>(chords.size())
             && chords[static_cast<size_t>(candidate)].startSeconds
                 <= scoreSeconds;
         ++candidate)
        index = candidate;
    if (index >= maximumChordTemplates)
        return empty;
    return chordTemplates[static_cast<size_t>(index)];
}

} // namespace mode1
