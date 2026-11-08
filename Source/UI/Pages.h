#pragma once
#include "Widgets.h"

class ElukkaNoiseProcessor;
class FxPresetManager;

// A page of the advanced section: titled panels with their controls spaced evenly (see spread() in Widgets.h).
class Page : public juce::Component
{
public:
    explicit Page (ElukkaNoiseProcessor& p) : proc (p) {}
    virtual void refresh() {}   // called when a preset loads
    virtual void tick();        // ~30 Hz while visible (modulation rings, displays)

protected:
    Knob&   makeKnob (const juce::String& id, const juce::String& name, const juce::String& tip = {});
    Choice& makeChoice (const juce::String& id, const juce::String& name);
    Toggle& makeToggle (const juce::String& id, const juce::String& name, const juce::String& tip = {});
    Panel&  makePanel (const juce::String& title);
    APVTS& state();
    bool raw (const char* id) const;   // a toggle's state

    ElukkaNoiseProcessor& proc;
    std::vector<std::unique_ptr<juce::Component>> owned;
    std::vector<Knob*> knobs;
};

class NoiseFilterPage : public Page
{
public:
    explicit NoiseFilterPage (ElukkaNoiseProcessor&);
    void resized() override;
private:
    Panel *noiseP {}, *filterP {}, *filter2P {};
    Knob *colour {}, *stereo {}, *pan {}, *level {}, *attack {}, *release {}, *vel {};
    Knob *cutoff {}, *res {}, *drive {}, *cutoff2 {}, *res2 {}, *drive2 {};
    Toggle *vary {}, *retrig {};
    Choice *type {}, *slope {}, *type2 {}, *slope2 {};
    ViewSwitch noiseViews { { "NOISE", "LEVEL" }, [this] { resized(); } };
};

// LFO 1 / 2 (tabs, wave display) and the S&H, laid out like ElukkaSynth's MOD and S&H RANDOM panels
class LfoPage : public Page
{
public:
    explicit LfoPage (ElukkaNoiseProcessor&);
    void resized() override;
    void paintOverChildren (juce::Graphics&) override;
    void tick() override;
private:
    void showLfo (int i);
    struct Unit
    {
        Choice* wave {};
        Toggle *sync {}, *retrig {};
        Knob *rate {}, *div {}, *phase {}, *depth {}, *smooth {};
        juce::Component* display {};
    };
    Panel *lfoP {}, *shP {};
    Unit units[2];
    juce::TextButton tabs[2];
    int shown = 0;
    float meterValue = 0.0f;
    Toggle *shSync {}, *shRetrig {};
    Knob *shRate {}, *shDiv {}, *shDepth {}, *shSmooth {};
    juce::Component* shPlot {};
    juce::Label shHint;
};

class GatePage : public Page
{
public:
    explicit GatePage (ElukkaNoiseProcessor&);
    void resized() override;
    void tick() override;
private:
    void showPatternMenu();
    Panel* panel {};
    juce::Component* grid = nullptr;
    juce::Component* envView = nullptr;
    juce::Component* stepper = nullptr;
    juce::TextButton patternsBtn;
    Toggle *onT {}, *retrigT {}, *randomT {};
    Choice* divC {};
    Knob *depthK {}, *swingK {}, *dutyK {}, *attK {}, *holdK {}, *decK {}, *susK {}, *relK {};
    ViewSwitch gateViews { { "STEPS", "ENVELOPE" }, [this] { resized(); } };
};

// EFFECTS: compressor, delay and reverb in signal order (the panel on top goes first). Drag a panel by its title to
// reorder them: it follows the mouse and the others slide to their new places.
class EffectsPage : public Page
{
public:
    explicit EffectsPage (ElukkaNoiseProcessor&);
    void resized() override;
    void tick() override;

    // one effect: its panel and controls in one component, so it moves as a whole
    struct Section : public juce::Component, public juce::SettableTooltipClient
    {
        explicit Section (const juce::String& t) : panel (t) { addAndMakeVisible (panel); panel.draggable = true; }
        void resized() override { panel.setBounds (getLocalBounds()); }
        void mouseMove (const juce::MouseEvent& e) override
        {
            const bool title = e.y < 24;
            setMouseCursor (title ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
            setTooltip (title ? "Drag to reorder: the panel on top goes first" : juce::String());
        }
        void mouseDown (const juce::MouseEvent& e) override { grabbed = e.y < 24; if (grabbed && onGrab) onGrab (e.getEventRelativeTo (getParentComponent()).y); }
        void mouseDrag (const juce::MouseEvent& e) override { if (grabbed && onDrag) onDrag (e.getEventRelativeTo (getParentComponent()).y); }
        void mouseUp (const juce::MouseEvent&) override { if (grabbed && onDrop) onDrop(); grabbed = false; }
        juce::Rectangle<int> content() const { return panel.content(); }
        Panel panel;
        bool grabbed = false;
        std::function<void (int)> onGrab, onDrag;
        std::function<void()> onDrop;
    };

private:
    int order() const;
    int heightOf (int fx) const;                                // the compressor panel is taller (its graph)
    juce::Rectangle<int> slotIn (const int* order, int i) const;  // where position i of an order sits
    void place (bool animate);
    void layoutSections();
    Section* sections[P::numFx] {};   // compressor, delay, reverb
    int shownOrder = -1;
    int dragging = -1, grabOffset = 0;
    int dragOrder[P::numFx] { 0, 1, 2 };
    Knob *cAmt {}, *cSpeed {};
    Toggle *cMakeup {}, *cOn {};
    juce::Component* meter {};
    Toggle *dOn {}, *dSync {}, *rOn {};
    Knob *dTime {}, *dDiv {}, *dFb {}, *dFilter {}, *dWidth {}, *dOffset {}, *dWet {}, *dMod {}, *dDuck {},
         *rSize {}, *rDecay {}, *rTone {}, *rWet {}, *rPre {}, *rWidth {}, *rDuck {}, *rMod {};
    ViewSwitch dViews { { "TIME", "TONE" }, [this] { layoutSections(); } }, rViews { { "TIME", "TONE" }, [this] { layoutSections(); } };
    juce::ComboBox dMode;             // delay stereo mode (attached)
    std::unique_ptr<APVTS::ComboBoxAttachment> dModeAtt;
    // Effect presets (v0.9.20): each effect has its own menu (factory, user, Save, Delete) and a MACRO button that
    // opens its mapping window. The preset defines the knobs and how MACRO moves them; the instrument preset does not.
    juce::TextButton dPreset, dMacro { "MACRO" }, rPreset, rMacro { "MACRO" };
    void showFxMenu (FxPresetManager& m, juce::Component& target);
    void saveFxDialog (FxPresetManager& m);
    void deleteFxDialog (FxPresetManager& m);
    void showMapEditor (FxPresetManager& m, juce::Component& target);
    void updateFxButtons();
};

// Modulation matrix (Serum / ElukkaSynth style): the rows in use plus one empty row to add a new one.
// Columns: source, curve, amount, destination, aux source (scales the amount), clear.
class MatrixPage : public Page
{
public:
    explicit MatrixPage (ElukkaNoiseProcessor&);
    ~MatrixPage() override;
    void resized() override;
    void paintOverChildren (juce::Graphics&) override;
    void tick() override;
    void refresh() override { rebuild(); }
private:
    void rebuild();
    std::vector<int> wanted() const;
    Panel* panel {};
    juce::Label help, rangeLabel;
    juce::Component* range = nullptr;
    juce::Viewport viewport;
    juce::Component list;
    std::vector<std::unique_ptr<juce::Component>> rowComps;
    std::vector<int> shown;
};
