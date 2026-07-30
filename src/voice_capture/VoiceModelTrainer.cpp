#include "VoiceModelTrainer.h"

namespace voice_capture
{

VoiceModelTrainer::VoiceModelTrainer() : juce::Thread("VoiceModelTrainer") {}

VoiceModelTrainer::~VoiceModelTrainer()
{
    cancel();
    stopThread(4000);
}

void VoiceModelTrainer::start(Config newConfig)
{
    if (isThreadRunning())
        return;

    config = std::move(newConfig);
    finished.store(false);
    succeeded.store(false);
    cancelRequested.store(false);
    newConfig.profileDirectory.getChildFile("training.log").deleteFile();
    setStatus(L"준비 중...");
    startThread();
}

void VoiceModelTrainer::cancel()
{
    cancelRequested.store(true);
    if (activeProcess != nullptr)
        activeProcess->kill();
    signalThreadShouldExit();
}

juce::String VoiceModelTrainer::getLatestStatusLine() const
{
    const juce::SpinLock::ScopedLockType lock(statusLock);
    return statusLine;
}

void VoiceModelTrainer::setStatus(const juce::String& text)
{
    const juce::SpinLock::ScopedLockType lock(statusLock);
    statusLine = text;
}

bool VoiceModelTrainer::runStep(
    const juce::String& label,
    const juce::StringArray& commandLine)
{
    if (cancelRequested.load())
        return false;

    setStatus(label + L" 시작...");

    juce::ChildProcess process;
    activeProcess = &process;
    if (!process.start(commandLine))
    {
        activeProcess = nullptr;
        setStatus(label + L" 실행 실패: 프로세스를 시작할 수 없습니다.");
        return false;
    }

    juce::String buffer;
    juce::String lastOutputLine;
    char chunk[512];
    while (process.isRunning() && !cancelRequested.load())
    {
        const int bytesRead = process.readProcessOutput(chunk, sizeof(chunk) - 1);
        if (bytesRead > 0)
        {
            buffer += juce::String::fromUTF8(chunk, bytesRead);
            int newlineIndex;
            while ((newlineIndex = buffer.indexOfChar('\n')) >= 0)
            {
                const auto line = buffer.substring(0, newlineIndex).trim();
                buffer = buffer.substring(newlineIndex + 1);
                if (line.isNotEmpty())
                {
                    lastOutputLine = line;
                    config.profileDirectory.getChildFile("training.log").appendText(
                        label + ": " + line + "\n");
                    setStatus(label + L": " + line);
                }
            }
        }
        else
        {
            juce::Thread::sleep(80);
        }
    }

    if (cancelRequested.load())
    {
        process.kill();
        activeProcess = nullptr;
        setStatus(label + L" 취소됨");
        return false;
    }

    process.waitForProcessToFinish(5000);
    while (true)
    {
        const int bytesRead =
            process.readProcessOutput(chunk, sizeof(chunk) - 1);
        if (bytesRead <= 0)
            break;
        buffer += juce::String::fromUTF8(chunk, bytesRead);
    }
    const auto trailingLine = buffer.trim();
    if (trailingLine.isNotEmpty())
    {
        lastOutputLine = trailingLine;
        config.profileDirectory.getChildFile("training.log").appendText(
            label + ": " + trailingLine + "\n");
    }
    const auto exitCode = process.getExitCode();
    activeProcess = nullptr;

    if (exitCode != 0)
    {
        setStatus(
            label + L" 실패 (종료 코드 " + juce::String(exitCode) + L")"
                + (lastOutputLine.isNotEmpty()
                    ? L" · " + lastOutputLine
                    : L" · 자세한 출력이 없습니다."));
        return false;
    }

    setStatus(label + L" 완료");
    return true;
}

void VoiceModelTrainer::run()
{
    const auto pythonExe = config.pythonExecutable.existsAsFile()
        ? config.pythonExecutable.getFullPathName()
        : juce::String("python");

    const auto prepareScript = config.repositoryRoot.getChildFile(
        "tools/mode1_song_package/prepare_voice_training_dataset.py");
    const auto trainScript = config.repositoryRoot.getChildFile(
        "tools/mode1_song_package/train_rvc_voice.py");
    const auto refreshScript = config.repositoryRoot.getChildFile(
        "tools/mode1_song_package/refresh_song_package_after_training.py");
    const auto combinedWav =
        config.profileDirectory.getChildFile("training_combined.wav");

    if (!prepareScript.existsAsFile()
        || !trainScript.existsAsFile()
        || !refreshScript.existsAsFile())
    {
        setStatus(L"재학습 도구를 찾지 못했습니다. 저장소 위치를 확인해 주세요.");
        finished.store(true);
        return;
    }

    const auto acceptedFiles =
        config.profileDirectory.getChildFile("accepted").findChildFiles(
            juce::File::findFiles, false, "*.wav");
    if (acceptedFiles.isEmpty())
    {
        setStatus(
            L"재학습할 통과 클립이 없습니다. 품질검사를 통과한 녹음이 필요합니다.");
        finished.store(true);
        return;
    }

    const juce::StringArray prepareArgs {
        pythonExe,
        prepareScript.getFullPathName(),
        "--profile-dir",
        config.profileDirectory.getFullPathName(),
        "--output",
        combinedWav.getFullPathName(),
        "--minimum-duration-seconds",
        "180",
    };

    if (!runStep(L"데이터 준비", prepareArgs))
    {
        finished.store(true);
        return;
    }

    const juce::StringArray trainArgs {
        pythonExe,
        trainScript.getFullPathName(),
        "--host",
        config.sshHost,
        "--voice-wav",
        combinedWav.getFullPathName(),
        "--experiment-name",
        config.experimentName,
    };

    if (!runStep(L"GPU 재학습", trainArgs))
    {
        finished.store(true);
        return;
    }

    if (!config.songPackage.existsAsFile())
    {
        setStatus(
            L"재학습은 완료됐지만 자동 변환할 song_package.json을 찾지 못했습니다.");
        finished.store(true);
        return;
    }

    const auto refreshResult =
        config.profileDirectory.getChildFile("song_refresh_result.json");
    const juce::StringArray refreshArgs {
        pythonExe,
        refreshScript.getFullPathName(),
        "--host",
        config.sshHost,
        "--experiment-name",
        config.experimentName,
        "--song-package",
        config.songPackage.getFullPathName(),
        "--user-voice",
        combinedWav.getFullPathName(),
        "--result-json",
        refreshResult.getFullPathName(),
    };
    succeeded.store(runStep(L"새 목소리로 곡 자동 변환", refreshArgs));
    finished.store(true);
}

} // namespace voice_capture
