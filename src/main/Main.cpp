#include <juce_gui_extra/juce_gui_extra.h>
#include "MainComponent.h"

class VocalGuitarApplication : public juce::JUCEApplication
{
public:
    VocalGuitarApplication() = default;

    const juce::String getApplicationName() override    { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override           { return true; }

    void initialise(const juce::String&) override
    {
        // JUCE's platform default may select a Latin-only face on Windows.
        // Malgun Gothic contains the complete Korean glyph set and is bundled
        // with supported Windows versions.
        lookAndFeel.setDefaultSansSerifTypefaceName("Malgun Gothic");
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
    juce::LookAndFeel_V4 lookAndFeel;
    std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION(VocalGuitarApplication)
