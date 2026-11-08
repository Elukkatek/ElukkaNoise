#pragma once
#include <JuceHeader.h>
#include "../Params.h"

// Black with white noise grains: everything is monochrome, the accent is plain white.
namespace Col
{
inline const juce::Colour bg       { 0xff000000 };
inline const juce::Colour panel    { 0xff0c0c0c };
inline const juce::Colour panelHi  { 0xff161616 };
inline const juce::Colour border   { 0xff2a2a2a };
inline const juce::Colour text     { 0xfff2f2f2 };
inline const juce::Colour dim      { 0xff7a7a7a };
inline const juce::Colour faint    { 0xff3a3a3a };
inline const juce::Colour accent   { 0xffffffff };
}

// The editor keeps track of the dialogs and call-out boxes opened from it (preset Save / Rename / Delete, a knob's
// options and value entry, the Macro window, the credits) and closes them when it is destroyed. A modal window that
// survived the host closing the plugin window would swallow the next click, and its contents (parameter attachments)
// would be deleted later, possibly after the processor. Call-out boxes are owned here (launch) and deleted at once
// in closeAll(), content first; AlertWindows are tracked (track) and deleted there too.
struct DialogHost
{
    virtual ~DialogHost() { closeAll(); }

    juce::CallOutBox& launch (std::unique_ptr<juce::Component> content, juce::Rectangle<int> area, juce::Component* parent)
    {
        auto popup = std::make_unique<Popup>();
        popup->content = std::move (content);
        popup->box = std::make_unique<juce::CallOutBox> (*popup->content, area, parent);
        auto* raw = popup.get();
        auto& box = *popup->box;
        popups.push_back (std::move (popup));
        std::weak_ptr<int> alive = token;
        box.enterModalState (true, juce::ModalCallbackFunction::create ([this, raw, alive] (int)
        {
            if (alive.lock() == nullptr) return;   // the host is gone: closeAll() already deleted the box
            popups.erase (std::remove_if (popups.begin(), popups.end(), [raw] (const auto& p) { return p.get() == raw; }), popups.end());
        }), false);
        return box;
    }
    void track (juce::Component& c)
    {
        open.erase (std::remove_if (open.begin(), open.end(), [] (const auto& p) { return p == nullptr; }), open.end());
        open.push_back (&c);
    }
    void closeAll()
    {
        for (auto& c : open) c.deleteAndZero();   // a deleted modal component leaves the modal state; its callback sees "cancelled"
        open.clear();
        popups.clear();
    }

private:
    struct Popup { std::unique_ptr<juce::Component> content; std::unique_ptr<juce::CallOutBox> box; };   // box dies first
    std::vector<std::unique_ptr<Popup>> popups;
    std::vector<juce::Component::SafePointer<juce::Component>> open;
    std::shared_ptr<int> token = std::make_shared<int> (0);
};

// A field of white grains on black: random pixels with random brightness, like film grain / static.
inline juce::Image makeGrain (int w, int h, float density, float maxAlpha, juce::uint32 seed)
{
    juce::Image img (juce::Image::ARGB, juce::jmax (1, w), juce::jmax (1, h), true);
    juce::Image::BitmapData bd (img, juce::Image::BitmapData::writeOnly);
    juce::Random rng ((juce::int64) seed);
    for (int y = 0; y < bd.height; ++y)
        for (int x = 0; x < bd.width; ++x)
            if (rng.nextFloat() < density)
            {
                const float a = rng.nextFloat();
                bd.setPixelColour (x, y, juce::Colours::white.withAlpha (maxAlpha * a * a));
            }
    return img;
}

class NoiseLookAndFeel : public juce::LookAndFeel_V4
{
public:
    NoiseLookAndFeel()
    {
        setColour (juce::ResizableWindow::backgroundColourId, Col::bg);
        setColour (juce::Label::textColourId, Col::text);
        setColour (juce::ComboBox::backgroundColourId, Col::panelHi);
        setColour (juce::ComboBox::outlineColourId, Col::border);
        setColour (juce::ComboBox::textColourId, Col::text);
        setColour (juce::ComboBox::arrowColourId, Col::dim);
        setColour (juce::PopupMenu::backgroundColourId, Col::panelHi);
        setColour (juce::PopupMenu::textColourId, Col::text);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, Col::accent.withAlpha (0.18f));
        setColour (juce::PopupMenu::highlightedTextColourId, Col::text);
        setColour (juce::PopupMenu::headerTextColourId, Col::dim);
        setColour (juce::TextButton::buttonColourId, Col::panelHi);
        setColour (juce::TextButton::buttonOnColourId, Col::accent.withAlpha (0.2f));
        setColour (juce::TextButton::textColourOffId, Col::dim);
        setColour (juce::TextButton::textColourOnId, Col::text);
        setColour (juce::Slider::textBoxTextColourId, Col::text);
        setColour (juce::Slider::textBoxOutlineColourId, Col::border);
        setColour (juce::Slider::textBoxBackgroundColourId, Col::panelHi);
        setColour (juce::BubbleComponent::backgroundColourId, Col::panelHi);
        setColour (juce::BubbleComponent::outlineColourId, Col::border);
        setColour (juce::TooltipWindow::backgroundColourId, Col::panelHi);
        setColour (juce::TooltipWindow::outlineColourId, Col::border);
        setColour (juce::TooltipWindow::textColourId, Col::text);
        setColour (juce::AlertWindow::backgroundColourId, Col::panel);
        setColour (juce::AlertWindow::textColourId, Col::text);
        setColour (juce::AlertWindow::outlineColourId, Col::border);
        setColour (juce::TextEditor::backgroundColourId, Col::panelHi);
        setColour (juce::TextEditor::textColourId, Col::text);
        setColour (juce::TextEditor::outlineColourId, Col::border);
        setColour (juce::TextEditor::focusedOutlineColourId, Col::accent.withAlpha (0.6f));
        setColour (juce::TextEditor::highlightColourId, Col::accent.withAlpha (0.25f));
        setColour (juce::CaretComponent::caretColourId, Col::text);
        setColour (juce::Slider::trackColourId, Col::accent);
        setColour (juce::Slider::backgroundColourId, Col::panelHi);
        setColour (juce::Slider::thumbColourId, Col::accent);
    }

    juce::Font getLabelFont (juce::Label&) override { return juce::Font (juce::FontOptions (12.0f)); }
    juce::Font getComboBoxFont (juce::ComboBox&) override { return juce::Font (juce::FontOptions (12.0f)); }
    juce::Font getPopupMenuFont() override { return juce::Font (juce::FontOptions (13.0f)); }
    juce::Font getTextButtonFont (juce::TextButton&, int) override { return juce::Font (juce::FontOptions (11.5f, juce::Font::bold)); }
    juce::Font getAlertWindowMessageFont() override { return juce::Font (juce::FontOptions (13.0f)); }
    juce::Font getAlertWindowTitleFont() override { return juce::Font (juce::FontOptions (15.0f, juce::Font::bold)); }
    juce::Font getAlertWindowFont() override { return juce::Font (juce::FontOptions (12.5f)); }

    void drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float start, float end, juce::Slider& s) override
    {
        auto b = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h).reduced (3.0f);
        const float size = juce::jmin (b.getWidth(), b.getHeight());
        b = b.withSizeKeepingCentre (size, size);
        const auto c = b.getCentre();
        const float r = size * 0.5f;
        const float angle = start + pos * (end - start);
        const bool bipolar = s.getMinimum() < 0.0 && s.getMaximum() > 0.0;
        const float zero = bipolar ? start + (float) s.valueToProportionOfLength (0.0) * (end - start) : start;

        g.setColour (Col::panelHi);
        g.fillEllipse (b.reduced (r * 0.2f));
        g.setColour (Col::border);
        g.drawEllipse (b.reduced (r * 0.2f), 1.0f);

        juce::Path track;
        track.addCentredArc (c.x, c.y, r - 1.5f, r - 1.5f, 0.0f, start, end, true);
        g.setColour (Col::faint);
        g.strokePath (track, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        juce::Path value;
        value.addCentredArc (c.x, c.y, r - 1.5f, r - 1.5f, 0.0f, juce::jmin (zero, angle), juce::jmax (zero, angle), true);
        g.setColour (s.isEnabled() ? Col::accent : Col::dim);
        g.strokePath (value, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        juce::Path ptr;
        const float len = r * 0.6f;
        ptr.addRoundedRectangle (-1.2f, -len, 2.4f, len * 0.55f, 1.2f);
        g.setColour (Col::text);
        g.fillPath (ptr, juce::AffineTransform::rotation (angle).translated (c.x, c.y));
    }

    void drawToggleButton (juce::Graphics& g, juce::ToggleButton& b, bool hover, bool) override
    {
        auto r = b.getLocalBounds().toFloat().reduced (1.0f);
        const bool on = b.getToggleState();
        g.setColour (on ? Col::accent.withAlpha (0.12f) : Col::panelHi.brighter (hover ? 0.1f : 0.0f));
        g.fillRoundedRectangle (r, 3.0f);
        g.setColour (on ? Col::accent.withAlpha (0.8f) : Col::border);
        g.drawRoundedRectangle (r, 3.0f, 1.0f);
        auto led = r.removeFromLeft (r.getHeight()).reduced (r.getHeight() * 0.33f);
        g.setColour (on ? Col::accent : Col::dim.withAlpha (0.35f));
        g.fillEllipse (led);
        g.setColour (on ? Col::text : Col::dim);
        g.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
        g.drawFittedText (b.getButtonText(), r.toNearestInt().withTrimmedRight (4), juce::Justification::centredLeft, 1);
    }

    void drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&, bool hover, bool down) override
    {
        auto r = b.getLocalBounds().toFloat().reduced (0.5f);
        const bool on = b.getToggleState();
        auto base = on ? Col::accent.withAlpha (0.14f) : Col::panelHi;
        if (hover) base = base.brighter (0.1f);
        if (down) base = base.brighter (0.2f);
        g.setColour (base);
        g.fillRoundedRectangle (r, 3.0f);
        g.setColour (on ? Col::accent.withAlpha (0.7f) : Col::border);
        g.drawRoundedRectangle (r, 3.0f, 1.0f);
    }

    void drawComboBox (juce::Graphics& g, int w, int h, bool, int, int, int, int, juce::ComboBox& box) override
    {
        auto r = juce::Rectangle<float> (0, 0, (float) w, (float) h).reduced (0.5f);
        g.setColour (Col::panelHi);
        g.fillRoundedRectangle (r, 3.0f);
        g.setColour (box.hasKeyboardFocus (true) ? Col::accent.withAlpha (0.5f) : Col::border);
        g.drawRoundedRectangle (r, 3.0f, 1.0f);
        juce::Path arrow;
        const float ax = (float) w - 10.0f, ay = (float) h * 0.5f;
        arrow.addTriangle (ax - 3.5f, ay - 2.0f, ax + 3.5f, ay - 2.0f, ax, ay + 2.5f);
        g.setColour (Col::dim);
        g.fillPath (arrow);
    }

    void positionComboBoxText (juce::ComboBox& box, juce::Label& label) override
    {
        label.setBounds (5, 1, box.getWidth() - 20, box.getHeight() - 2);
        label.setFont (getComboBoxFont (box));
    }

    // the right-click and value windows: black, like the plugin, with a thin border
    void drawCallOutBoxBackground (juce::CallOutBox&, juce::Graphics& g, const juce::Path& path, juce::Image&) override
    {
        g.setColour (Col::bg);
        g.fillPath (path);
        g.setColour (Col::border.brighter (0.3f));
        g.strokePath (path, juce::PathStrokeType (1.0f));
    }
    int getCallOutBoxBorderSize (const juce::CallOutBox&) override { return 14; }

    void drawTooltip (juce::Graphics& g, const juce::String& text, int w, int h) override
    {
        g.fillAll (Col::panelHi);
        g.setColour (Col::border);
        g.drawRect (0, 0, w, h);
        g.setColour (Col::text);
        g.setFont (juce::Font (juce::FontOptions (12.0f)));
        g.drawFittedText (text, juce::Rectangle<int> (0, 0, w, h).reduced (6, 3), juce::Justification::centredLeft, 4);
    }
};

using APVTS = juce::AudioProcessorValueTreeState;

// Modulation matrix helpers (message thread)
namespace Mod
{
inline float raw (APVTS& s, const juce::String& id) { return s.getRawParameterValue (id)->load(); }
inline int src (APVTS& s, int k) { return (int) std::lround (raw (s, P::mx (k, "src"))); }
inline int dst (APVTS& s, int k) { return (int) std::lround (raw (s, P::mx (k, "dst"))) - 1; }
inline bool used (APVTS& s, int k) { return src (s, k) != P::SNone; }
inline void setParam (APVTS& s, const juce::String& id, float value)
{
    if (auto* p = s.getParameter (id))
    {
        p->beginChangeGesture();
        p->setValueNotifyingHost (p->convertTo0to1 (value));
        p->endChangeGesture();
    }
}
// slots that modulate destination d
inline std::vector<int> slotsFor (APVTS& s, int d)
{
    std::vector<int> v;
    for (int k = 0; k < P::numMatrixSlots; ++k)
        if (used (s, k) && dst (s, k) == d) v.push_back (k);
    return v;
}
inline float baseNorm (APVTS& s, int d)
{
    if (d == P::DAmp) return 1.0f;
    return s.getParameter (P::dests[d].paramID)->getValue();
}
// Routes `source` to destination d in the first free slot; returns the slot or -1 when all are in use. As in
// ElukkaSynth, sources run 0..1 and push the knob up, so the amount starts at what is left above the knob: knob value +
// amount = 100 %, and at the source's peak the knob just reaches its end. (Amp has no knob: full depth downwards.)
inline int add (APVTS& s, int source, int d)
{
    for (int k = 0; k < P::numMatrixSlots; ++k)
        if (! used (s, k))
        {
            const float amt = d == P::DAmp ? -1.0f : 1.0f - baseNorm (s, d);
            setParam (s, P::mx (k, "dst"), (float) (d + 1));
            setParam (s, P::mx (k, "amt"), amt);
            setParam (s, P::mx (k, "crv"), 0.0f);
            setParam (s, P::mx (k, "aux"), (float) P::SNone);
            setParam (s, P::mx (k, "src"), (float) source);
            return k;
        }
    return -1;
}
inline void clear (APVTS& s, int k)
{
    setParam (s, P::mx (k, "src"), (float) P::SNone);
    setParam (s, P::mx (k, "dst"), 0.0f);
    setParam (s, P::mx (k, "amt"), 0.0f);
    setParam (s, P::mx (k, "crv"), 0.0f);
    setParam (s, P::mx (k, "aux"), (float) P::SNone);
}
// The range (normalised) the modulation can move destination d over: base plus every slot's reach
inline juce::Range<float> reach (APVTS& s, int d)
{
    const float b = baseNorm (s, d);
    float lo = 0.0f, hi = 0.0f;
    for (int k : slotsFor (s, d))
    {
        const float amt = raw (s, P::mx (k, "amt"));   // sources are 0..1: each row reaches from 0 to its amount
        lo += juce::jmin (0.0f, amt);
        hi += juce::jmax (0.0f, amt);
    }
    return { juce::jlimit (0.0f, 1.0f, b + lo), juce::jlimit (0.0f, 1.0f, b + hi) };
}
}

// A typed value is applied only when it reads as one: it has a digit, or the parameter's text for what it parses to
// contains it ("C", "pink" -> "Pink 50%"). Anything else would parse as 0 and send the knob to its minimum.
inline bool parsesAsValue (const juce::RangedAudioParameter& p, const juce::String& text)
{
    if (text.containsAnyOf ("0123456789")) return true;
    return p.getText (p.getValueForText (text), 64).containsIgnoreCase (text);
}

// Small box for typing a knob's value: Enter applies it and closes, Escape closes without changing it.
class ValueEntry : public juce::Component
{
public:
    explicit ValueEntry (juce::RangedAudioParameter& p) : param (p)
    {
        editor.setText (param.getCurrentValueAsText(), false);
        editor.setJustification (juce::Justification::centred);
        editor.setColour (juce::TextEditor::backgroundColourId, Col::bg);   // black, like the plugin
        editor.onReturnKey = [this] { apply(); close(); };
        editor.onEscapeKey = [this] { close(); };
        addAndMakeVisible (editor);
        setSize (120, 30);
        juce::Component::SafePointer<ValueEntry> safe (this);
        juce::MessageManager::callAsync ([safe] { if (safe != nullptr) { safe->editor.grabKeyboardFocus(); safe->editor.selectAll(); } });
    }
    void resized() override { editor.setBounds (getLocalBounds().reduced (3)); }
    void apply()
    {
        const auto text = editor.getText().trim();
        if (text.isEmpty() || ! parsesAsValue (param, text)) return;
        param.beginChangeGesture();
        param.setValueNotifyingHost (param.getValueForText (text));
        param.endChangeGesture();
    }
    juce::TextEditor editor;
private:
    void close() { if (auto* box = findParentComponentOfClass<juce::CallOutBox>()) box->dismiss(); }
    juce::RangedAudioParameter& param;
};

// Right-click window for a knob (as in ElukkaSynth): type an exact value, reset it, and modulate it with the macro, an
// LFO, the S&H or velocity. Each modulation is a matrix row; the window lists this knob's rows with their amount, aux
// source (which scales the amount) and a Remove button.
class KnobOptions : public juce::Component
{
public:
    KnobOptions (APVTS& s, const juce::String& id) : state (s), param (s.getParameter (id)), dest (P::destForParam (id))
    {
        title.setText (param->getName (64), juce::dontSendNotification);
        title.setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::bold)));
        title.setColour (juce::Label::textColourId, Col::text);
        addAndMakeVisible (title);
        valueLabel.setText ("Value", juce::dontSendNotification);
        valueLabel.setColour (juce::Label::textColourId, Col::dim);
        addAndMakeVisible (valueLabel);
        valueBox.setText (param->getCurrentValueAsText(), false);
        valueBox.setColour (juce::TextEditor::backgroundColourId, Col::bg);
        valueBox.onReturnKey = [this] { applyValue(); closeWindow(); };
        valueBox.onFocusLost = [this] { valueBox.setText (param->getCurrentValueAsText(), false); };   // Return applies; clicking away reverts
        addAndMakeVisible (valueBox);
        resetButton.setButtonText ("Reset to default");
        resetButton.onClick = [this]
        {
            param->beginChangeGesture();
            param->setValueNotifyingHost (param->getDefaultValue());
            param->endChangeGesture();
            valueBox.setText (param->getCurrentValueAsText(), false);
            closeWindow();
        };
        addAndMakeVisible (resetButton);
        hint.setText ("type a value, then press Enter", juce::dontSendNotification);
        hint.setColour (juce::Label::textColourId, Col::dim);
        hint.setFont (juce::Font (juce::FontOptions (10.5f)));
        addAndMakeVisible (hint);
        if (dest >= 0)
        {
            mapLabel.setText ("Modulate with", juce::dontSendNotification);
            mapLabel.setColour (juce::Label::textColourId, Col::dim);
            addAndMakeVisible (mapLabel);
            for (int i = 0; i < numMapButtons; ++i)
            {
                const int source = i + 1;   // Macro, LFO 1, LFO 2, S&H, Velocity
                mapButtons[i].setButtonText (source == P::SVel ? juce::String ("Vel") : P::modSources[source]);
                mapButtons[i].setTooltip ("Modulate this knob with " + P::modSources[source]);
                mapButtons[i].onClick = [this, source]
                {
                    status.setText (Mod::add (state, source, dest) < 0 ? "All 16 matrix rows are in use.\nFree one in the MOD MATRIX tab." : "",
                                    juce::dontSendNotification);
                    buildRows();
                };
                addAndMakeVisible (mapButtons[i]);
            }
            status.setColour (juce::Label::textColourId, Col::text);
            status.setFont (juce::Font (juce::FontOptions (11.0f)));
            addAndMakeVisible (status);
        }
        buildRows();
        juce::Component::SafePointer<KnobOptions> safe (this);
        juce::MessageManager::callAsync ([safe] { if (safe != nullptr) { safe->valueBox.grabKeyboardFocus(); safe->valueBox.selectAll(); } });
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (10);
        title.setBounds (r.removeFromTop (20));
        r.removeFromTop (4);
        auto vr = r.removeFromTop (24);
        valueLabel.setBounds (vr.removeFromLeft (44));
        valueBox.setBounds (vr);
        hint.setBounds (r.removeFromTop (16).withTrimmedLeft (44));
        r.removeFromTop (4);
        resetButton.setBounds (r.removeFromTop (24).withTrimmedLeft (44).removeFromLeft (130));
        if (dest < 0) return;
        r.removeFromTop (8);
        mapLabel.setBounds (r.removeFromTop (18));
        auto br = r.removeFromTop (24);
        const int bw = (br.getWidth() - 3 * (numMapButtons - 1)) / numMapButtons;
        for (auto& b : mapButtons) { b.setBounds (br.removeFromLeft (bw)); br.removeFromLeft (3); }
        r.removeFromTop (4);
        status.setBounds (r.removeFromTop (status.getText().isEmpty() ? 0 : 30));
        for (auto& row : rows)
        {
            auto rr = r.removeFromTop (24);
            row->name.setBounds (rr.removeFromLeft (52));
            row->remove.setBounds (rr.removeFromRight (22));
            rr.removeFromRight (3);
            row->aux.setBounds (rr.removeFromRight (72));
            rr.removeFromRight (3);
            row->amount.setBounds (rr);
            r.removeFromTop (3);
        }
    }

private:
    struct Row
    {
        juce::Label name;
        juce::Slider amount;
        juce::ComboBox aux;
        std::unique_ptr<APVTS::SliderAttachment> amountAtt;
        std::unique_ptr<APVTS::ComboBoxAttachment> auxAtt;
        juce::TextButton remove;
    };

    void applyValue()
    {
        const auto text = valueBox.getText().trim();
        if (text.isNotEmpty() && text != param->getCurrentValueAsText() && parsesAsValue (*param, text))
        {
            param->beginChangeGesture();
            param->setValueNotifyingHost (param->getValueForText (text));
            param->endChangeGesture();
        }
        valueBox.setText (param->getCurrentValueAsText(), false);
    }
    void closeWindow() { if (auto* box = findParentComponentOfClass<juce::CallOutBox>()) box->dismiss(); }

    void buildRows()
    {
        rows.clear();
        if (dest >= 0)
            for (int k : Mod::slotsFor (state, dest))
            {
                auto row = std::make_unique<Row>();
                row->name.setText (P::modSources[Mod::src (state, k)], juce::dontSendNotification);
                row->name.setColour (juce::Label::textColourId, Col::text);
                row->name.setFont (juce::Font (juce::FontOptions (11.5f, juce::Font::bold)));
                row->name.setMinimumHorizontalScale (0.8f);
                row->remove.setButtonText (juce::String::fromUTF8 ("\xc3\x97"));
                row->remove.setTooltip ("Remove this modulation");
                row->amount.setSliderStyle (juce::Slider::LinearHorizontal);
                row->amount.setTextBoxStyle (juce::Slider::TextBoxRight, false, 42, 20);
                row->amountAtt = std::make_unique<APVTS::SliderAttachment> (state, P::mx (k, "amt"), row->amount);
                row->amount.setDoubleClickReturnValue (true, 0.5);
                row->amount.setTooltip ("Modulation amount");
                row->aux.addItemList ({ "No aux", "x Macro", "x LFO 1", "x LFO 2", "x S&H", "x Vel" }, 1);
                row->auxAtt = std::make_unique<APVTS::ComboBoxAttachment> (state, P::mx (k, "aux"), row->aux);
                row->aux.setTooltip ("Aux source: scales this amount (e.g. x Macro: the macro decides how much the source moves the knob)");
                row->remove.onClick = [this, k]
                {
                    Mod::clear (state, k);
                    juce::Component::SafePointer<KnobOptions> safe (this);
                    juce::MessageManager::callAsync ([safe] { if (safe != nullptr) safe->buildRows(); });
                };
                for (juce::Component* c : { (juce::Component*) &row->name, (juce::Component*) &row->amount, (juce::Component*) &row->aux, (juce::Component*) &row->remove })
                    addAndMakeVisible (c);
                rows.push_back (std::move (row));
            }
        const int h = dest < 0 ? 112 : 176 + (status.getText().isEmpty() ? 0 : 30) + (int) rows.size() * 27;
        setSize (300, h);
        resized();
    }

    APVTS& state;
    juce::RangedAudioParameter* param;
    int dest;
    juce::Label title, valueLabel, hint, mapLabel, status;
    juce::TextEditor valueBox;
    juce::TextButton resetButton;
    static constexpr int numMapButtons = 5;
    juce::TextButton mapButtons[numMapButtons];
    std::vector<std::unique_ptr<Row>> rows;
};

// Small "modulated" badge on a knob. Dragging it (up/right = more, Shift = fine) scales the knob's modulation amounts.
class ModBadge : public juce::Component, public juce::SettableTooltipClient
{
public:
    std::function<void()> onDragStart, onDragEnd;
    std::function<void (float)> onDrag;   // total change in modulation amount since the drag started

    ModBadge()
    {
        setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
        setTooltip ("Modulated: drag up or down to change the modulation amount");
    }
    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (Col::bg);
        g.fillEllipse (r);
        g.setColour (Col::accent.withAlpha (isMouseOverOrDragging() ? 1.0f : 0.8f));
        g.drawEllipse (r.reduced (0.5f), 1.0f);
        juce::Path wave;
        const auto c = r.getCentre();
        const float w = r.getWidth() * 0.32f, h = r.getHeight() * 0.18f;
        for (int i = 0; i <= 12; ++i)
        {
            const float t = (float) i / 12.0f;
            const juce::Point<float> p (c.x - w + 2.0f * w * t, c.y - h * std::sin (t * juce::MathConstants<float>::twoPi));
            if (i == 0) wave.startNewSubPath (p); else wave.lineTo (p);
        }
        g.strokePath (wave, juce::PathStrokeType (1.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override { repaint(); }
    void mouseDown (const juce::MouseEvent&) override { if (onDragStart) onDragStart(); }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        const float px = (float) (e.getDistanceFromDragStartX() - e.getDistanceFromDragStartY());
        if (onDrag) onDrag (px * 0.01f * (e.mods.isShiftDown() ? 0.2f : 1.0f));
    }
    void mouseUp (const juce::MouseEvent&) override { if (onDragEnd) onDragEnd(); }
};

// Rotary knob with its name underneath; the name shows the value while the mouse is over or dragging.
// Double-click types a value, right-click opens the options window (value, Modulate with...), Alt-click resets.
// A modulated knob shows a ring over the range its modulation can reach, a dot where the modulation has it right now,
// and a badge (drag it to scale the amounts).
class Knob : public juce::Component
{
public:
    Knob (APVTS& s, const juce::String& id, const juce::String& name, const juce::String& tip = {})
        : state (s), paramID (id), labelText (name), dest (P::destForParam (id))
    {
        setName (id);   // the parameter ID
        slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        slider.setRotaryParameters (juce::MathConstants<float>::pi * 1.25f, juce::MathConstants<float>::pi * 2.75f, true);
        slider.setMouseDragSensitivity (160);
        slider.onValueChange = [this] { if (showingValue) updateLabel(); };
        slider.onDragStart = [this] { showingValue = true; updateLabel(); };
        slider.onDragEnd = [this] { showingValue = isMouseOverOrDragging (true); updateLabel(); };
        slider.onPopup = [this] { showOptions(); };
        slider.onDoubleClick = [this] { showValueEntry(); };
        slider.addMouseListener (this, false);
        if (auto* p = s.getParameter (id))
        {
            slider.setDoubleClickReturnValue (true, p->convertFrom0to1 (p->getDefaultValue()));   // Alt-click resets
            slider.setTooltip ((tip.isNotEmpty() ? tip : p->getName (64))
                               + (dest >= 0 ? "\nDouble-click to type a value. Right-click to modulate it. Alt-click resets" : "\nDouble-click to type a value. Alt-click resets"));
        }
        addAndMakeVisible (slider);
        label.setJustificationType (juce::Justification::centred);
        label.setColour (juce::Label::textColourId, Col::dim);
        label.setFont (juce::Font (juce::FontOptions (10.5f)));
        label.setInterceptsMouseClicks (false, false);
        addAndMakeVisible (label);
        attachment = std::make_unique<APVTS::SliderAttachment> (s, id, slider);
        if (dest >= 0)
        {
            addChildComponent (badge);
            badge.onDragStart = [this] { beginAmountDrag(); };
            badge.onDrag = [this] (float delta) { dragAmount (delta); };
            badge.onDragEnd = [this] { endAmountDrag(); };
        }
        updateLabel();
    }

    // where the engine has this destination right now (normalised), for the live dot
    void setLive (const std::atomic<float>* v) { live = v; }

    // An effect knob (v0.9.20): its macro mapping comes from the effect preset, not the matrix. entry() gives the
    // mapping as it stands; the ring and dot show it like a matrix modulation. A Range mapping ignores the knob, so
    // the knob is drawn dimmed while one is on.
    void setFxMap (const std::atomic<float>* liveValue, std::function<P::FxMapEntry()> entry)
    {
        live = liveValue;
        fxEntry = std::move (entry);
        slider.setTooltip (slider.getTooltip() + "\nMACRO moves it as the " + (paramID.startsWith ("dly_") ? "delay" : "reverb") + " preset's Macro window says");
    }

    void resized() override
    {
        auto r = getLocalBounds();
        label.setBounds (r.removeFromBottom (13));
        slider.setBounds (r);
        const auto k = knobCircle();
        const int size = 12;
        badge.setBounds (juce::jmin (getWidth() - size, (int) (k.getRight() - 2.0f)), juce::jmax (0, (int) (k.getY() - 3.0f)), size, size);
    }
    void mouseEnter (const juce::MouseEvent&) override { showingValue = true; updateLabel(); }
    void mouseExit (const juce::MouseEvent&) override { if (! slider.isMouseButtonDown()) { showingValue = false; updateLabel(); } }

    // ~30 Hz from the page: badge, ring and live dot
    void tickMod()
    {
        if (fxEntry)
        {
            const auto e = fxEntry();
            const float alpha = e.on && e.mode == P::MapRange ? 0.5f : 1.0f;
            if (std::abs (slider.getAlpha() - alpha) > 0.01f) slider.setAlpha (alpha);
            if (e.on != wasMapped) { wasMapped = e.on; repaint(); }
            if (! e.on) return;
            const auto rg = reach();
            const float now = live != nullptr ? live->load() : -1.0f;
            const float key = rg.getStart() * 3.0f + rg.getEnd() * 7.0f + now * 11.0f;
            if (key != lastKey) { lastKey = key; repaint(); }
            return;
        }
        if (dest < 0 || ! dragSlots.empty()) return;
        const bool mapped = ! Mod::slotsFor (state, dest).empty();
        if (mapped != badge.isVisible()) badge.setVisible (mapped);
        if (! mapped) { if (wasMapped) { wasMapped = false; repaint(); } return; }
        wasMapped = true;
        const auto rg = Mod::reach (state, dest);
        const float now = live != nullptr ? live->load() : -1.0f;
        const float key = rg.getStart() * 3.0f + rg.getEnd() * 7.0f + now * 11.0f;
        if (key != lastKey) { lastKey = key; repaint(); }
    }

    void paintOverChildren (juce::Graphics& g) override
    {
        if ((dest < 0 && ! fxEntry) || ! wasMapped) return;
        const auto k = knobCircle();
        const auto c = k.getCentre();
        const float r = k.getWidth() * 0.5f + 1.0f;
        const float start = juce::MathConstants<float>::pi * 1.25f, end = juce::MathConstants<float>::pi * 2.75f;
        auto ang = [&] (float p) { return start + p * (end - start); };
        const auto rg = reach();
        juce::Path range;
        range.addCentredArc (c.x, c.y, r, r, 0.0f, ang (rg.getStart()), ang (juce::jmax (rg.getEnd(), rg.getStart() + 0.002f)), true);
        g.setColour (Col::accent.withAlpha (0.45f));
        g.strokePath (range, juce::PathStrokeType (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::butt));
        if (live != nullptr)
        {
            const auto dot = c.getPointOnCircumference (r, ang (live->load()));
            g.setColour (Col::accent);
            g.fillEllipse (dot.x - 3.0f, dot.y - 3.0f, 6.0f, 6.0f);
            g.setColour (Col::bg);
            g.drawEllipse (dot.x - 3.0f, dot.y - 3.0f, 6.0f, 6.0f, 1.0f);
        }
    }

    struct PopupSlider : juce::Slider
    {
        std::function<void()> onPopup, onDoubleClick;
        void mouseDown (const juce::MouseEvent& e) override
        {
            if (e.mods.isPopupMenu()) { if (onPopup) onPopup(); return; }
            juce::Slider::mouseDown (e);
        }
        void mouseDrag (const juce::MouseEvent& e) override { if (! e.mods.isPopupMenu()) juce::Slider::mouseDrag (e); }
        void mouseUp (const juce::MouseEvent& e) override { if (! e.mods.isPopupMenu()) juce::Slider::mouseUp (e); }
        void mouseDoubleClick (const juce::MouseEvent&) override { if (onDoubleClick) onDoubleClick(); }
    };
    PopupSlider slider;

private:
    juce::Rectangle<float> knobCircle() const
    {
        auto b = slider.getBounds().toFloat().reduced (3.0f);
        const float size = juce::jmin (b.getWidth(), b.getHeight());
        return b.withSizeKeepingCentre (size, size);
    }
    // the normalised range the modulation (matrix or effect mapping) can move this knob over
    juce::Range<float> reach() const
    {
        if (fxEntry)
        {
            auto* p = state.getParameter (paramID);
            return P::fxMapReach (fxEntry(), p != nullptr ? p->getValue() : 0.0f);
        }
        return Mod::reach (state, dest);
    }

    void showOptions()
    {
        // inside the plugin window, so it stays on top and can take keyboard focus in hosts
        auto* top = getTopLevelComponent();
        const auto area = top->getLocalArea (&slider, slider.getLocalBounds());
        if (auto* host = findParentComponentOfClass<DialogHost>()) host->launch (std::make_unique<KnobOptions> (state, paramID), area, top);
        else juce::CallOutBox::launchAsynchronously (std::make_unique<KnobOptions> (state, paramID), area, top);
    }
    void showValueEntry()
    {
        if (auto* p = state.getParameter (paramID))
        {
            auto* top = getTopLevelComponent();
            const auto area = top->getLocalArea (&slider, slider.getLocalBounds());
            if (auto* host = findParentComponentOfClass<DialogHost>()) host->launch (std::make_unique<ValueEntry> (*p), area, top);
            else juce::CallOutBox::launchAsynchronously (std::make_unique<ValueEntry> (*p), area, top);
        }
    }

    // Badge drag: scales all of this knob's amounts so their total moves by delta (keeping their balance); when they
    // cancel out the change is shared equally.
    void beginAmountDrag()
    {
        dragSlots = Mod::slotsFor (state, dest);
        dragStart.clear();
        for (int k : dragSlots)
        {
            dragStart.push_back (Mod::raw (state, P::mx (k, "amt")));
            state.getParameter (P::mx (k, "amt"))->beginChangeGesture();
        }
    }
    void dragAmount (float delta)
    {
        if (dragSlots.empty()) return;
        float startTotal = 0.0f;
        for (float a : dragStart) startTotal += a;
        const float n = (float) dragSlots.size();
        const float target = juce::jlimit (-n, n, startTotal + delta);
        float total = 0.0f;
        for (size_t i = 0; i < dragSlots.size(); ++i)
        {
            const float a = std::abs (startTotal) > 0.02f ? dragStart[i] * target / startTotal : dragStart[i] + (target - startTotal) / n;
            auto* p = state.getParameter (P::mx (dragSlots[i], "amt"));
            const float clamped = juce::jlimit (-1.0f, 1.0f, a);
            p->setValueNotifyingHost (p->convertTo0to1 (clamped));
            total += clamped;
        }
        label.setText ((total > 0.0f ? "+" : "") + juce::String (juce::roundToInt (total * 100.0f)) + "%", juce::dontSendNotification);
        label.setColour (juce::Label::textColourId, Col::text);
        repaint();
    }
    void endAmountDrag()
    {
        for (int k : dragSlots) state.getParameter (P::mx (k, "amt"))->endChangeGesture();
        dragSlots.clear();
        updateLabel();
    }

    void updateLabel()
    {
        if (showingValue)
            if (auto* p = state.getParameter (paramID))
            {
                label.setColour (juce::Label::textColourId, Col::text);
                label.setText (p->getCurrentValueAsText(), juce::dontSendNotification);
                return;
            }
        label.setColour (juce::Label::textColourId, Col::dim);
        label.setText (labelText, juce::dontSendNotification);
    }

    APVTS& state;
    juce::String paramID, labelText;
    int dest = -1;
    const std::atomic<float>* live = nullptr;
    std::function<P::FxMapEntry()> fxEntry;
    float lastKey = -1.0e9f;
    bool wasMapped = false;
    juce::Label label;
    std::unique_ptr<APVTS::SliderAttachment> attachment;
    ModBadge badge;
    std::vector<int> dragSlots;
    std::vector<float> dragStart;
    bool showingValue = false;
};

class Choice : public juce::Component
{
public:
    Choice (APVTS& s, const juce::String& id, const juce::String& name)
    {
        if (auto* p = dynamic_cast<juce::AudioParameterChoice*> (s.getParameter (id)))
            box.addItemList (p->choices, 1);
        box.setScrollWheelEnabled (true);
        addAndMakeVisible (box);
        label.setText (name, juce::dontSendNotification);
        label.setJustificationType (juce::Justification::centred);
        label.setColour (juce::Label::textColourId, Col::dim);
        label.setFont (juce::Font (juce::FontOptions (10.5f)));
        addAndMakeVisible (label);
        attachment = std::make_unique<APVTS::ComboBoxAttachment> (s, id, box);
    }
    void resized() override
    {
        auto r = getLocalBounds();
        if (label.getText().isNotEmpty()) label.setBounds (r.removeFromBottom (13));
        else label.setBounds ({});   // no label: the box is centred in the whole cell (it was pushed up and clipped)
        box.setBounds (r.withSizeKeepingCentre (r.getWidth(), 22));
    }
    juce::ComboBox box;
private:
    juce::Label label;
    std::unique_ptr<APVTS::ComboBoxAttachment> attachment;
};

class Toggle : public juce::Component
{
public:
    Toggle (APVTS& s, const juce::String& id, const juce::String& name)
    {
        button.setButtonText (name);
        addAndMakeVisible (button);
        attachment = std::make_unique<APVTS::ButtonAttachment> (s, id, button);
    }
    void resized() override { button.setBounds (getLocalBounds().withSizeKeepingCentre (getWidth(), 22)); }
    juce::ToggleButton button;
private:
    std::unique_ptr<APVTS::ButtonAttachment> attachment;
};

// Section heading inside a page
class Heading : public juce::Component
{
public:
    explicit Heading (const juce::String& t) : text (t) {}
    void paint (juce::Graphics& g) override
    {
        g.setColour (Col::text);
        g.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
        auto r = getLocalBounds();
        const int tw = juce::roundToInt (juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), text)) + 8;
        g.drawText (text, r.removeFromLeft (tw), juce::Justification::centredLeft);
        g.setColour (Col::border);
        g.drawHorizontalLine (r.getCentreY(), (float) r.getX(), (float) r.getRight());
    }
private:
    juce::String text;
};

// Small peak meter (falls slowly)
class Meter : public juce::Component
{
public:
    void set (float peak)
    {
        shown = juce::jmax (peak, shown * 0.85f);
        repaint();
    }
    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (Col::panelHi);
        g.fillRoundedRectangle (r, 2.0f);
        const float db = juce::Decibels::gainToDecibels (shown, -60.0f);
        const float frac = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f);
        auto fill = r.reduced (1.0f);
        fill = fill.removeFromBottom (fill.getHeight() * frac);
        g.setColour (db > -1.0f ? Col::accent : Col::accent.withAlpha (0.65f));
        g.fillRoundedRectangle (fill, 1.5f);
        g.setColour (Col::border);
        g.drawRoundedRectangle (r, 2.0f, 1.0f);
    }
private:
    float shown = 0.0f;
};

// MIDI activity light
class Led : public juce::Component
{
public:
    void set (bool on) { if (on != lit) { lit = on; repaint(); } }
    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (2.0f);
        g.setColour (lit ? Col::accent : Col::faint);
        g.fillEllipse (r);
        if (lit)
        {
            g.setColour (Col::accent.withAlpha (0.25f));
            g.drawEllipse (r.expanded (2.0f), 2.0f);
        }
    }
private:
    bool lit = false;
};

// Rounded section background with a title (as in ElukkaSynth).
class Panel : public juce::Component
{
public:
    explicit Panel (const juce::String& t) : title (t) { setInterceptsMouseClicks (false, true); }
    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (highlight ? Col::panelHi : Col::panel);
        g.fillRoundedRectangle (r, 5.0f);
        g.setColour (highlight ? Col::accent.withAlpha (0.6f) : Col::border);
        g.drawRoundedRectangle (r, 5.0f, 1.0f);
        g.setColour (Col::text);
        g.setFont (juce::Font (juce::FontOptions (11.5f, juce::Font::bold)));
        int x = 10;
        if (draggable)
        {
            // grip: two columns of three dots
            g.setColour (Col::dim);
            for (int i = 0; i < 2; ++i)
                for (int j = 0; j < 3; ++j)
                    g.fillEllipse (10.0f + (float) i * 4.0f, 8.0f + (float) j * 4.0f, 2.2f, 2.2f);
            g.setColour (Col::text);
            x = 26;
        }
        g.drawText (title, x, 5, getWidth() - x - 10, 16, juce::Justification::centredLeft);
    }
    bool draggable = false, highlight = false;
    juce::Rectangle<int> titleStripInParent() const { return getBounds().withHeight (24); }
    // area below the title, in the panel's own coordinates / in its parent's
    juce::Rectangle<int> content() const { return getLocalBounds().reduced (8).withTrimmedTop (18); }
    juce::Rectangle<int> contentInParent() const { return content() + getPosition(); }
    juce::String title;
    // where the title text ends (for the view buttons that follow it)
    int titleEnd() const
    {
        return (draggable ? 26 : 10) + juce::GlyphArrangement::getStringWidthInt (juce::Font (juce::FontOptions (11.5f, juce::Font::bold)), title);
    }
};

// A row of small radio buttons in a panel's title strip that picks which group of controls the panel shows (v0.9.23:
// NOISE / LEVEL, STEPS / ENVELOPE, TIME / TONE). The page lays itself out again when the view changes.
class ViewSwitch : public juce::Component
{
public:
    ViewSwitch (std::initializer_list<const char*> names, std::function<void()> onChange) : change (std::move (onChange))
    {
        static int group = 7000;
        const int g = ++group;
        for (auto* n : names)
        {
            auto b = std::make_unique<juce::TextButton> (n);
            b->setClickingTogglesState (true);
            b->setRadioGroupId (g);
            const int i = (int) buttons.size();
            b->onClick = [this, i] { if (index != i) { index = i; if (change) change(); } };
            b->setTooltip (juce::String ("Show the ") + juce::String (n).toLowerCase() + " controls of this section");
            addAndMakeVisible (*b);
            buttons.push_back (std::move (b));
        }
        buttons[0]->setToggleState (true, juce::dontSendNotification);
    }
    static constexpr int buttonW = 66, gap = 4;
    int idealWidth() const { return (int) buttons.size() * buttonW + ((int) buttons.size() - 1) * gap; }
    void resized() override
    {
        int x = 0;
        for (auto& b : buttons) { b->setBounds (x, 0, buttonW, getHeight()); x += buttonW + gap; }
    }
    int index = 0;
private:
    std::vector<std::unique_ptr<juce::TextButton>> buttons;
    std::function<void()> change;
};

// One cell of a row: a component, its width and its height (centred vertically in the row). Every knob row of the
// advanced section uses the same cell (v0.9.23), so the knobs line up from page to page.
struct Cell { juce::Component* c; int w; int h; };
inline Cell knobCell (juce::Component& c, int w = 62, int h = 72) { return { &c, w, h }; }
inline Cell choiceCell (juce::Component& c, int w = 100) { return { &c, w, 40 }; }
inline Cell toggleCell (juce::Component& c, int w = 74) { return { &c, w, 24 }; }

// Lays cells out left to right across `area` with equal gaps (the group is centred; gaps never exceed maxGap).
inline void spread (juce::Rectangle<int> area, std::initializer_list<Cell> cells, int maxGap = 48)
{
    int total = 0, n = 0;
    for (auto& c : cells) { total += c.w; ++n; }
    if (n == 0) return;
    const int gap = juce::jlimit (4, maxGap, (area.getWidth() - total) / (n + 1));
    int x = area.getX() + (area.getWidth() - total - gap * (n - 1)) / 2;
    for (auto& c : cells)
    {
        if (c.c != nullptr) c.c->setBounds (x, area.getCentreY() - c.h / 2, c.w, c.h);
        x += c.w + gap;
    }
}
