#include <juce_gui_extra/juce_gui_extra.h>
#include "MainComponent.h"
#include "ui/GuitaruLookAndFeel.h"

class VocalGuitarApplication : public juce::JUCEApplication
{
public:
    VocalGuitarApplication() = default;

    const juce::String getApplicationName() override    { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override           { return true; }

    void initialise(const juce::String&) override
    {
        // 앱 전체(오디오 장치 설정 창, 경고창 포함)를 크레용 스타일로 통일한다.
        // GuitaruLookAndFeel이 쓰는 Gaegu는 한글 글리프를 갖고 있어, JUCE 기본
        // 폰트가 플랫폼에 따라 라틴 전용 서체를 고르는 문제도 함께 해결된다.
        juce::LookAndFeel::setDefaultLookAndFeel(&lookAndFeel);
        mainWindow.reset(new MainWindow(getApplicationName()));
    }

    void shutdown() override
    {
        mainWindow = nullptr;
        juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
    }

    void systemRequestedQuit() override
    {
        quit();
    }

    class MainWindow : public juce::DocumentWindow
    {
    public:
        explicit MainWindow(const juce::String& name)
            : DocumentWindow(name,
                              juce::Desktop::getInstance().getDefaultLookAndFeel()
                                  .findColour(juce::ResizableWindow::backgroundColourId),
                              DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar(true);
            setContentOwned(new MainComponent(), true);

            setResizable(true, true);
            centreWithSize(getWidth(), getHeight());
            setVisible(true);
        }

        void closeButtonPressed() override
        {
            JUCEApplication::getInstance()->systemRequestedQuit();
        }
    };

private:
    GuitaruLookAndFeel lookAndFeel;
    std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION(VocalGuitarApplication)
