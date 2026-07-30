#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>
#include <iostream>
#include <vector>

using namespace juce;

namespace
{
    struct BoundaryRow
    {
        int index;
        double startSec;
        double endSec;
    };

    // MidiSplitter가 <곡이름>_boundaries.csv로 남긴 시간 경계를 그대로 읽어서
    // 원본 WAV를 MIDI 조각과 같은 지점에서 자르기 위한 파서
    std::vector<BoundaryRow> readBoundaries (const File& csvFile)
    {
        std::vector<BoundaryRow> rows;
        StringArray lines;
        csvFile.readLines (lines);

        for (auto& line : lines)
        {
            if (line.startsWithIgnoreCase ("part") || line.trim().isEmpty())
                continue;

            auto tokens = StringArray::fromTokens (line, ",", "");
            if (tokens.size() < 3)
                continue;

            rows.push_back ({ tokens[0].getIntValue(), tokens[1].getDoubleValue(), tokens[2].getDoubleValue() });
        }

        return rows;
    }

    bool splitWav (const File& inputWav, const File& boundariesCsv, const File& outputDir)
    {
        AudioFormatManager formatManager;
        formatManager.registerBasicFormats();

        std::unique_ptr<AudioFormatReader> reader (formatManager.createReaderFor (inputWav));
        if (reader == nullptr)
        {
            std::cerr << "WAV 파일을 열 수 없음: " << inputWav.getFullPathName().toStdString() << std::endl;
            return false;
        }

        auto rows = readBoundaries (boundariesCsv);
        if (rows.empty())
        {
            std::cerr << "경계 CSV에서 읽은 구간이 없음: " << boundariesCsv.getFullPathName().toStdString() << std::endl;
            return false;
        }

        outputDir.createDirectory();
        auto baseName = inputWav.getFileNameWithoutExtension();
        auto* wavFormat = formatManager.findFormatForFileExtension ("wav");

        for (auto& row : rows)
        {
            auto startSample = (int64) std::llround (row.startSec * reader->sampleRate);
            auto endSample   = (int64) std::llround (row.endSec   * reader->sampleRate);
            startSample = jlimit ((int64) 0, reader->lengthInSamples, startSample);
            endSample   = jlimit (startSample, reader->lengthInSamples, endSample);
            auto numSamples = (int) (endSample - startSample);

            if (numSamples <= 0)
            {
                std::cerr << "  part" << row.index << ": 구간 길이가 0이라 건너뜀 (원본 오디오가 그 지점까지 없음)" << std::endl;
                continue;
            }

            AudioBuffer<float> buffer ((int) reader->numChannels, numSamples);
            reader->read (&buffer, 0, numSamples, startSample, true, true);

            auto outFile = outputDir.getChildFile (
                baseName + "_part" + String (row.index).paddedLeft ('0', 2) + ".wav");
            outFile.deleteFile();

            std::unique_ptr<OutputStream> outStream (outFile.createOutputStream());
            if (outStream == nullptr)
            {
                std::cerr << "  출력 파일을 열 수 없음: " << outFile.getFullPathName().toStdString() << std::endl;
                continue;
            }

            auto options = AudioFormatWriterOptions{}
                .withSampleRate (reader->sampleRate)
                .withNumChannels ((int) reader->numChannels)
                .withBitsPerSample ((int) reader->bitsPerSample);

            if (auto writer = wavFormat->createWriterFor (outStream, options))
            {
                writer->writeFromAudioSampleBuffer (buffer, 0, numSamples);
                std::cout << "  " << outFile.getFileName().toStdString()
                           << " (" << row.startSec << "s ~ " << row.endSec << "s)" << std::endl;
            }
            else
            {
                std::cerr << "  " << outFile.getFileName().toStdString() << ": writer 생성 실패" << std::endl;
            }
        }

        return true;
    }
}

int main (int argc, char* argv[])
{
    if (argc < 3)
    {
        std::cerr << "사용법: WavSplitter <입력.wav> <boundaries.csv> [출력폴더]" << std::endl;
        std::cerr << "출력폴더를 생략하면 boundaries.csv가 있는 폴더에 씀 (그러면 MIDI 조각들과 같은 폴더에 모임)" << std::endl;
        return 1;
    }

    auto cwd = File::getCurrentWorkingDirectory();
    File inputWav = cwd.getChildFile (argv[1]);
    File boundariesCsv = cwd.getChildFile (argv[2]);
    File outputDir = argc > 3 ? cwd.getChildFile (argv[3]) : boundariesCsv.getParentDirectory();

    if (! inputWav.existsAsFile())
    {
        std::cerr << "WAV 파일이 없음: " << inputWav.getFullPathName().toStdString() << std::endl;
        return 1;
    }
    if (! boundariesCsv.existsAsFile())
    {
        std::cerr << "boundaries.csv 파일이 없음: " << boundariesCsv.getFullPathName().toStdString() << std::endl;
        return 1;
    }

    return splitWav (inputWav, boundariesCsv, outputDir) ? 0 : 1;
}
