#include "MainComponent.h"
#include "params/Mode2Params.h"

#include <algorithm>
#include <cmath>

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
    refreshChannelChoices();
    addChildComponent(mode2Screen);

    setSize(620, 740);
}

MainComponent::~MainComponent()
{
    deviceManager.removeChangeListener(this);
    shutdownAudio();
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
    // 우리가 의도적으로 넣은 목소리 지연(5절 장치 ①) + 콜백 블록 1개분.
    const double processingMs = static_cast<double>(device->getCurrentBufferSizeSamples())
                               * (1 + mode2::params::vocalArtificialDelayBlocks) * toMs;

    mode2Screen.setLatencyInfo(inputMs, outputMs, processingMs);
}

void MainComponent::refreshChannelChoices()
{
    int numInputs = 2;
    if (auto* device = deviceManager.getCurrentAudioDevice())
        numInputs = juce::jmax(1, device->getActiveInputChannels().countNumberOfSetBits());

    mode2Screen.setAvailableInputChannels(numInputs,
                                          juce::jlimit(0, numInputs - 1, guitarChannelIndex.load()),
                                          juce::jlimit(0, numInputs - 1, vocalChannelIndex.load()));
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
    }
}

void MainComponent::prepareToPlay(int samplesPerBlockExpected, double sampleRate)
{
    mode2Controller.prepare(sampleRate, samplesPerBlockExpected);
    guitarInputScratch.assign(static_cast<size_t>(samplesPerBlockExpected), 0.0f);
    vocalInputScratch.assign(static_cast<size_t>(samplesPerBlockExpected), 0.0f);
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

    if (static_cast<int>(guitarInputScratch.size()) < numSamples)
    {
        guitarInputScratch.resize(static_cast<size_t>(numSamples));
        vocalInputScratch.resize(static_cast<size_t>(numSamples));
    }

    // 뒤에서 outL/outR로 같은 버퍼 채널에 덮어쓰므로, 입력을 먼저 스크래치로 복사해 둔다.
    const int lastChannel = buffer.getNumChannels() - 1;
    const int guitarChannel = juce::jlimit(0, lastChannel, guitarChannelIndex.load());
    const int vocalChannel = juce::jlimit(0, lastChannel, vocalChannelIndex.load());
    const float* guitarRead = buffer.getReadPointer(guitarChannel, startSample);
    const float* vocalRead = buffer.getReadPointer(vocalChannel, startSample);
    std::copy(guitarRead, guitarRead + numSamples, guitarInputScratch.begin());
    std::copy(vocalRead, vocalRead + numSamples, vocalInputScratch.begin());

    {
        static int callCount = 0;
        if (++callCount % 50 == 0)
        {
            float guitarPeak = 0.0f, vocalPeak = 0.0f;
            for (int i = 0; i < numSamples; ++i)
            {
                guitarPeak = std::max(guitarPeak, std::abs(guitarRead[i]));
                vocalPeak = std::max(vocalPeak, std::abs(vocalRead[i]));
            }
            juce::Logger::writeToLog("[DBG] guitarPeak=" + juce::String(guitarPeak, 5) + " vocalPeak=" + juce::String(vocalPeak, 5));
        }
    }

    float* outL = buffer.getWritePointer(0, startSample);
    float* outR = buffer.getWritePointer(1, startSample);

    mode2Controller.processBlock(guitarInputScratch.data(), vocalInputScratch.data(), outL, outR, numSamples);
}

void MainComponent::releaseResources()
{
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
