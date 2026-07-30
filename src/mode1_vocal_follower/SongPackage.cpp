#include "SongPackage.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace mode1
{
namespace
{
double numberProperty(const juce::DynamicObject* object, const juce::Identifier& name)
{
    if (object == nullptr || !object->hasProperty(name))
        return 0.0;
    return static_cast<double>(object->getProperty(name));
}

juce::String stringProperty(const juce::DynamicObject* object, const juce::Identifier& name)
{
    if (object == nullptr || !object->hasProperty(name))
        return {};
    return object->getProperty(name).toString();
}

bool boolProperty(const juce::DynamicObject* object, const juce::Identifier& name)
{
    if (object == nullptr || !object->hasProperty(name))
        return false;
    return static_cast<bool>(object->getProperty(name));
}
} // namespace

bool SongPackage::loadFromFile(const juce::File& packageFile, juce::String& error)
{
    clear();
    error.clear();

    if (!packageFile.existsAsFile())
    {
        error = "Song package does not exist: " + packageFile.getFullPathName();
        return false;
    }

    juce::var root;
    const auto parseResult = juce::JSON::parse(packageFile.loadFileAsString(), root);
    if (parseResult.failed())
    {
        error = "Invalid song package JSON: " + parseResult.getErrorMessage();
        return false;
    }

    const auto* rootObject = root.getDynamicObject();
    if (rootObject == nullptr)
    {
        error = "Song package root must be an object.";
        return false;
    }

    const auto phraseValue = rootObject->hasProperty("micro_phrases")
        ? rootObject->getProperty("micro_phrases")
        : rootObject->getProperty("phrases");
    const auto* phraseArray = phraseValue.getArray();
    if (phraseArray == nullptr || phraseArray->isEmpty())
    {
        error = "Song package contains no phrases.";
        return false;
    }

    sourceFile = packageFile;
    songName = stringProperty(rootObject, "song");
    scoreBpm = numberProperty(rootObject, "score_bpm");
    baseKeyShift = static_cast<int>(
        numberProperty(rootObject, "base_key_shift"));
    if (rootObject->hasProperty("intro_alignment_offset_sec"))
        introAlignmentOffsetSeconds =
            numberProperty(rootObject, "intro_alignment_offset_sec");
    if (rootObject->hasProperty("intro_alignment_preferred_scale"))
        introAlignmentPreferredScale =
            numberProperty(rootObject, "intro_alignment_preferred_scale");
    if (rootObject->hasProperty("intro_alignment_scale_center"))
        introAlignmentScaleCenter =
            numberProperty(rootObject, "intro_alignment_scale_center");
    if (rootObject->hasProperty("intro_alignment_scale_half_range"))
        introAlignmentScaleHalfRange =
            numberProperty(rootObject, "intro_alignment_scale_half_range");
    if (rootObject->hasProperty("vocal_output_gain"))
        vocalOutputGain = numberProperty(rootObject, "vocal_output_gain");
    if (rootObject->hasProperty("vocal_output_delay_sec"))
        vocalOutputDelaySeconds =
            numberProperty(rootObject, "vocal_output_delay_sec");
    if (rootObject->hasProperty("disable_boundary_prediction"))
        disableBoundaryPrediction =
            boolProperty(rootObject, "disable_boundary_prediction");
    keyAnchors = { baseKeyShift };
    if (const auto* keyObject =
            rootObject->getProperty("key_style").getDynamicObject())
    {
        if (const auto* anchors =
                keyObject->getProperty("available_key_shifts").getArray())
        {
            keyAnchors.clear();
            for (const auto& anchor : *anchors)
                keyAnchors.push_back(static_cast<int>(anchor));
        }
        keyAnchors.push_back(baseKeyShift);
        std::sort(keyAnchors.begin(), keyAnchors.end());
        keyAnchors.erase(
            std::unique(keyAnchors.begin(), keyAnchors.end()),
            keyAnchors.end());
        if (const auto* manualRange =
                keyObject->getProperty("manual_key_shift_range").getArray();
            manualRange != nullptr && manualRange->size() >= 2)
        {
            manualKeyShiftMinimum =
                static_cast<int>(manualRange->getUnchecked(0));
            manualKeyShiftMaximum =
                static_cast<int>(manualRange->getUnchecked(1));
            if (manualKeyShiftMinimum > manualKeyShiftMaximum)
                std::swap(
                    manualKeyShiftMinimum, manualKeyShiftMaximum);
            hasManualKeyShiftRange = true;
        }
    }
    if (const auto* expressionObject =
            rootObject->getProperty("expression_style").getDynamicObject())
    {
        defaultExpressionStrength = static_cast<int>(
            numberProperty(expressionObject, "default_strength"));
        if (const auto* strengths =
                expressionObject->getProperty("available_strengths").getArray())
        {
            expressionStrengths.clear();
            for (const auto& strength : *strengths)
                expressionStrengths.push_back(static_cast<int>(strength));
        }
    }
    if (const auto* styleObject =
            rootObject->getProperty("vocal_style").getDynamicObject())
    {
        if (const auto* warnings =
                styleObject->getProperty("range_warnings").getArray())
        {
            for (const auto& warningValue : *warnings)
            {
                const auto* warningObject = warningValue.getDynamicObject();
                if (warningObject == nullptr)
                    continue;
                const auto type = stringProperty(warningObject, "type");
                const auto semitones =
                    numberProperty(warningObject, "semitones");
                juce::String warningText;
                if (type == "above_comfortable_range")
                    warningText = "High notes exceed profile by "
                        + juce::String(semitones, 1) + " st";
                else if (type == "below_comfortable_range")
                    warningText = "Low notes exceed profile by "
                        + juce::String(semitones, 1) + " st";
                if (warningText.isNotEmpty())
                    rangeWarning +=
                        (rangeWarning.isEmpty() ? "" : " / ") + warningText;
            }
        }
    }
    const auto packageDirectory = packageFile.getParentDirectory();

    if (const auto* chordArray =
            rootObject->getProperty("chord_timeline").getArray())
    {
        chordTimeline.reserve(static_cast<size_t>(chordArray->size()));
        for (const auto& chordValue : *chordArray)
        {
            const auto* chordObject = chordValue.getDynamicObject();
            if (chordObject == nullptr)
                continue;

            const auto chord = stringProperty(chordObject, "chord");
            chordTimeline.push_back({
                numberProperty(chordObject, "start_sec"),
                chord,
                stringProperty(chordObject, "raw_chord"),
            });
        }
        std::sort(
            chordTimeline.begin(),
            chordTimeline.end(),
            [](const ChordEvent& left, const ChordEvent& right)
            {
                return left.startSeconds < right.startSeconds;
            });
    }

    if (const auto* tabObject =
            rootObject->getProperty("tab_tracking").getDynamicObject())
    {
        if (const auto* hintArray =
                tabObject->getProperty("chord_hints").getArray())
        {
            tabChordHints.reserve(static_cast<size_t>(hintArray->size()));
            for (const auto& hintValue : *hintArray)
            {
                const auto* hintObject = hintValue.getDynamicObject();
                if (hintObject == nullptr)
                    continue;
                const int chordEventIndex = static_cast<int>(
                    numberProperty(hintObject, "chord_event_index"));
                if (chordEventIndex < 0
                    || chordEventIndex >= static_cast<int>(
                        chordTimeline.size()))
                    continue;
                double firstOnsetOffsetSeconds = 0.0;
                if (const auto* events =
                        hintObject->getProperty("events").getArray();
                    events != nullptr && !events->isEmpty())
                {
                    firstOnsetOffsetSeconds =
                        std::numeric_limits<double>::max();
                    for (const auto& eventValue : *events)
                    {
                        const auto* eventObject =
                            eventValue.getDynamicObject();
                        if (eventObject != nullptr)
                            firstOnsetOffsetSeconds = std::min(
                                firstOnsetOffsetSeconds,
                                numberProperty(
                                    eventObject, "offset_sec"));
                    }
                    if (!std::isfinite(firstOnsetOffsetSeconds))
                        firstOnsetOffsetSeconds = 0.0;
                }
                tabChordHints.push_back({
                    chordEventIndex,
                    static_cast<int>(
                        numberProperty(hintObject, "pitch_class_mask")),
                    static_cast<int>(
                        numberProperty(
                            hintObject,
                            "bass_pitch_class_mask")),
                    static_cast<int>(
                        numberProperty(hintObject, "note_group_count")),
                    numberProperty(hintObject, "arpeggio_likelihood"),
                    std::max(0.0, firstOnsetOffsetSeconds),
                });
            }
            std::sort(
                tabChordHints.begin(),
                tabChordHints.end(),
                [](const TabChordHint& left, const TabChordHint& right)
                {
                    return left.chordEventIndex < right.chordEventIndex;
                });
        }
    }

    std::vector<Phrase> loadedPhrases;
    loadedPhrases.reserve(static_cast<size_t>(phraseArray->size()));

    double previousScoreStart = -1.0;
    for (const auto& phraseValueItem : *phraseArray)
    {
        const auto* phraseObject = phraseValueItem.getDynamicObject();
        if (phraseObject == nullptr)
        {
            error = "Every phrase must be a JSON object.";
            return false;
        }

        Phrase phrase;
        phrase.id = stringProperty(phraseObject, "phrase_id");
        phrase.lyrics = stringProperty(phraseObject, "lyrics");

        const auto* scoreObject =
            phraseObject->getProperty("score").getDynamicObject();
        const auto* sourceObject =
            phraseObject->getProperty("source").getDynamicObject();
        const auto* vocalObject =
            phraseObject->getProperty("vocal").getDynamicObject();

        if (scoreObject == nullptr || sourceObject == nullptr || vocalObject == nullptr)
        {
            error = "Phrase " + phrase.id + " is missing score/source/vocal metadata.";
            return false;
        }

        phrase.scoreStartSeconds = numberProperty(scoreObject, "start_sec");
        phrase.scoreEndSeconds = numberProperty(scoreObject, "end_sec");
        phrase.sourceStartSeconds = numberProperty(sourceObject, "start_sec");
        phrase.sourceEndSeconds = numberProperty(sourceObject, "end_sec");
        phrase.contentOffsetSeconds = numberProperty(vocalObject, "content_offset_sec");
        if (const auto* syncObject =
                vocalObject->getProperty("sync").getDynamicObject())
        {
            phrase.audibleOnsetOffsetSeconds = juce::jlimit(
                0.0, 0.250,
                numberProperty(syncObject, "audible_onset_sec"));
            if (syncObject->hasProperty("vowel_onset_sec"))
                phrase.vowelOnsetOffsetSeconds = juce::jlimit(
                    0.0, 0.250,
                    numberProperty(syncObject, "vowel_onset_sec"));
            if (syncObject->hasProperty("confidence"))
                phrase.vocalAnchorConfidence = juce::jlimit(
                    0.0, 1.0,
                    numberProperty(syncObject, "confidence"));
        }
        const auto vocalDirectoryName = vocalObject->hasProperty("directory")
            ? stringProperty(vocalObject, "directory")
            : juce::String("vocals");
        phrase.vocalFile = packageDirectory
            .getChildFile(vocalDirectoryName)
            .getChildFile(stringProperty(vocalObject, "file"));
        if (const auto* variantsObject =
                phraseObject->getProperty("vocal_variants").getDynamicObject())
        {
            for (const auto& property : variantsObject->getProperties())
            {
                const auto* variantObject = property.value.getDynamicObject();
                if (variantObject == nullptr)
                    continue;
                const auto directory = stringProperty(variantObject, "directory");
                phrase.vocalVariants.push_back({
                    property.name.toString().getIntValue(),
                    baseKeyShift,
                    numberProperty(variantObject, "content_offset_sec"),
                    packageDirectory.getChildFile(directory).getChildFile(
                        stringProperty(variantObject, "file")),
                });
            }
        }
        if (phrase.vocalVariants.empty())
            phrase.vocalVariants.push_back({
                defaultExpressionStrength,
                baseKeyShift,
                phrase.contentOffsetSeconds,
                phrase.vocalFile,
            });
        if (const auto* keyVariantsObject =
                phraseObject->getProperty("vocal_key_variants").getDynamicObject())
        {
            for (const auto& keyProperty : keyVariantsObject->getProperties())
            {
                const int keyShift =
                    keyProperty.name.toString().getIntValue();
                const auto* strengthsObject =
                    keyProperty.value.getDynamicObject();
                if (strengthsObject == nullptr)
                    continue;
                phrase.vocalVariants.erase(
                    std::remove_if(
                        phrase.vocalVariants.begin(),
                        phrase.vocalVariants.end(),
                        [keyShift](const VocalVariant& variant)
                        {
                            return variant.keyShift == keyShift;
                        }),
                    phrase.vocalVariants.end());
                for (const auto& strengthProperty :
                     strengthsObject->getProperties())
                {
                    const auto* variantObject =
                        strengthProperty.value.getDynamicObject();
                    if (variantObject == nullptr)
                        continue;
                    const auto directory =
                        stringProperty(variantObject, "directory");
                    VocalVariant keyedVariant {
                        strengthProperty.name.toString().getIntValue(),
                        keyShift,
                        numberProperty(
                            variantObject, "content_offset_sec"),
                        packageDirectory.getChildFile(directory).getChildFile(
                            stringProperty(variantObject, "file")),
                    };
                    if (variantObject->hasProperty("playback_start_sec"))
                        keyedVariant.playbackStartSeconds = numberProperty(
                            variantObject, "playback_start_sec");
                    if (variantObject->hasProperty("playback_end_sec"))
                        keyedVariant.playbackEndSeconds = numberProperty(
                            variantObject, "playback_end_sec");
                    phrase.vocalVariants.push_back(std::move(keyedVariant));
                }
            }
        }

        if (phrase.scoreEndSeconds <= phrase.scoreStartSeconds
            || phrase.sourceEndSeconds <= phrase.sourceStartSeconds)
        {
            error = "Phrase " + phrase.id + " has an invalid time range.";
            return false;
        }
        if (phrase.scoreStartSeconds < previousScoreStart)
        {
            error = "Phrase score times must be monotonic.";
            return false;
        }
        if (!phrase.vocalFile.existsAsFile())
        {
            error = "Missing phrase audio: " + phrase.vocalFile.getFullPathName();
            return false;
        }
        for (const auto& variant : phrase.vocalVariants)
        {
            if (!variant.vocalFile.existsAsFile())
            {
                error = "Missing vocal variant: "
                    + variant.vocalFile.getFullPathName();
                return false;
            }
        }
        previousScoreStart = phrase.scoreStartSeconds;

        if (const auto* chordArray = phraseObject->getProperty("chords").getArray())
        {
            phrase.chords.reserve(static_cast<size_t>(chordArray->size()));
            for (const auto& chordValue : *chordArray)
            {
                const auto* chordObject = chordValue.getDynamicObject();
                if (chordObject == nullptr)
                    continue;

                phrase.chords.push_back({
                    numberProperty(chordObject, "start_sec"),
                    stringProperty(chordObject, "chord"),
                    stringProperty(chordObject, "raw_chord"),
                });
            }
        }

        loadedPhrases.push_back(std::move(phrase));
    }

    phrases = std::move(loadedPhrases);
    if (!chordTimeline.empty())
    {
        for (auto& phrase : phrases)
        {
            int bestChordIndex = -1;
            double minDistance = std::numeric_limits<double>::max();
            for (size_t chordIdx = 0; chordIdx < chordTimeline.size(); ++chordIdx)
            {
                const auto chord = chordTimeline[chordIdx].chord.trim().toUpperCase();
                if (chord.isEmpty() || chord == "N" || chord == "N.C.")
                    continue;
                const double dist = std::abs(
                    chordTimeline[chordIdx].startSeconds - phrase.sourceStartSeconds);
                if (dist < minDistance && dist <= 0.350)
                {
                    minDistance = dist;
                    bestChordIndex = static_cast<int>(chordIdx);
                }
            }
            if (bestChordIndex >= 0)
            {
                phrase.anchorChordEventIndex = bestChordIndex;
                phrase.chordRelativeStartSeconds = std::max(
                    0.0,
                    phrase.sourceStartSeconds
                        - chordTimeline[static_cast<size_t>(
                            bestChordIndex)].startSeconds);
            }
        }
    }
    return true;
}

void SongPackage::clear()
{
    // juce::File은 File/String 양쪽에서 대입이 가능해 `= {}`가 clang에서 모호해진다.
    sourceFile = juce::File();
    songName.clear();
    scoreBpm = 0.0;
    baseKeyShift = 0;
    introAlignmentOffsetSeconds = 0.540;
    introAlignmentPreferredScale = -1.0;
    introAlignmentScaleCenter = 0.99;
    introAlignmentScaleHalfRange = 0.07;
    vocalOutputGain = 1.0;
    vocalOutputDelaySeconds = 0.0;
    disableBoundaryPrediction = false;
    rangeWarning.clear();
    defaultExpressionStrength = 25;
    expressionStrengths = { 25 };
    keyAnchors = { 0 };
    hasManualKeyShiftRange = false;
    manualKeyShiftMinimum = -6;
    manualKeyShiftMaximum = 6;
    chordTimeline.clear();
    tabChordHints.clear();
    phrases.clear();
}

const TabChordHint* SongPackage::getTabChordHint(
    int chordEventIndex) const noexcept
{
    const auto match = std::lower_bound(
        tabChordHints.begin(),
        tabChordHints.end(),
        chordEventIndex,
        [](const TabChordHint& hint, int index)
        {
            return hint.chordEventIndex < index;
        });
    return match != tabChordHints.end()
            && match->chordEventIndex == chordEventIndex
        ? &*match
        : nullptr;
}

int SongPackage::getNearestKeyAnchor(int targetKeyShift) const noexcept
{
    int nearest = baseKeyShift;
    int bestDistance = std::abs(targetKeyShift - nearest);
    for (const int anchor : keyAnchors)
    {
        const int distance = std::abs(targetKeyShift - anchor);
        if (distance < bestDistance
            || (distance == bestDistance && anchor > nearest))
        {
            nearest = anchor;
            bestDistance = distance;
        }
    }
    return nearest;
}

} // namespace mode1
