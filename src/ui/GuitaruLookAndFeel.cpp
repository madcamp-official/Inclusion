#include "GuitaruLookAndFeel.h"
#include "BinaryData.h"

namespace
{
    juce::Font roundedFont(float height, bool bold = false)
    {
        return guitaru::chalkFont(height, bold);
    }

    void drawWobblyRoundedOutline(juce::Graphics& g, juce::Rectangle<float> bounds,
                                  float radius, juce::Colour colour, float thickness)
    {
        g.setColour(colour.withAlpha(0.72f));
        g.drawRoundedRectangle(bounds, radius, thickness);
        g.setColour(colour.withAlpha(0.38f));
        g.drawRoundedRectangle(bounds.translated(0.9f, -0.7f).reduced(1.0f),
                               radius + 0.8f, juce::jmax(0.8f, thickness * 0.55f));
        g.setColour(colour.withAlpha(0.24f));
        g.drawRoundedRectangle(bounds.translated(-0.6f, 0.8f).reduced(1.8f),
                               radius - 0.5f, juce::jmax(0.7f, thickness * 0.42f));
    }

    void drawSoftChalkTrack(juce::Graphics& g, float startX, float endX, float centreY,
                            juce::Colour colour, float thickness, int seed)
    {
        if (endX <= startX)
            return;

        juce::Path core;
        core.startNewSubPath(startX, centreY);
        core.lineTo(endX, centreY);
        g.setColour(colour.withAlpha(0.68f));
        g.strokePath(core, juce::PathStrokeType(thickness,
                                                juce::PathStrokeType::curved,
                                                juce::PathStrokeType::rounded));

        // 외곽은 연속된 얇은 가루층으로 잡아 선 모양이 너덜너덜해지지 않게 한다.
        for (const float offset : { -0.9f, 0.9f })
        {
            juce::Path edge;
            edge.startNewSubPath(startX + 1.0f, centreY + offset);
            edge.lineTo(endX - 1.0f, centreY + offset);
            g.setColour(colour.withAlpha(0.20f));
            g.strokePath(edge, juce::PathStrokeType(0.9f,
                                                    juce::PathStrokeType::curved,
                                                    juce::PathStrokeType::rounded));
        }

        // 굵은 긁힘 대신 작은 밝고 어두운 입자를 촘촘히 섞어 고운 분필 결을 만든다.
        juce::Random random(seed);
        const float length = endX - startX;
        const int marks = juce::jmax(36, juce::roundToInt(length * 0.90f));
        for (int i = 0; i < marks; ++i)
        {
            const float markX = startX + random.nextFloat() * length;
            const float jitterY = (random.nextFloat() - 0.5f) * thickness * 0.72f;
            const float markLength = 0.8f + random.nextFloat() * 2.2f;
            const auto grain = random.nextBool() ? guitaru::paper()
                                                  : colour.darker(0.32f);
            g.setColour(grain.withAlpha(0.28f + random.nextFloat() * 0.22f));
            g.drawLine(markX, centreY + jitterY,
                       juce::jmin(endX, markX + markLength), centreY + jitterY,
                       0.55f + random.nextFloat() * 0.42f);
        }
    }
}

namespace guitaru
{
    juce::Font chalkFont(float height, bool)
    {
        static const auto typeface = juce::Typeface::createSystemTypefaceFor(
            BinaryData::GaeguBold_ttf, BinaryData::GaeguBold_ttfSize);
        return juce::Font(juce::FontOptions(typeface)
                              .withHeight(height)
                              .withKerningFactor(0.018f));
    }

    void drawPaperTexture(juce::Graphics& g, juce::Rectangle<int> bounds, int seed)
    {
        g.fillAll(paper());
        juce::Random random(seed);
        for (int i = 0; i < bounds.getWidth() * bounds.getHeight() / 520; ++i)
        {
            const float x = static_cast<float>(bounds.getX() + random.nextInt(juce::jmax(1, bounds.getWidth())));
            const float y = static_cast<float>(bounds.getY() + random.nextInt(juce::jmax(1, bounds.getHeight())));
            const float alpha = 0.018f + random.nextFloat() * 0.038f;
            g.setColour((random.nextBool() ? green() : juce::Colours::saddlebrown).withAlpha(alpha));
            if (random.nextBool())
                g.fillEllipse(x, y, 0.6f + random.nextFloat() * 1.5f,
                              0.6f + random.nextFloat() * 1.5f);
            else
                g.drawLine(x, y, x + 1.0f + random.nextFloat() * 3.0f,
                           y + random.nextFloat() * 1.5f, 0.55f);
        }
    }

    void drawChalkLine(juce::Graphics& g, juce::Point<float> start, juce::Point<float> end,
                       juce::Colour colour, float width, int seed)
    {
        const auto delta = end - start;
        const float length = delta.getDistanceFromOrigin();
        if (length < 0.5f)
            return;

        const auto direction = delta / length;
        const juce::Point<float> normal(-direction.y, direction.x);
        g.setColour(colour.withAlpha(0.26f));
        g.drawLine({ start, end }, width * 1.15f);

        juce::Random random(seed);
        const int marks = juce::jmax(12, juce::roundToInt(length * 0.72f));
        for (int i = 0; i < marks; ++i)
        {
            const float t = random.nextFloat();
            const float jitter = (random.nextFloat() - 0.5f) * width * 1.25f;
            const float markLength = 0.8f + random.nextFloat() * 4.2f;
            const auto p = start + direction * (t * length) + normal * jitter;
            g.setColour(colour.withAlpha(0.18f + random.nextFloat() * 0.48f));
            g.drawLine({ p, p + direction * markLength },
                       0.55f + random.nextFloat() * juce::jmax(0.7f, width * 0.48f));
        }
    }

    void drawChalkFill(juce::Graphics& g, juce::Rectangle<float> bounds, juce::Colour colour,
                       float cornerSize, int seed)
    {
        g.setColour(colour.withAlpha(0.86f));
        g.fillRoundedRectangle(bounds, cornerSize);

        juce::Path clip;
        clip.addRoundedRectangle(bounds, cornerSize);
        g.saveState();
        g.reduceClipRegion(clip);

        juce::Random random(seed);
        const int marks = juce::jlimit(80, 1200,
                                      juce::roundToInt(bounds.getWidth() * bounds.getHeight() / 24.0f));
        for (int i = 0; i < marks; ++i)
        {
            const float x = bounds.getX() + random.nextFloat() * bounds.getWidth();
            const float y = bounds.getY() + random.nextFloat() * bounds.getHeight();
            const float length = 0.7f + random.nextFloat() * 4.8f;
            const auto dust = random.nextInt(4) == 0
                                ? juce::Colours::white
                                : colour.darker(0.35f);
            g.setColour(dust.withAlpha(0.055f + random.nextFloat() * 0.13f));
            g.drawLine(x, y, x + length, y + (random.nextFloat() - 0.5f) * 1.5f,
                       0.45f + random.nextFloat() * 0.9f);
        }
        g.restoreState();
    }

    void drawCrayonCard(juce::Graphics& g, juce::Rectangle<float> bounds, float cornerSize)
    {
        g.setColour(juce::Colours::white.withAlpha(0.68f));
        g.fillRoundedRectangle(bounds, cornerSize);
        drawWobblyRoundedOutline(g, bounds.reduced(1.0f), cornerSize, green(), 2.1f);

        juce::Random random(juce::roundToInt(bounds.getX() * 17.0f
                                             + bounds.getY() * 31.0f
                                             + bounds.getWidth()));
        const float perimeter = 2.0f * (bounds.getWidth() + bounds.getHeight());
        for (int i = 0; i < juce::roundToInt(perimeter / 5.0f); ++i)
        {
            const bool horizontal = random.nextBool();
            const bool farEdge = random.nextBool();
            const float alpha = 0.06f + random.nextFloat() * 0.16f;
            g.setColour(green().withAlpha(alpha));
            if (horizontal)
            {
                const float x = bounds.getX() + cornerSize
                                + random.nextFloat() * juce::jmax(1.0f, bounds.getWidth() - cornerSize * 2.0f);
                const float y = farEdge ? bounds.getBottom() : bounds.getY();
                g.drawLine(x, y + (random.nextFloat() - 0.5f) * 3.0f,
                           x + 1.0f + random.nextFloat() * 5.0f, y, 0.8f);
            }
            else
            {
                const float x = farEdge ? bounds.getRight() : bounds.getX();
                const float y = bounds.getY() + cornerSize
                                + random.nextFloat() * juce::jmax(1.0f, bounds.getHeight() - cornerSize * 2.0f);
                g.drawLine(x + (random.nextFloat() - 0.5f) * 3.0f, y,
                           x, y + 1.0f + random.nextFloat() * 5.0f, 0.8f);
            }
        }
    }
}

GuitaruLookAndFeel::GuitaruLookAndFeel()
{
    setColour(juce::ResizableWindow::backgroundColourId, guitaru::paper());
    setColour(juce::Label::textColourId, guitaru::green());
    setColour(juce::TextButton::buttonColourId, guitaru::blue());
    setColour(juce::TextButton::buttonOnColourId, guitaru::red());
    setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    setColour(juce::TextButton::textColourOnId, juce::Colours::white);
    setColour(juce::ComboBox::backgroundColourId, juce::Colours::white.withAlpha(0.78f));
    setColour(juce::ComboBox::textColourId, guitaru::green());
    setColour(juce::ComboBox::outlineColourId, guitaru::green());
    setColour(juce::ComboBox::arrowColourId, guitaru::green());
    setColour(juce::Slider::trackColourId, guitaru::bluePale());
    setColour(juce::Slider::thumbColourId, guitaru::blue());
    setColour(juce::Slider::textBoxTextColourId, guitaru::green());
    setColour(juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    setColour(juce::Slider::textBoxOutlineColourId, guitaru::green().withAlpha(0.35f));
    setColour(juce::ProgressBar::backgroundColourId, guitaru::paperDark());
    setColour(juce::ProgressBar::foregroundColourId, guitaru::blue());
    setColour(juce::ToggleButton::textColourId, guitaru::green());
    setColour(juce::PopupMenu::backgroundColourId, guitaru::paper());
    setColour(juce::PopupMenu::textColourId, guitaru::green());
    setColour(juce::PopupMenu::highlightedBackgroundColourId, guitaru::bluePale());
    setColour(juce::PopupMenu::highlightedTextColourId, guitaru::green());
}

juce::Font GuitaruLookAndFeel::getLabelFont(juce::Label& label)
{
    if (label.getName() == "channel-heading")
        return roundedFont(18.0f, true);

    return roundedFont(18.0f);
}

juce::Font GuitaruLookAndFeel::getTextButtonFont(juce::TextButton&, int buttonHeight)
{
    return roundedFont(juce::jlimit(17.0f, 22.0f,
                                    static_cast<float>(buttonHeight) * 0.48f), true);
}

juce::Font GuitaruLookAndFeel::getComboBoxFont(juce::ComboBox&)
{
    return roundedFont(17.0f, true);
}

void GuitaruLookAndFeel::drawLabel(juce::Graphics& g, juce::Label& label)
{
    g.fillAll(label.findColour(juce::Label::backgroundColourId));

    if (! label.isBeingEdited())
    {
        const float alpha = label.isEnabled() ? 1.0f : 0.5f;
        const bool isSliderValue = dynamic_cast<juce::Slider*>(label.getParentComponent()) != nullptr;
        const auto font = isSliderValue ? roundedFont(16.0f, true) : getLabelFont(label);

        g.setColour((isSliderValue ? guitaru::green()
                                   : label.findColour(juce::Label::textColourId))
                        .withMultipliedAlpha(alpha));
        g.setFont(font);

        const auto textArea = label.getBorderSize().subtractedFrom(label.getLocalBounds());
        g.drawFittedText(label.getText(), textArea, label.getJustificationType(),
                         juce::jmax(1, static_cast<int>(
                             static_cast<float>(textArea.getHeight()) / font.getHeight())),
                         label.getMinimumHorizontalScale());
    }

    // 슬라이더의 숫자 입력칸처럼 외곽선이 있는 Label에 분필 테두리를 한 겹 더 얹는다.
    // 일반 텍스트 Label은 outlineColour가 투명하므로 영향을 받지 않는다.
    const auto outline = label.findColour(juce::Label::outlineColourId);
    if (outline.getFloatAlpha() > 0.01f)
    {
        auto bounds = label.getLocalBounds().toFloat().reduced(1.0f);
        drawWobblyRoundedOutline(g, bounds, 3.5f, guitaru::green(), 1.35f);
        guitaru::drawChalkLine(g, bounds.getTopLeft(), bounds.getTopRight(),
                               guitaru::green(), 1.0f,
                               label.getWidth() * 19 + label.getHeight() * 7);
        guitaru::drawChalkLine(g, bounds.getBottomLeft(), bounds.getBottomRight(),
                               guitaru::green(), 1.0f,
                               label.getWidth() * 23 + label.getHeight() * 11);
    }
}

void GuitaruLookAndFeel::drawButtonBackground(juce::Graphics& g, juce::Button& button,
                                              const juce::Colour& backgroundColour,
                                              bool isMouseOverButton, bool isButtonDown)
{
    auto bounds = button.getLocalBounds().toFloat().reduced(2.0f);
    auto fill = button.isEnabled() ? backgroundColour : guitaru::paperDark();
    if (isButtonDown)
        fill = fill.darker(0.12f);
    else if (isMouseOverButton)
        fill = fill.brighter(0.08f);

    guitaru::drawChalkFill(g, bounds, fill, 14.0f,
                           button.getComponentID().hashCode()
                               + button.getButtonText().hashCode()
                               + button.getWidth() * 31);
    drawWobblyRoundedOutline(g, bounds, 14.0f,
                             button.isEnabled() ? guitaru::green() : guitaru::inkMuted(),
                             2.0f);
}

void GuitaruLookAndFeel::drawButtonText(juce::Graphics& g, juce::TextButton& button,
                                        bool, bool)
{
    g.setFont(getTextButtonFont(button, button.getHeight()));
    g.setColour(button.findColour(button.getToggleState()
                                      ? juce::TextButton::textColourOnId
                                      : juce::TextButton::textColourOffId)
                    .withMultipliedAlpha(button.isEnabled() ? 1.0f : 0.72f));
    g.drawFittedText(button.getButtonText(), button.getLocalBounds().reduced(12, 2),
                     juce::Justification::centred, 1);
}

void GuitaruLookAndFeel::drawComboBox(juce::Graphics& g, int width, int height, bool,
                                      int, int, int, int, juce::ComboBox&)
{
    auto bounds = juce::Rectangle<float>(0.0f, 0.0f,
                                         static_cast<float>(width), static_cast<float>(height)).reduced(1.0f);
    g.setColour(juce::Colours::white.withAlpha(0.76f));
    g.fillRoundedRectangle(bounds, 9.0f);
    drawWobblyRoundedOutline(g, bounds, 9.0f, guitaru::green(), 1.5f);
    guitaru::drawChalkLine(g, { 9.0f, 1.0f },
                           { static_cast<float>(width) - 9.0f, 1.0f },
                           guitaru::green(), 1.0f, width * 17 + height);
    guitaru::drawChalkLine(g, { 9.0f, static_cast<float>(height) - 1.0f },
                           { static_cast<float>(width) - 9.0f, static_cast<float>(height) - 1.0f },
                           guitaru::green(), 1.0f, width * 31 + height);

    juce::Path arrow;
    const float cx = static_cast<float>(width) - 15.0f;
    const float cy = static_cast<float>(height) * 0.5f;
    arrow.startNewSubPath(cx - 4.0f, cy - 2.0f);
    arrow.lineTo(cx, cy + 2.5f);
    arrow.lineTo(cx + 4.0f, cy - 2.0f);
    g.setColour(guitaru::green());
    g.strokePath(arrow, juce::PathStrokeType(2.0f, juce::PathStrokeType::curved,
                                             juce::PathStrokeType::rounded));
}

void GuitaruLookAndFeel::positionComboBoxText(juce::ComboBox& box, juce::Label& label)
{
    label.setBounds(10, 1, box.getWidth() - 32, box.getHeight() - 2);
    label.setFont(getComboBoxFont(box));
}

void GuitaruLookAndFeel::drawLinearSlider(juce::Graphics& g, int x, int y, int width, int height,
                                          float sliderPos, float, float,
                                          juce::Slider::SliderStyle style, juce::Slider& slider)
{
    if (style != juce::Slider::LinearHorizontal)
    {
        LookAndFeel_V4::drawLinearSlider(g, x, y, width, height, sliderPos, 0.0f, 0.0f,
                                         style, slider);
        return;
    }

    const float trackStart = static_cast<float>(x + 6);
    const float trackEnd = static_cast<float>(x + width - 6);
    const float centreY = static_cast<float>(y + height / 2);
    drawSoftChalkTrack(g, trackStart, trackEnd, centreY,
                       guitaru::paperDark().darker(0.18f), 5.2f,
                       width * 13 + height);
    drawSoftChalkTrack(g, trackStart, sliderPos, centreY,
                       guitaru::blue().darker(0.03f), 5.4f,
                       width * 29 + height);

    g.setColour(guitaru::paper());
    g.fillEllipse(sliderPos - 8.0f, centreY - 8.0f, 16.0f, 16.0f);
    g.setColour(guitaru::green());
    g.drawEllipse(sliderPos - 8.0f, centreY - 8.0f, 16.0f, 16.0f, 2.0f);
    g.setColour(guitaru::red());
    g.fillEllipse(sliderPos - 3.0f, centreY - 3.0f, 6.0f, 6.0f);
}

void GuitaruLookAndFeel::drawProgressBar(juce::Graphics& g, juce::ProgressBar&,
                                         int width, int height, double progress,
                                         const juce::String&)
{
    auto bounds = juce::Rectangle<float>(0.0f, 0.0f,
                                         static_cast<float>(width), static_cast<float>(height)).reduced(1.0f);
    const float radius = bounds.getHeight() * 0.5f;
    guitaru::drawChalkFill(g, bounds, guitaru::paperDark(), radius,
                           width * 13 + height * 37);
    const float filled = bounds.getWidth() * static_cast<float>(juce::jlimit(0.0, 1.0, progress));
    if (filled > 1.0f)
        guitaru::drawChalkFill(g, bounds.withWidth(filled), guitaru::blue(),
                               radius,
                               width * 17 + height * 7 + juce::roundToInt(progress * 100.0));
    drawWobblyRoundedOutline(g, bounds, radius, guitaru::green(), 1.3f);
    guitaru::drawChalkLine(g,
                           { bounds.getX() + radius, bounds.getY() + 0.5f },
                           { bounds.getRight() - radius, bounds.getY() + 0.5f },
                           guitaru::green(), 1.0f, width * 41 + height);
    guitaru::drawChalkLine(g,
                           { bounds.getX() + radius, bounds.getBottom() - 0.5f },
                           { bounds.getRight() - radius, bounds.getBottom() - 0.5f },
                           guitaru::green(), 1.0f, width * 43 + height);
}

void GuitaruLookAndFeel::drawToggleButton(juce::Graphics& g, juce::ToggleButton& button,
                                          bool isMouseOverButton, bool)
{
    auto box = juce::Rectangle<float>(2.0f,
                                      (static_cast<float>(button.getHeight()) - 19.0f) * 0.5f,
                                      19.0f, 19.0f);
    g.setColour(isMouseOverButton ? guitaru::bluePale() : juce::Colours::white.withAlpha(0.7f));
    g.fillEllipse(box);
    g.setColour(guitaru::green());
    g.drawEllipse(box, 2.0f);
    if (button.getToggleState())
    {
        g.setColour(guitaru::red());
        g.fillEllipse(box.reduced(5.0f));
    }

    g.setColour(button.findColour(juce::ToggleButton::textColourId));
    g.setFont(roundedFont(17.0f, true));
    g.drawFittedText(button.getButtonText(), button.getLocalBounds().withTrimmedLeft(29),
                     juce::Justification::centredLeft, 2);
}
