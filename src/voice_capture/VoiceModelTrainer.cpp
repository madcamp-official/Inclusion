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
                    setStatus(label + L": " + line);
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
    const auto exitCode = process.getExitCode();
    activeProcess = nullptr;

    if (exitCode != 0)
    {
        setStatus(label + L" 실패 (종료 코드 " + juce::String(exitCode) + ")");
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
    const auto combinedWav =
        config.profileDirectory.getChildFile("training_combined.wav");

    const juce::StringArray prepareArgs {
        pythonExe,
        prepareScript.getFullPathName(),
        "--profile-dir",
        config.profileDirectory.getFullPathName(),
        "--output",
        combinedWav.getFullPathName(),
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

    succeeded.store(runStep(L"GPU 재학습", trainArgs));
    finished.store(true);
}

} // namespace voice_capture
