#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>

#include <iostream>
#include <optional>
#include <vector>

using namespace juce;

namespace
{
    // SynthV 등 보이스 신스가 트랙당 처리 가능한 노트 수(약 40개)보다 여유 있게 낮춘 기본값
    constexpr int defaultMaxNotesPerChunk = 35;

    struct ChunkMeta
    {
        std::optional<MidiMessage> tempo;
        std::optional<MidiMessage> timeSig;
    };

    struct TempoPoint
    {
        double tick;
        double secondsPerQuarterNote;
    };

    // WavSplitter가 같은 지점에서 원본 WAV를 자를 수 있도록, 조각별 시간 경계를 초 단위로 넘겨주기 위한 정보
    struct ChunkBounds
    {
        int index;
        double startTick;
        double endTick;
        int noteCount;
    };

    std::vector<TempoPoint> buildTempoMap (const MidiFile& midiFile, short timeFormat)
    {
        MidiMessageSequence tempoEvents;
        midiFile.findAllTempoEvents (tempoEvents);

        std::vector<TempoPoint> map;
        for (int i = 0; i < tempoEvents.getNumEvents(); ++i)
        {
            const auto& m = tempoEvents.getEventPointer (i)->message;
            map.push_back ({ m.getTimeStamp(), m.getTempoSecondsPerQuarterNote() });
        }

        if (map.empty() || map.front().tick > 0.0)
            map.insert (map.begin(), { 0.0, 0.5 }); // 템포 정보가 없으면 120bpm으로 가정

        (void) timeFormat;
        return map;
    }

    double ticksToSeconds (double tick, const std::vector<TempoPoint>& tempoMap, int ticksPerQuarterNote)
    {
        double seconds = 0.0;

        for (size_t i = 0; i < tempoMap.size(); ++i)
        {
            auto segStart = tempoMap[i].tick;
            auto segEnd = (i + 1 < tempoMap.size()) ? tempoMap[i + 1].tick : tick;
            auto effectiveEnd = jmin (segEnd, tick);

            if (effectiveEnd > segStart)
                seconds += (effectiveEnd - segStart) / (double) ticksPerQuarterNote * tempoMap[i].secondsPerQuarterNote;

            if (tick <= segEnd)
                break;
        }

        return seconds;
    }

    void writeChunk (const File& songOutputDir, const String& baseName, int chunkIndex,
                      short timeFormat, MidiMessageSequence& chunk, int noteCount)
    {
        if (chunk.getNumEvents() == 0)
            return;

        MidiFile out;
        out.setTicksPerQuarterNote (timeFormat);
        out.addTrack (chunk);

        auto outFile = songOutputDir.getChildFile (
            baseName + "_part" + String (chunkIndex).paddedLeft ('0', 2) + ".mid");

        outFile.deleteFile();
        if (auto outStream = outFile.createOutputStream())
        {
            out.writeTo (*outStream);
            std::cout << "  " << outFile.getFileName().toStdString()
                       << " (" << noteCount << " notes)" << std::endl;
        }
        else
        {
            std::cerr << "  출력 파일을 열 수 없음: " << outFile.getFullPathName().toStdString() << std::endl;
        }
    }

    void writeBoundariesCsv (const File& songOutputDir, const String& baseName,
                              const std::vector<ChunkBounds>& chunks,
                              const std::vector<TempoPoint>& tempoMap, int ticksPerQuarterNote)
    {
        String csv = "part,startSec,endSec,noteCount\n";
        for (auto& c : chunks)
        {
            auto startSec = ticksToSeconds (c.startTick, tempoMap, ticksPerQuarterNote);
            auto endSec = ticksToSeconds (c.endTick, tempoMap, ticksPerQuarterNote);
            csv << c.index << "," << String (startSec, 6) << "," << String (endSec, 6) << "," << c.noteCount << "\n";
        }

        auto outFile = songOutputDir.getChildFile (baseName + "_boundaries.csv");
        outFile.deleteFile();
        outFile.appendText (csv);
        std::cout << "  " << outFile.getFileName().toStdString() << " (WavSplitter용 시간 경계)" << std::endl;
    }

    bool splitOneFile (const File& inputFile, const File& outputDir, int maxNotesPerChunk)
    {
        auto inputStream = inputFile.createInputStream();
        if (inputStream == nullptr)
        {
            std::cerr << "  열 수 없음: " << inputFile.getFullPathName().toStdString() << std::endl;
            return false;
        }

        MidiFile midiFile;
        if (! midiFile.readFrom (*inputStream))
        {
            std::cerr << "  MIDI 파싱 실패: " << inputFile.getFullPathName().toStdString() << std::endl;
            return false;
        }

        auto timeFormat = midiFile.getTimeFormat();
        if (timeFormat <= 0)
        {
            std::cerr << "  SMPTE 타임코드 MIDI는 지원하지 않음: " << inputFile.getFullPathName().toStdString() << std::endl;
            return false;
        }

        auto tempoMap = buildTempoMap (midiFile, timeFormat);

        MidiMessageSequence merged;
        for (int t = 0; t < midiFile.getNumTracks(); ++t)
            merged.addSequence (*midiFile.getTrack (t), 0.0);
        merged.sort();

        auto baseName = inputFile.getFileNameWithoutExtension();
        auto songOutputDir = outputDir.getChildFile (baseName);
        songOutputDir.createDirectory();

        MidiMessageSequence currentChunk;
        int notesInChunk = 0;
        int chunkIndex = 1;
        double chunkStartTime = 0.0;
        ChunkMeta carry;
        std::vector<ChunkBounds> chunkBounds;

        for (int i = 0; i < merged.getNumEvents(); ++i)
        {
            const auto& message = merged.getEventPointer (i)->message;

            if (message.isEndOfTrackMetaEvent())
                continue; // writeTo()가 각 조각 끝에 알아서 붙여준다

            if (message.isTempoMetaEvent())
                carry.tempo = message;
            else if (message.isTimeSignatureMetaEvent())
                carry.timeSig = message;

            if (message.isNoteOn())
            {
                if (notesInChunk >= maxNotesPerChunk)
                {
                    chunkBounds.push_back ({ chunkIndex, chunkStartTime, message.getTimeStamp(), notesInChunk });
                    writeChunk (songOutputDir, baseName, chunkIndex, timeFormat, currentChunk, notesInChunk);
                    currentChunk.clear();
                    ++chunkIndex;
                    notesInChunk = 0;
                    chunkStartTime = message.getTimeStamp();

                    // 이전 조각에서 정해진 템포/박자를 새 조각 맨 앞(0틱)에 이식해야
                    // 각 조각을 SynthV 등에서 단독으로 열어도 재생 속도가 맞다
                    if (carry.tempo.has_value())
                    {
                        auto tempoCopy = *carry.tempo;
                        tempoCopy.setTimeStamp (0.0);
                        currentChunk.addEvent (tempoCopy);
                    }
                    if (carry.timeSig.has_value())
                    {
                        auto timeSigCopy = *carry.timeSig;
                        timeSigCopy.setTimeStamp (0.0);
                        currentChunk.addEvent (timeSigCopy);
                    }
                }

                ++notesInChunk;
            }

            auto rebased = message;
            rebased.setTimeStamp (jmax (0.0, message.getTimeStamp() - chunkStartTime));
            currentChunk.addEvent (rebased);
        }

        auto finalEndTick = jmax (chunkStartTime, merged.getEndTime());
        chunkBounds.push_back ({ chunkIndex, chunkStartTime, finalEndTick, notesInChunk });
        writeChunk (songOutputDir, baseName, chunkIndex, timeFormat, currentChunk, notesInChunk);

        writeBoundariesCsv (songOutputDir, baseName, chunkBounds, tempoMap, timeFormat);

        return true;
    }
}

int main (int argc, char* argv[])
{
    auto cwd = File::getCurrentWorkingDirectory();
    File inputDir  = argc > 1 ? cwd.getChildFile (argv[1]) : cwd.getChildFile ("assets/midi_to_split");
    File outputDir = argc > 2 ? cwd.getChildFile (argv[2]) : inputDir.getChildFile ("output");
    int maxNotes   = argc > 3 ? String (argv[3]).getIntValue() : defaultMaxNotesPerChunk;

    if (maxNotes <= 0)
        maxNotes = defaultMaxNotesPerChunk;

    if (! inputDir.isDirectory())
    {
        std::cerr << "입력 폴더가 없음: " << inputDir.getFullPathName().toStdString() << std::endl;
        std::cerr << "사용법: MidiSplitter [입력폴더] [출력폴더] [조각당 최대 노트 수]" << std::endl;
        return 1;
    }

    outputDir.createDirectory();

    auto midiFiles = inputDir.findChildFiles (File::findFiles, false, "*.mid;*.midi");

    if (midiFiles.isEmpty())
    {
        std::cout << inputDir.getFullPathName().toStdString() << " 에 .mid 파일이 없습니다." << std::endl;
        return 0;
    }

    std::cout << "조각당 최대 " << maxNotes << "개 노트로 분할합니다." << std::endl;

    for (auto& file : midiFiles)
    {
        std::cout << file.getFileName().toStdString() << ":" << std::endl;
        splitOneFile (file, outputDir, maxNotes);
    }

    return 0;
}
