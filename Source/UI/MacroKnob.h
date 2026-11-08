#pragma once
#include "Widgets.h"

// The MACRO knob: a large ring with a field of noise grains inside that brightens as the filter opens, a marker for
// the stereo position and a big percentage readout. The slider underneath handles the mouse and the host link.
class MacroKnob : public juce::Component
{
public:
    explicit MacroKnob (APVTS& s)
    {
        slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        slider.setRotaryParameters (juce::MathConstants<float>::pi * 1.25f, juce::MathConstants<float>::pi * 2.75f, true);
        slider.setMouseDragSensitivity (300);
        slider.setLookAndFeel (&nullLnf);
        slider.setTooltip ("MACRO: automate this. 0% to 100% is a riser, 100% to 0% a fall");
        slider.onValueChange = [this] { repaint(); };
        addAndMakeVisible (slider);
        attachment = std::make_unique<APVTS::SliderAttachment> (s, "macro", slider);
        for (int i = 0; i < numGrains; ++i) grain[i] = makeGrain (360, 360, 0.22f, 1.0f, 100u + (juce::uint32) i);
    }
    ~MacroKnob() override { slider.setLookAndFeel (nullptr); }

    // live values from the engine
    void setDisplay (float brightness, float pan, float width, bool noteOn)
    {
        bright = brightness; panPos = pan; widthVal = width; playing = noteOn;
        if (playing) frame = (frame + 1) % numGrains;
        repaint();
    }

    void resized() override { slider.setBounds (getLocalBounds()); }

    void paint (juce::Graphics& g) override
    {
        auto area = getLocalBounds().toFloat();
        const float size = juce::jmin (area.getWidth(), area.getHeight());
        auto b = area.withSizeKeepingCentre (size, size).reduced (6.0f);
        const auto c = b.getCentre();
        const float r = b.getWidth() * 0.5f;
        const float start = juce::MathConstants<float>::pi * 1.25f, end = juce::MathConstants<float>::pi * 2.75f;
        const float pos = (float) slider.valueToProportionOfLength (slider.getValue());
        const float angle = start + pos * (end - start);

        // inner disc: black, then grains whose brightness follows the filter
        auto disc = b.reduced (r * 0.16f);
        g.setColour (Col::panel);
        g.fillEllipse (disc);
        {
            juce::Graphics::ScopedSaveState ss (g);
            juce::Path clip;
            clip.addEllipse (disc.reduced (2.0f));
            g.reduceClipRegion (clip);
            const float a = 0.08f + 0.92f * bright * bright;
            g.setOpacity (a);
            g.drawImage (grain[frame], disc.reduced (2.0f), juce::RectanglePlacement::fillDestination);
            // a soft glow from the middle, stronger when bright
            juce::ColourGradient glow (Col::accent.withAlpha (0.22f * bright), c.x, c.y, Col::accent.withAlpha (0.0f), c.x, c.y - disc.getHeight() * 0.5f, true);
            g.setOpacity (1.0f);
            g.setGradientFill (glow);
            g.fillEllipse (disc);
        }
        g.setColour (Col::border);
        g.drawEllipse (disc, 1.0f);

        // ring
        juce::Path track;
        track.addCentredArc (c.x, c.y, r - 4.0f, r - 4.0f, 0.0f, start, end, true);
        g.setColour (Col::faint);
        g.strokePath (track, juce::PathStrokeType (7.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        juce::Path value;
        value.addCentredArc (c.x, c.y, r - 4.0f, r - 4.0f, 0.0f, start, angle, true);
        g.setColour (Col::accent);
        g.strokePath (value, juce::PathStrokeType (7.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        // tick marks at 0, 25, 50, 75, 100
        g.setColour (Col::dim);
        for (int i = 0; i <= 4; ++i)
        {
            const float a = start + (end - start) * (float) i / 4.0f;
            const auto p1 = c.getPointOnCircumference (r + 2.0f, a), p2 = c.getPointOnCircumference (r + 7.0f, a);
            g.drawLine (p1.x, p1.y, p2.x, p2.y, 1.5f);
        }
        // pointer
        {
            juce::Path ptr;
            ptr.addRoundedRectangle (-2.0f, -(r * 0.84f), 4.0f, r * 0.13f, 2.0f);
            g.setColour (Col::text);
            g.fillPath (ptr, juce::AffineTransform::rotation (angle).translated (c.x, c.y));
        }

        // readout
        const int pct = juce::roundToInt (slider.getValue());
        g.setColour (Col::text);
        g.setFont (juce::Font (juce::FontOptions (r * 0.42f, juce::Font::bold)));
        g.drawText (juce::String (pct) + "%", disc.toNearestInt(), juce::Justification::centred);
        g.setFont (juce::Font (juce::FontOptions (r * 0.11f, juce::Font::bold)));
        g.setColour (Col::dim);
        g.drawText ("MACRO", disc.toNearestInt().withTrimmedTop ((int) (r * 0.45f)), juce::Justification::centred);

        // stereo position: a bar for the width, a dot for the pan
        {
            const float y = c.y + r * 0.58f, w = r * 0.5f;
            g.setColour (Col::faint);
            g.drawLine (c.x - w, y, c.x + w, y, 1.0f);
            const float half = w * juce::jlimit (0.0f, 1.0f, widthVal * 0.5f);
            g.setColour (Col::dim);
            g.drawLine (c.x + panPos * w - half, y, c.x + panPos * w + half, y, 2.0f);
            g.setColour (playing ? Col::accent : Col::dim);
            g.fillEllipse (c.x + panPos * w - 3.0f, y - 3.0f, 6.0f, 6.0f);
        }
    }

    juce::Slider slider;

private:
    struct NullLnf : juce::LookAndFeel_V4 { void drawRotarySlider (juce::Graphics&, int, int, int, int, float, float, float, juce::Slider&) override {} };
    NullLnf nullLnf;
    std::unique_ptr<APVTS::SliderAttachment> attachment;
    static constexpr int numGrains = 3;
    juce::Image grain[numGrains];
    int frame = 0;
    float bright = 0.0f, panPos = 0.0f, widthVal = 1.0f;
    bool playing = false;
};
