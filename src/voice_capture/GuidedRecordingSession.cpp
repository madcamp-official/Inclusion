#include "GuidedRecordingSession.h"

#include <algorithm>

namespace voice_capture
{

const std::vector<GuidedRecordingSession::Item>& GuidedRecordingSession::speakingItems()
{
    static const std::vector<Item> items {
        { L"지금 읽어주세요",
          L"오늘은 내 목소리로 노래를 만들기 위한 음성 샘플을 녹음합니다." },
        { L"지금 읽어주세요",
          L"바람이 불어오는 길을 따라 천천히 걸으며 주변의 소리를 들어봅니다." },
        { L"지금 읽어주세요",
          L"기쁘고 밝은 순간도 있고, 조용하고 차분한 순간도 있습니다." },
        { L"지금 읽어주세요",
          L"봄에는 따뜻한 햇살이 비치고, 여름에는 시원한 비가 내립니다." },
        { L"지금 읽어주세요",
          L"가나다라마바사, 아자차카타파하." },
        { L"지금 읽어주세요",
          L"빠르게 말하지 않고, 평소 사용하는 편안한 목소리로 읽겠습니다." },
    };
    return items;
}

const std::vector<GuidedRecordingSession::Item>& GuidedRecordingSession::vowelItems()
{
    static const std::vector<Item> items {
        { L"화면의 모음을 편안하게 길게 발성해 주세요", L"아——————" },
        { L"화면의 모음을 편안하게 길게 발성해 주세요", L"에——————" },
        { L"화면의 모음을 편안하게 길게 발성해 주세요", L"아——————" },
    };
    return items;
}

const std::vector<GuidedRecordingSession::Item>& GuidedRecordingSession::defaultSongItems()
{
    // Generic placeholder lines used only when the caller hasn't supplied
    // real song lyrics via setSongLines() (see that method).
    static const std::vector<Item> items {
        { L"반주 없이 다음 가사를 자연스럽게 불러 주세요", L"오늘의 바람이 나를 불러" },
        { L"반주 없이 다음 가사를 자연스럽게 불러 주세요", L"멀리 빛나는 길을 따라서" },
        { L"반주 없이 다음 가사를 자연스럽게 불러 주세요", L"조용히 마음을 열어 보면" },
        { L"반주 없이 다음 가사를 자연스럽게 불러 주세요", L"우리의 노래가 다시 피어나" },
        { L"반주 없이 다음 가사를 자연스럽게 불러 주세요", L"흐르는 시간 속에 남은 기억" },
        { L"반주 없이 다음 가사를 자연스럽게 불러 주세요", L"작은 목소리로 시작해 볼게" },
        { L"반주 없이 다음 가사를 자연스럽게 불러 주세요", L"높이 낮게 마음대로 불러도 좋아" },
        { L"반주 없이 다음 가사를 자연스럽게 불러 주세요", L"오래된 노래처럼 편안하게" },
        { L"반주 없이 다음 가사를 자연스럽게 불러 주세요", L"창밖에 스치는 바람을 느끼며" },
        { L"반주 없이 다음 가사를 자연스럽게 불러 주세요", L"다시 한번 처음처럼 불러본다" },
    };
    return items;
}

const std::vector<GuidedRecordingSession::Item>& GuidedRecordingSession::songItemsInUse() const
{
    return customSongItems.empty() ? defaultSongItems() : customSongItems;
}

int GuidedRecordingSession::itemCountFor(GuidedRecordingStage forStage) const
{
    switch (forStage)
    {
        case GuidedRecordingStage::vowel: return static_cast<int>(vowelItems().size());
        case GuidedRecordingStage::song: return static_cast<int>(songItemsInUse().size());
        case GuidedRecordingStage::speaking:
        case GuidedRecordingStage::finished:
        default: return static_cast<int>(speakingItems().size());
    }
}

const GuidedRecordingSession::Item& GuidedRecordingSession::itemFor(
    GuidedRecordingStage forStage,
    int index) const
{
    const auto& items =
        forStage == GuidedRecordingStage::vowel ? vowelItems()
        : forStage == GuidedRecordingStage::song ? songItemsInUse()
                                                  : speakingItems();
    const auto boundedIndex =
        juce::jlimit(0, static_cast<int>(items.size()) - 1, index);
    return items[static_cast<size_t>(boundedIndex)];
}

void GuidedRecordingSession::setSongLines(const juce::StringArray& lines)
{
    const juce::SpinLock::ScopedLockType scopedLock(lock);
    customSongItems.clear();
    customSongItems.reserve(static_cast<size_t>(lines.size()));
    for (const auto& line : lines)
    {
        const auto trimmed = line.trim();
        if (trimmed.isNotEmpty())
            customSongItems.push_back(
                { L"반주 없이 다음 가사를 자연스럽게 불러 주세요", trimmed });
    }
}

void GuidedRecordingSession::prepare(double newSampleRate)
{
    const juce::SpinLock::ScopedLockType scopedLock(lock);
    sampleRate = std::max(1.0, newSampleRate);
    tracker.prepare(sampleRate);
}

void GuidedRecordingSession::start() noexcept
{
    const juce::SpinLock::ScopedLockType scopedLock(lock);
    active = true;
    sessionFinished = false;
    stage = GuidedRecordingStage::speaking;
    itemIndex = 0;
    songStageElapsedSeconds = 0.0;
    continuousRecordingSeconds = 0.0;
    captureHasStarted = false;
    beginItemLocked();
}

void GuidedRecordingSession::stop() noexcept
{
    const juce::SpinLock::ScopedLockType scopedLock(lock);
    active = false;
}

juce::StringArray GuidedRecordingSession::tokenizePrompt(
    const juce::String& prompt,
    GuidedRecordingStage forStage)
{
    juce::StringArray tokens;
    if (forStage == GuidedRecordingStage::song)
    {
        // Syllable granularity: each Hangul character is one syllable, so
        // highlighting per character matches how the line is actually sung.
        for (auto character = prompt.begin(); character != prompt.end(); ++character)
        {
            const juce::juce_wchar codepoint = *character;
            if (!juce::CharacterFunctions::isWhitespace(codepoint))
                tokens.add(juce::String::charToString(codepoint));
        }
        return tokens;
    }

    tokens.addTokens(prompt, " ", "");
    tokens.removeEmptyStrings();
    return tokens;
}

void GuidedRecordingSession::beginItemLocked() noexcept
{
    // The song stage is captured as one continuous take: once it has
    // started, advancing to the next lyric line must not stop the recorder
    // or re-run the get-ready countdown, otherwise the singer is cut off
    // mid-phrase at every line boundary.
    if (captureHasStarted)
    {
        phase = ItemPhase::listening;
    }
    else
    {
        phase = ItemPhase::countdown;
        countdownRemainingSeconds = countdownSeconds;
        tracker.reset();
        itemElapsedSeconds = 0.0;
        secondsVoicedTotal = 0.0;
    }

    highlightedWordIndex = -1;
    songSyllableProgress = 0.0;

    if (stage == GuidedRecordingStage::vowel)
    {
        currentItemWordCount = 1;
        currentItemMaxSeconds = vowelItemMaxSeconds;
        return;
    }

    const auto& item = itemFor(stage, itemIndex);
    currentItemWordCount =
        juce::jmax(1, tokenizePrompt(item.prompt, stage).size());
    currentItemMaxSeconds =
        perWordMaxSecondsBase + currentItemWordCount * perWordMaxSecondsFactor;
}

void GuidedRecordingSession::advanceToNextItemLocked() noexcept
{
    if (stage == GuidedRecordingStage::speaking)
    {
        ++itemIndex;
        if (itemIndex >= itemCountFor(stage))
        {
            stage = GuidedRecordingStage::vowel;
            itemIndex = 0;
        }
    }
    else if (stage == GuidedRecordingStage::vowel)
    {
        ++itemIndex;
        if (itemIndex >= itemCountFor(stage))
        {
            stage = GuidedRecordingStage::song;
            itemIndex = 0;
        }
    }
    else if (stage == GuidedRecordingStage::song)
    {
        stage = GuidedRecordingStage::finished;
        active = false;
        sessionFinished = true;
        return;
    }

    beginItemLocked();
}

void GuidedRecordingSession::processSongBlockLocked(double blockSeconds) noexcept
{
    songStageElapsedSeconds += blockSeconds;
}

void GuidedRecordingSession::processAudioBlock(
    const float* input,
    int numSamples) noexcept
{
    if (!active || input == nullptr || numSamples <= 0)
        return;

    const juce::SpinLock::ScopedTryLockType scopedLock(lock);
    if (!scopedLock.isLocked())
        return;

    if (!active || phase == ItemPhase::awaitingFinalize)
        return;

    const double blockSeconds = static_cast<double>(numSamples) / sampleRate;

    if (phase == ItemPhase::countdown)
    {
        countdownRemainingSeconds -= blockSeconds;
        if (countdownRemainingSeconds <= 0.0)
        {
            phase = ItemPhase::listening;
            captureHasStarted = true;
            tracker.reset();
            highlightedWordIndex = -1;
            itemElapsedSeconds = 0.0;
            secondsVoicedTotal = 0.0;
        }
        return;
    }

    tracker.processBlock(input, numSamples);
    itemElapsedSeconds += blockSeconds;
    continuousRecordingSeconds += blockSeconds;
    if (tracker.isVoiced())
        secondsVoicedTotal += blockSeconds;

    if (stage == GuidedRecordingStage::song)
        processSongBlockLocked(blockSeconds);
}

void GuidedRecordingSession::requestSkip() noexcept
{
    const juce::SpinLock::ScopedLockType scopedLock(lock);
    if (!active || phase != ItemPhase::listening)
        return;

    if (stage == GuidedRecordingStage::song
        && continuousRecordingSeconds < minimumSessionSeconds)
    {
        // Mid-song skip means "move to the next lyric line", not "end the
        // whole song stage" — the take keeps rolling.
        ++itemIndex;
        if (itemIndex >= itemCountFor(stage))
            itemIndex = 0;
        songSyllableProgress = 0.0;
        highlightedWordIndex = -1;
        currentItemWordCount = juce::jmax(
            1, tokenizePrompt(itemFor(stage, itemIndex).prompt, stage).size());
        return;
    }

    if (stage == GuidedRecordingStage::song)
    {
        phase = ItemPhase::awaitingFinalize;
        return;
    }

    advanceToNextItemLocked();
}

void GuidedRecordingSession::requestFinishSongEarly() noexcept
{
    const juce::SpinLock::ScopedLockType scopedLock(lock);
    if (!active || stage != GuidedRecordingStage::song || phase != ItemPhase::listening)
        return;

    phase = ItemPhase::awaitingFinalize;
}

void GuidedRecordingSession::requestRetry() noexcept
{
    const juce::SpinLock::ScopedLockType scopedLock(lock);
    if (!active)
        return;

    stage = GuidedRecordingStage::speaking;
    itemIndex = 0;
    songStageElapsedSeconds = 0.0;
    continuousRecordingSeconds = 0.0;
    captureHasStarted = false;
    beginItemLocked();
}

void GuidedRecordingSession::notifyItemFinalized() noexcept
{
    const juce::SpinLock::ScopedLockType scopedLock(lock);
    if (!active || phase != ItemPhase::awaitingFinalize)
        return;
    advanceToNextItemLocked();
}

void GuidedRecordingSession::notifyItemRejected() noexcept
{
    const juce::SpinLock::ScopedLockType scopedLock(lock);
    if (!active || phase != ItemPhase::awaitingFinalize)
        return;

    stage = GuidedRecordingStage::speaking;
    itemIndex = 0;
    songStageElapsedSeconds = 0.0;
    continuousRecordingSeconds = 0.0;
    captureHasStarted = false;
    beginItemLocked();
}

bool GuidedRecordingSession::isActive() const noexcept
{
    const juce::SpinLock::ScopedLockType scopedLock(lock);
    return active;
}

bool GuidedRecordingSession::shouldBeCapturing() const noexcept
{
    const juce::SpinLock::ScopedLockType scopedLock(lock);
    return active && phase == ItemPhase::listening;
}

juce::String GuidedRecordingSession::getCurrentCategorySlug() const
{
    const juce::SpinLock::ScopedLockType scopedLock(lock);
    switch (stage)
    {
        case GuidedRecordingStage::vowel: return "vowel";
        case GuidedRecordingStage::song: return "song";
        case GuidedRecordingStage::speaking:
        case GuidedRecordingStage::finished:
        default: return "speaking";
    }
}

GuidedRecordingState GuidedRecordingSession::getState() const
{
    GuidedRecordingState state;
    const juce::SpinLock::ScopedLockType scopedLock(lock);

    state.stage = stage;
    if (!active || stage == GuidedRecordingStage::finished)
    {
        state.sessionFinished = true;
        return state;
    }

    state.itemIndexInStage = itemIndex;
    state.itemCountInStage = itemCountFor(stage);
    state.itemElapsedSeconds = itemElapsedSeconds;
    state.countdownActive = phase == ItemPhase::countdown;
    state.countdownRemainingSeconds = juce::jmax(0.0, countdownRemainingSeconds);
    state.recordingActive = phase == ItemPhase::listening;
    state.awaitingFinalize = phase == ItemPhase::awaitingFinalize;

    const auto& item = itemFor(stage, itemIndex);
    state.heading = item.heading;
    state.fullPrompt = item.prompt;

    if (stage == GuidedRecordingStage::vowel)
    {
        state.words = juce::StringArray(item.prompt);
        state.highlightedWordIndex = -1;
    }
    else
    {
        state.words = tokenizePrompt(item.prompt, stage);
        state.highlightedWordIndex = -1;
    }

    switch (stage)
    {
        case GuidedRecordingStage::speaking: state.stageLabel = L"말하기"; break;
        case GuidedRecordingStage::vowel: state.stageLabel = L"모음 지속 발성"; break;
        case GuidedRecordingStage::song: state.stageLabel = L"노래"; break;
        default: break;
    }

    double stageOrdinal = 0.0;
    double withinStageFraction = 0.0;
    switch (stage)
    {
        case GuidedRecordingStage::speaking:
            stageOrdinal = 0.0;
            withinStageFraction = state.itemCountInStage > 0
                ? static_cast<double>(itemIndex) / state.itemCountInStage
                : 0.0;
            break;
        case GuidedRecordingStage::vowel:
            stageOrdinal = 1.0;
            withinStageFraction = state.itemCountInStage > 0
                ? static_cast<double>(itemIndex) / state.itemCountInStage
                : 0.0;
            break;
        case GuidedRecordingStage::song:
            stageOrdinal = 2.0;
            withinStageFraction = juce::jlimit(
                0.0, 1.0,
                continuousRecordingSeconds / minimumSessionSeconds);
            state.stageProgressSeconds = continuousRecordingSeconds;
            state.stageProgressTargetSeconds = minimumSessionSeconds;
            break;
        default:
            break;
    }
    state.overallProgress =
        juce::jlimit(0.0, 1.0, (stageOrdinal + withinStageFraction) / 3.0);

    return state;
}

} // namespace voice_capture
