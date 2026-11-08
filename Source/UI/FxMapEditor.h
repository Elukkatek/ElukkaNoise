#pragma once
#include "Widgets.h"
#include "../FxPresets.h"

// The Macro window of an effect (v0.9.20; the content of a call-out box): one row per knob of the effect, saying how
// the shared MACRO moves it. The mapping is part of the effect preset: saved with it, restored from it. Edits reach
// the engine at once (FxPresetManager::setMap).
class FxMapEditor : public juce::Component
{
public:
    // maxWidth / maxHeight: the room the window has (v0.9.26: at small UI scales the rows tighten and the width shrinks)
    FxMapEditor (APVTS& s, FxPresetManager& m, int maxWidth = 640, int maxHeight = 1000) : state (s), mgr (m)
    {
        const juce::String fx = juce::String (m.title()).toLowerCase();
        title.setText (juce::String (m.title()).toUpperCase() + " MACRO   " + juce::String::fromUTF8 ("\xe2\x80\x94") + "   " + m.currentName, juce::dontSendNotification);
        title.setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::bold)));
        title.setColour (juce::Label::textColourId, Col::text);
        addAndMakeVisible (title);
        hint.setText ("How the MACRO knob moves the " + fx + ". Saved with the " + fx + " preset, not with the instrument preset.\n"
                      "Add: the knob plus a depth the macro brings in. Range: from start to end, the knob ignored. Curve: early (-) or late (+).",
                      juce::dontSendNotification);
        hint.setColour (juce::Label::textColourId, Col::dim);
        hint.setFont (juce::Font (juce::FontOptions (10.5f)));
        addAndMakeVisible (hint);
        for (int d = 0; d < mgr.numDests(); ++d)
        {
            auto row = std::make_unique<Row>();
            row->param = state.getParameter (mgr.dests()[d].paramID);
            row->on.setButtonText (mgr.dests()[d].name);
            row->on.setTooltip (juce::String ("MACRO moves ") + mgr.dests()[d].name + ": on / off");
            row->on.onClick = [this, d] { apply (d); refresh (d); };
            row->mode.addItemList ({ "Add", "Range" }, 1);
            row->mode.setTooltip ("Add: the knob is the reference, the macro adds the depth. Range: the macro travels from start to end and the knob is ignored");
            row->mode.onChange = [this, d] { apply (d); refresh (d); };
            for (auto* sl : { &row->s1, &row->s2, &row->curve })
            {
                sl->setSliderStyle (juce::Slider::LinearHorizontal);
                sl->setTextBoxStyle (juce::Slider::TextBoxRight, false, 58, 20);
                sl->onValueChange = [this, d] { if (! loading) apply (d); };
            }
            row->curve.setRange (-1.0, 1.0, 0.01);
            row->curve.setDoubleClickReturnValue (true, 0.0);
            row->curve.textFromValueFunction = [] (double v) { return (v > 0.0 ? "+" : "") + juce::String (juce::roundToInt (v * 100.0)); };
            row->curve.valueFromTextFunction = [] (const juce::String& t) { return t.getDoubleValue() * 0.01; };
            row->curve.setTooltip ("Response: negative = most of the change early in the macro's travel, positive = late, 0 = even");
            row->s1.setTooltip ("Add: depth (how far the macro pushes the knob, up or down). Range: the value at MACRO 0 %");
            row->s2.setTooltip ("The value at MACRO 100 %");
            for (juce::Component* c : { (juce::Component*) &row->on, (juce::Component*) &row->mode, (juce::Component*) &row->s1, (juce::Component*) &row->s2, (juce::Component*) &row->curve })
                addAndMakeVisible (c);
            rows.push_back (std::move (row));
            refresh (d);
        }
        restore.setButtonText ("Restore");
        restore.setTooltip ("Back to the mapping saved in the preset");
        restore.onClick = [this] { mgr.restoreMap(); for (int d = 0; d < mgr.numDests(); ++d) refresh (d); };
        addAndMakeVisible (restore);
        rowH = juce::jlimit (18, 26, (maxHeight - 110) / juce::jmax (1, mgr.numDests()));
        setSize (juce::jlimit (420, 640, maxWidth), 70 + rowH * mgr.numDests() + 40);
    }

    void paint (juce::Graphics& g) override { g.fillAll (Col::bg); }   // (the call-out box paints the same; a snapshot shows it too)

    void resized() override
    {
        auto r = getLocalBounds().reduced (12, 10);
        title.setBounds (r.removeFromTop (20));
        hint.setBounds (r.removeFromTop (30));
        r.removeFromTop (6);
        for (auto& row : rows)
        {
            auto line = r.removeFromTop (rowH);
            row->on.setBounds (line.removeFromLeft (96).reduced (0, 2));
            line.removeFromLeft (6);
            row->mode.setBounds (line.removeFromLeft (68).reduced (0, 2));
            line.removeFromLeft (6);
            auto curveArea = line.removeFromRight (120);
            row->curve.setBounds (curveArea);
            line.removeFromRight (8);
            const bool range = row->mode.getSelectedId() == 2;
            if (range)
            {
                row->s1.setBounds (line.removeFromLeft (line.getWidth() / 2 - 3));
                line.removeFromLeft (6);
                row->s2.setBounds (line);
            }
            else row->s1.setBounds (line);
        }
        r.removeFromTop (8);
        restore.setBounds (r.removeFromTop (24).removeFromLeft (96));
    }

private:
    int rowH = 26;
    struct Row
    {
        juce::RangedAudioParameter* param = nullptr;
        juce::ToggleButton on;
        juce::ComboBox mode;
        juce::Slider s1, s2, curve;   // Add: s1 = depth. Range: s1 = start, s2 = end (shown in the knob's units)
    };

    P::FxMapEntry entryOf (int d) const
    {
        auto& row = *rows[(size_t) d];
        P::FxMapEntry e;
        e.on = row.on.getToggleState();
        e.mode = row.mode.getSelectedId() == 2 ? P::MapRange : P::MapAdd;
        if (e.mode == P::MapRange) { e.a = (float) row.s1.getValue(); e.b = (float) row.s2.getValue(); }
        else { e.a = 0.0f; e.b = (float) row.s1.getValue(); }
        e.curve = (float) row.curve.getValue();
        return e;
    }
    void apply (int d)
    {
        auto e = entryOf (d);
        const auto& old = mgr.map[d];
        if (e.mode != old.mode)   // a mode switch keeps the travel it had: depth <-> start / end around the knob
        {
            const float base = rows[(size_t) d]->param != nullptr ? rows[(size_t) d]->param->getValue() : 0.5f;
            if (e.mode == P::MapRange) { e.a = base; e.b = juce::jlimit (0.0f, 1.0f, base + old.b); }
            else e.b = juce::jlimit (-1.0f, 1.0f, old.b - old.a);
        }
        mgr.setMap (d, e);
    }
    void refresh (int d)
    {
        loading = true;
        auto& row = *rows[(size_t) d];
        const auto& e = mgr.map[d];
        row.on.setToggleState (e.on, juce::dontSendNotification);
        row.mode.setSelectedId (e.mode == P::MapRange ? 2 : 1, juce::dontSendNotification);
        const bool range = e.mode == P::MapRange;
        auto* prm = row.param;
        if (range)
        {
            for (auto* sl : { &row.s1, &row.s2 })
            {
                sl->setRange (0.0, 1.0, 0.001);
                sl->textFromValueFunction = [prm] (double v) { return prm != nullptr ? prm->getText ((float) v, 0) : juce::String (v, 2); };
                sl->valueFromTextFunction = [prm] (const juce::String& t) { return prm != nullptr ? (double) prm->getValueForText (t) : t.getDoubleValue(); };
            }
            row.s1.setValue (e.a, juce::dontSendNotification);
            row.s2.setValue (e.b, juce::dontSendNotification);
        }
        else
        {
            row.s1.setRange (-1.0, 1.0, 0.01);
            row.s1.setDoubleClickReturnValue (true, 0.0);
            row.s1.textFromValueFunction = [] (double v) { return (v > 0.0 ? "+" : "") + juce::String (juce::roundToInt (v * 100.0)) + "%"; };
            row.s1.valueFromTextFunction = [] (const juce::String& t) { return t.getDoubleValue() * 0.01; };
            row.s1.setValue (e.b, juce::dontSendNotification);
        }
        row.s2.setVisible (range);
        row.curve.setValue (e.curve, juce::dontSendNotification);
        for (auto* c : { (juce::Component*) &row.mode, (juce::Component*) &row.s1, (juce::Component*) &row.s2, (juce::Component*) &row.curve }) c->setEnabled (e.on);
        for (auto* sl : { &row.s1, &row.s2, &row.curve }) sl->updateText();
        loading = false;
        resized();
    }

    APVTS& state;
    FxPresetManager& mgr;
    juce::Label title, hint;
    std::vector<std::unique_ptr<Row>> rows;
    juce::TextButton restore;
    bool loading = false;
};
