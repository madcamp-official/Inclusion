#include "SongPackage.h"

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
        const auto vocalDirectoryName = vocalObject->hasProperty("directory")
            ? stringProperty(vocalObject, "directory")
            : juce::String("vocals");
        phrase.vocalFile = packageDirectory
            .getChildFile(vocalDirectoryName)
            .getChildFile(stringProperty(vocalObject, "file"));

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
    return true;
}

void SongPackage::clear()
{
    sourceFile = {};
    songName.clear();
    scoreBpm = 0.0;
    baseKeyShift = 0;
    rangeWarning.clear();
    phrases.clear();
}

} // namespace mode1
