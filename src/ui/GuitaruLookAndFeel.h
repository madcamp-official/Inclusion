#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

namespace guitaru
{
    inline juce::Colour paper()      { return juce::Colour(0xfffaf7ef); }
    inline juce::Colour paperDark()  { return juce::Colour(0xfff0eadc); }
    inline juce::Colour green()      { return juce::Colour(0xff18572e); }
    inline juce::Colour blue()       { return juce::Colour(0xff2489e8); }
    inline juce::Colour bluePale()   { return juce::Colour(0xffdceeff); }
    inline juce::Colour red()        { return juce::Colour(0xffee554d); }
    inline juce::Colour inkMuted()   { return juce::Colour(0xff66806d); }

    juce::Font chalkFont(float height, bool bold = false);
    void drawPaperTexture(juce::Graphics&, juce::Rectangle<int>, int seed = 730);
    void drawChalkLine(juce::Graphics&, juce::Point<float> start, juce::Point<float> end,
                       juce::Colour, float width, int seed);
    void drawChalkFill(juce::Graphics&, juce::Rectangle<float>, juce::Colour,
                       float cornerSize, int seed);
    void drawCrayonCard(juce::Graphics&, juce::Rectangle<float>, float cornerSize = 20.0f);
}

class GuitaruLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    GuitaruLookAndFeel();

    juce::Font getLabelFont(juce::Label&) override;
    juce::Font getTextButtonFont(juce::TextButton&, int buttonHeight) override;
    juce::Font getComboBoxFont(juce::ComboBox&) override;

    void drawLabel(juce::Graphics&, juce::Label&) override;
    void drawButtonBackground(juce::Graphics&, juce::Button&, const juce::Colour&,
                              bool isMouseOverButton, bool isButtonDown) override;
    void drawButtonText(juce::Graphics&, juce::TextButton&,
                        bool isMouseOverButton, bool isButtonDown) override;
    void drawComboBox(juce::Graphics&, int width, int height, bool isButtonDown,
                      int buttonX, int buttonY, int buttonW, int buttonH,
                      juce::ComboBox&) override;
    void positionComboBoxText(juce::ComboBox&, juce::Label&) override;
    void drawLinearSlider(juce::Graphics&, int x, int y, int width, int height,
                          float sliderPos, float minSliderPos, float maxSliderPos,
                          juce::Slider::SliderStyle, juce::Slider&) override;
    void drawProgressBar(juce::Graphics&, juce::ProgressBar&, int width, int height,
                         double progress, const juce::String& textToShow) override;
    void drawToggleButton(juce::Graphics&, juce::ToggleButton&,
                          bool isMouseOverButton, bool isButtonDown) override;
};
