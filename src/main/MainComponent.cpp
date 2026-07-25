#include "MainComponent.h"

MainComponent::MainComponent()
{
    setAudioChannels(1, 2); // 마이크 입력 1채널, 스피커 출력 2채널

    statusLabel.setText("Skeleton — no DSP wired yet", juce::dontSendNotification);
    statusLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(statusLabel);

    mode1Button.onClick = [this] { statusLabel.setText("Mode 1 selected (not implemented)", juce::dontSendNotification); };
    mode2Button.onClick = [this] { statusLabel.setText("Mode 2 selected (not implemented)", juce::dontSendNotification); };
    addAndMakeVisible(mode1Button);
    addAndMakeVisible(mode2Button);

    setSize(600, 400);
}

MainComponent::~MainComponent()
{
    shutdownAudio();
}

void MainComponent::prepareToPlay(int /*samplesPerBlockExpected*/, double /*sampleRate*/)
{
    // TODO: mode1/mode2 컨트롤러 prepareToPlay 호출
}

void MainComponent::getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill)
{
    bufferToFill.clearActiveBufferRegion(); // TODO: 선택된 모드 컨트롤러로 위임
}

void MainComponent::releaseResources()
{
    // TODO: mode1/mode2 컨트롤러 releaseResources 호출
}

void MainComponent::paint(juce::Graphics& g)
{
    g.fillAll(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));
}

void MainComponent::resized()
{
    auto area = getLocalBounds().reduced(20);
    statusLabel.setBounds(area.removeFromTop(40));
    area.removeFromTop(20);
    mode1Button.setBounds(area.removeFromTop(40));
    area.removeFromTop(10);
    mode2Button.setBounds(area.removeFromTop(40));
}
