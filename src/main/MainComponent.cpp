#include "MainComponent.h"
#include "params/Mode2Params.h"

#include <algorithm>
#include <cmath>

// 입력 채널 RMS 진단 로그. 채널 배선이나 기타 유입을 쫓을 때만 켠다(-DMODE2_DIAGNOSTIC_LOG=1).
#ifndef MODE2_DIAGNOSTIC_LOG
 #define MODE2_DIAGNOSTIC_LOG 0
#endif

namespace
{
    // 8비트 리터럴을 ASCII로 오해하는 juce::String(const char*) 대신 UTF-8로 명시 변환한다.
    juce::String utf8(const char* text)
    {
        return juce::String(juce::CharPointer_UTF8(text));
    }
}

MainComponent::MainComponent()
{
    std::unique_ptr<juce::XmlElement> savedAudioState;
    if (const auto settingsFile = getAudioSettingsFile(); settingsFile.existsAsFile())
        savedAudioState = juce::XmlDocument::parse(settingsFile);

    // 입력은 여유 있게 요청한다. 통합 기기(Aggregate Device)로 오디오 인터페이스와 내장
    // 마이크를 함께 쓰면 입력 채널이 3개 이상이 되고, 어느 채널을 쓸지는 화면에서 고른다.
    setAudioChannels(8, 2, savedAudioState.get());
    deviceManager.addChangeListener(this);

    statusLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(statusLabel);
    updateAudioDeviceStatus();

    mode1Button.onClick = [this] { statusLabel.setText("Mode 1 selected (not implemented)", juce::dontSendNotification); };
    mode2Button.onClick = [this] { showMode2(); };
    addAndMakeVisible(mode1Button);
    addAndMakeVisible(mode2Button);

    audioSettingsButton.setButtonText(utf8("오디오 장치 설정"));
    audioSettingsButton.onClick = [this] { showAudioSettings(); };
    addAndMakeVisible(audioSettingsButton);

    loadChannelMap();
    mode2Screen.onInputChannelsChanged = [this](int guitarChannel, int vocalChannel)
    {
        if (guitarChannel < 0 || vocalChannel < 0)
            return;
        guitarChannelIndex.store(guitarChannel);
        vocalChannelIndex.store(vocalChannel);
        saveChannelMap();
    };
    mode2Screen.onRoomChannelChanged = [this](int roomChannel)
    {
        roomChannelIndex.store(roomChannel < 0 ? -1 : roomChannel);
        saveChannelMap();
    };
    mode2Screen.onToggleRecording = [this] { return toggleRecording(); };
    mode2Screen.onPitchShifterBackendChanged = [this](PitchShifterEngine::Backend backend)
    {
        selectPitchShifterBackend(backend);
    };
    refreshChannelChoices();
    addChildComponent(mode2Screen);

    // Mode2Screen의 컨트롤이 모두 들어가야 한다.
    setSize(620, 860);
}

MainComponent::~MainComponent()
{
    deviceManager.removeChangeListener(this);
    shutdownAudio();
    stopRecording();
    recorderThread.stopThread(2000);
}

void MainComponent::showMode2()
{
    // 이전 캘리브레이션이 게인 상한을 낮게 잠근 상태로 남아 무음처럼 들리는 일을 막는다.
    mode2Controller.resetCalibration();
    updateLatencyInfo();
    mode2Active = true;
    statusLabel.setVisible(false);
    mode1Button.setVisible(false);
    mode2Button.setVisible(false);
    audioSettingsButton.setVisible(false);
    mode2Screen.setVisible(true);
    resized();
}

void MainComponent::showAudioSettings()
{
    auto* selector = new juce::AudioDeviceSelectorComponent(deviceManager, 1, 8, 1, 2, false, false, false, false);
    selector->setSize(500, 450);

    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(selector);
    options.dialogTitle = utf8("오디오 장치 설정");
    options.dialogBackgroundColour = getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId);
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = true;
    options.launchAsync();
}

void MainComponent::updateAudioDeviceStatus()
{
    if (mode2Active)
        return;

    if (auto* device = deviceManager.getCurrentAudioDevice())
    {
        const int numIn = device->getActiveInputChannels().countNumberOfSetBits();
        const int numOut = device->getActiveOutputChannels().countNumberOfSetBits();
        const auto setup = deviceManager.getAudioDeviceSetup();
        statusLabel.setText(utf8("입력 ") + setup.inputDeviceName + " (" + juce::String(numIn) + utf8("채널)  /  출력 ")
                                 + setup.outputDeviceName + " (" + juce::String(numOut) + utf8("채널)"),
                             juce::dontSendNotification);
    }
    else
    {
        statusLabel.setText(utf8("오디오 장치 없음 - 설정을 확인하세요"), juce::dontSendNotification);
    }
}

void MainComponent::changeListenerCallback(juce::ChangeBroadcaster* /*source*/)
{
    updateAudioDeviceStatus();
    saveAudioSettings();
    refreshChannelChoices();
    updateLatencyInfo();
}

void MainComponent::updateLatencyInfo()
{
    auto* device = deviceManager.getCurrentAudioDevice();
    if (device == nullptr)
        return;

    const double sr = device->getCurrentSampleRate();
    if (sr <= 0.0)
        return;

    const double toMs = 1000.0 / sr;
    const double inputMs = static_cast<double>(device->getInputLatencyInSamples()) * toMs;
    const double outputMs = static_cast<double>(device->getOutputLatencyInSamples()) * toMs;
    // 제어 lookahead용 목소리 지연 + 콜백 블록 1개분 + 시프터 고유 지연.
    const double processingMs =
        static_cast<double>(device->getCurrentBufferSizeSamples()) * toMs
        + 1000.0 * mode2::params::vocalControlLookaheadSeconds
        + static_cast<double>(mode2Controller.getPitchShifterLatencySamples()) * toMs;

    mode2Screen.setLatencyInfo(inputMs, outputMs, processingMs);
}

void MainComponent::selectPitchShifterBackend(PitchShifterEngine::Backend backend)
{
    if (! PitchShifterEngine::isBackendAvailable(backend)
        || backend == mode2Controller.getPitchShifterBackend())
        return;

    auto* device = deviceManager.getCurrentAudioDevice();
    if (device == nullptr)
        return;

    // UI 스레드에서 백엔드 객체를 reset하는 동안 오디오 콜백이 같은 객체를 건드리지
    // 못하게 한다. 전환 순간에는 짧은 무음이 생기지만 A/B 중 데이터 레이스나 클릭은 없다.
    {
        const juce::ScopedLock callbackLock(deviceManager.getAudioCallbackLock());
        mode2Controller.setPitchShifterBackend(backend);
        mode2Controller.prepare(device->getCurrentSampleRate(),
                                device->getCurrentBufferSizeSamples());
    }
    updateLatencyInfo();
}

void MainComponent::refreshChannelChoices()
{
    int numInputs = 2;
    if (auto* device = deviceManager.getCurrentAudioDevice())
        numInputs = juce::jmax(1, device->getActiveInputChannels().countNumberOfSetBits());

    const int room = roomChannelIndex.load();
    mode2Screen.setAvailableInputChannels(numInputs,
                                          juce::jlimit(0, numInputs - 1, guitarChannelIndex.load()),
                                          juce::jlimit(0, numInputs - 1, vocalChannelIndex.load()),
                                          room < numInputs ? room : -1);
}

juce::File MainComponent::getAudioSettingsFile()
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("VocalGuitarApp")
        .getChildFile("AudioDeviceSettings.xml");
}

void MainComponent::saveAudioSettings()
{
    if (auto xml = deviceManager.createStateXml())
    {
        const auto file = getAudioSettingsFile();
        file.getParentDirectory().createDirectory();
        xml->writeTo(file);
    }
}

juce::File MainComponent::getChannelMapFile()
{
    return getAudioSettingsFile().getSiblingFile("Mode2ChannelMap.xml");
}

void MainComponent::saveChannelMap()
{
    juce::XmlElement xml("CHANNELMAP");
    xml.setAttribute("guitarChannel", guitarChannelIndex.load());
    xml.setAttribute("vocalChannel", vocalChannelIndex.load());
    xml.setAttribute("roomChannel", roomChannelIndex.load());

    const auto file = getChannelMapFile();
    file.getParentDirectory().createDirectory();
    xml.writeTo(file);
}

void MainComponent::loadChannelMap()
{
    const auto file = getChannelMapFile();
    if (! file.existsAsFile())
        return;

    if (auto xml = juce::XmlDocument::parse(file))
    {
        guitarChannelIndex.store(xml->getIntAttribute("guitarChannel", 0));
        vocalChannelIndex.store(xml->getIntAttribute("vocalChannel", 1));
        roomChannelIndex.store(xml->getIntAttribute("roomChannel", -1));
    }
}

juce::File MainComponent::getRecordingsDirectory()
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("VocalGuitarApp")
        .getChildFile("recordings");
}

void MainComponent::startRecording()
{
    stopRecording();

    const auto dir = getRecordingsDirectory();
    dir.createDirectory();
    recordingFile = dir.getChildFile("mode2_" + juce::Time::getCurrentTime().formatted("%Y%m%d_%H%M%S") + ".wav");
    recordingFile.deleteFile();

    auto stream = std::unique_ptr<juce::FileOutputStream>(recordingFile.createOutputStream());
    if (stream == nullptr)
        return;

    juce::WavAudioFormat wav;
    // ch0 = 기타 입력, ch1 = 목소리 입력(부스트 전), ch2 = 최종 출력.
    // 방 마이크 채널을 골라 두면 ch3 = 스피커 앞에서 받은 소리(앱 출력 + 반주가 섞인 실제 청취음).
    recordingChannelCount = roomChannelIndex.load() >= 0 ? 4 : 3;
    auto* writer = wav.createWriterFor(stream.get(), currentSampleRate, recordingChannelCount, 24, {}, 0);
    if (writer == nullptr)
        return;

    stream.release(); // writer가 소유권을 가져간다.
    threadedWriter.reset(new juce::AudioFormatWriter::ThreadedWriter(writer, recorderThread, 65536));

    // 듣기용 파일: 보정된 목소리 + 4번째 채널(반주)만. 변환 없이 바로 재생된다.
    // 4번째 채널이 없으면 앱 출력만 남는다.
    listenFile = recordingFile.getSiblingFile(recordingFile.getFileNameWithoutExtension()
                                              + "_들어보기.wav");
    listenFile.deleteFile();
    if (auto listenStream = std::unique_ptr<juce::FileOutputStream>(listenFile.createOutputStream()))
    {
        juce::WavAudioFormat listenWav;
        if (auto* lw = listenWav.createWriterFor(listenStream.get(), currentSampleRate, 2, 24, {}, 0))
        {
            listenStream.release();
            listenWriter.reset(new juce::AudioFormatWriter::ThreadedWriter(lw, recorderThread, 65536));
        }
    }

    const juce::ScopedLock sl(writerLock);
    activeWriter = threadedWriter.get();
    activeListenWriter = listenWriter.get();
}

void MainComponent::stopRecording()
{
    {
        const juce::ScopedLock sl(writerLock);
        activeWriter = nullptr;
        activeListenWriter = nullptr;
    }
    threadedWriter.reset();
    listenWriter.reset();
}

bool MainComponent::isRecording() const
{
    return threadedWriter != nullptr;
}

bool MainComponent::toggleRecording()
{
    if (isRecording())
        stopRecording();
    else
        startRecording();

    return isRecording();
}

void MainComponent::prepareToPlay(int samplesPerBlockExpected, double sampleRate)
{
    currentSampleRate = sampleRate;
    if (! recorderThread.isThreadRunning())
        recorderThread.startThread();

    mode2Controller.prepare(sampleRate, samplesPerBlockExpected);
    guitarInputScratch.assign(static_cast<size_t>(samplesPerBlockExpected), 0.0f);
    vocalInputScratch.assign(static_cast<size_t>(samplesPerBlockExpected), 0.0f);
    roomInputScratch.assign(static_cast<size_t>(samplesPerBlockExpected), 0.0f);
    listenLeftScratch.assign(static_cast<size_t>(samplesPerBlockExpected), 0.0f);
    listenRightScratch.assign(static_cast<size_t>(samplesPerBlockExpected), 0.0f);
}

void MainComponent::getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill)
{
    // 주의: 이 버퍼에는 입력 샘플이 담겨 온다. clearActiveBufferRegion()을 먼저 부르면
    // 입력을 읽기 전에 지워버리므로, 처리하지 않는 경우에만 비운다.
    if (!mode2Active || bufferToFill.buffer->getNumChannels() < 2)
    {
        bufferToFill.clearActiveBufferRegion();
        return;
    }

    auto& buffer = *bufferToFill.buffer;
    const int numSamples = bufferToFill.numSamples;
    const int startSample = bufferToFill.startSample;

    // 세 버퍼를 각각 확인한다. 예전에는 기타 버퍼 하나만 보고 셋을 함께 늘렸는데, 방 마이크
    // 버퍼를 prepareToPlay에서 할당하는 것을 빠뜨리자 이 검사가 통과해 버려(기타 버퍼는 이미
    // 충분했다) 크기 0인 벡터에 블록을 복사하고 세그폴트가 났다.
    const auto ensure = [numSamples](std::vector<float>& v)
    {
        if (static_cast<int>(v.size()) < numSamples)
            v.resize(static_cast<size_t>(numSamples), 0.0f);
    };
    ensure(guitarInputScratch);
    ensure(vocalInputScratch);
    ensure(roomInputScratch);

    // 뒤에서 outL/outR로 같은 버퍼 채널에 덮어쓰므로, 입력을 먼저 스크래치로 복사해 둔다.
    const int lastChannel = buffer.getNumChannels() - 1;
    const int guitarChannel = juce::jlimit(0, lastChannel, guitarChannelIndex.load());
    const int vocalChannel = juce::jlimit(0, lastChannel, vocalChannelIndex.load());
    const float* guitarRead = buffer.getReadPointer(guitarChannel, startSample);
    const float* vocalRead = buffer.getReadPointer(vocalChannel, startSample);
    std::copy(guitarRead, guitarRead + numSamples, guitarInputScratch.begin());
    std::copy(vocalRead, vocalRead + numSamples, vocalInputScratch.begin());

    // 방 마이크도 여기서 복사해 둔다. 아래에서 출력이 채널 0-1을 덮어쓰므로, 방 마이크를
    // 그 채널에 물린 경우에도 덮이기 전 값이 남아야 한다.
    const int roomChannel = roomChannelIndex.load();
    const bool haveRoom = roomChannel >= 0 && roomChannel <= lastChannel;
    if (haveRoom)
    {
        const float* roomRead = buffer.getReadPointer(roomChannel, startSample);
        std::copy(roomRead, roomRead + numSamples, roomInputScratch.begin());
    }

#if MODE2_DIAGNOSTIC_LOG
    {
        // 통합기기의 채널 순서가 바뀌었는지, 아니면 마이크가 기타를 줍는지 구분하기 위해
        // 전체 입력 채널의 RMS를 찍는다.
        static int chLogCount = 0;
        if (++chLogCount % 24 == 0)
        {
            juce::String line = "[CH] ";
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            {
                const float* read = buffer.getReadPointer(ch, startSample);
                double ss = 0.0;
                for (int i = 0; i < numSamples; ++i)
                    ss += static_cast<double>(read[i]) * read[i];
                line += "ch" + juce::String(ch) + "=" + juce::String(std::sqrt(ss / numSamples), 5) + "  ";
            }
            line += "(기타=ch" + juce::String(guitarChannel) + " 목소리=ch" + juce::String(vocalChannel) + ")";
            juce::Logger::writeToLog(line);
        }
    }
#endif

    float* outL = buffer.getWritePointer(0, startSample);
    float* outR = buffer.getWritePointer(1, startSample);

    mode2Controller.processBlock(guitarInputScratch.data(), vocalInputScratch.data(), outL, outR, numSamples);

    {
        // 진단 녹음: 기타 입력 / 목소리 입력 / 최종 출력을 한 파일에 남긴다.
        // ThreadedWriter::write는 락 없는 FIFO에 밀어넣기만 하므로 오디오 스레드에서 안전하다.
        const juce::ScopedTryLock stl(writerLock);
        if (stl.isLocked() && activeWriter != nullptr)
        {
            // 채널 수는 녹음을 시작할 때 정해졌다. 도중에 방 마이크 선택이 바뀌어도
            // writer가 기대하는 개수를 그대로 유지해야 한다(무음으로 채운다).
            const float* channels[4] = { guitarInputScratch.data(), vocalInputScratch.data(), outL,
                                         haveRoom ? roomInputScratch.data() : nullptr };
            if (recordingChannelCount == 4 && channels[3] == nullptr)
            {
                std::fill(roomInputScratch.begin(), roomInputScratch.begin() + numSamples, 0.0f);
                channels[3] = roomInputScratch.data();
            }
            activeWriter->write(channels, numSamples);

            // 듣기 파일: 보정된 목소리 + 반주만 섞는다. 기타 라인과 목소리 원본은 넣지 않는다.
            // 0.8은 둘을 더할 때 피크가 1을 넘지 않게 하는 여유다.
            if (activeListenWriter != nullptr)
            {
                const float* extra = recordingChannelCount == 4 ? channels[3] : nullptr;
                for (int i = 0; i < numSamples; ++i)
                {
                    const float mixed = 0.8f * outL[i] + (extra != nullptr ? 0.8f * extra[i] : 0.0f);
                    listenLeftScratch[static_cast<size_t>(i)] = mixed;
                    listenRightScratch[static_cast<size_t>(i)] = mixed;
                }
                const float* listenChannels[2] = { listenLeftScratch.data(),
                                                   listenRightScratch.data() };
                activeListenWriter->write(listenChannels, numSamples);
            }
        }
    }
}

void MainComponent::releaseResources()
{
    stopRecording();
    mode2Controller.reset();
}

void MainComponent::paint(juce::Graphics& g)
{
    g.fillAll(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));
}

void MainComponent::resized()
{
    auto area = getLocalBounds();

    if (mode2Active)
    {
        mode2Screen.setBounds(area);
        return;
    }

    area = area.reduced(20);
    statusLabel.setBounds(area.removeFromTop(40));
    area.removeFromTop(20);
    mode1Button.setBounds(area.removeFromTop(40));
    area.removeFromTop(10);
    mode2Button.setBounds(area.removeFromTop(40));
    area.removeFromTop(20);
    audioSettingsButton.setBounds(area.removeFromTop(36));
}
