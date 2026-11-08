#include "Pages.h"
#include "FxMapEditor.h"
#include "../PluginProcessor.h"
#include <deque>

APVTS& Page::state() { return proc.apvts; }
bool Page::raw (const char* id) const { return proc.apvts.getRawParameterValue (id)->load() > 0.5f; }

Knob& Page::makeKnob (const juce::String& id, const juce::String& name, const juce::String& tip)
{
    auto k = std::make_unique<Knob> (state(), id, name, tip);
    auto* r = k.get();
    if (const int d = P::destForParam (id); d >= 0) r->setLive (&proc.engine.uiDest[d]);
    addAndMakeVisible (*r);
    knobs.push_back (r);
    owned.push_back (std::move (k));
    return *r;
}

Choice& Page::makeChoice (const juce::String& id, const juce::String& name)
{
    auto c = std::make_unique<Choice> (state(), id, name);
    auto* r = c.get();
    addAndMakeVisible (*r);
    owned.push_back (std::move (c));
    return *r;
}

Toggle& Page::makeToggle (const juce::String& id, const juce::String& name, const juce::String& tip)
{
    auto t = std::make_unique<Toggle> (state(), id, name);
    auto* r = t.get();
    if (tip.isNotEmpty()) r->button.setTooltip (tip);
    addAndMakeVisible (*r);
    owned.push_back (std::move (t));
    return *r;
}

Panel& Page::makePanel (const juce::String& title)
{
    auto p = std::make_unique<Panel> (title);
    auto* r = p.get();
    addAndMakeVisible (*r);
    owned.push_back (std::move (p));
    return *r;
}

void Page::tick()
{
    for (auto* k : knobs) k->tickMod();
}

// A knob pair sharing one spot: the synced division while Sync is on, the free rate / time otherwise
static void syncSwap (juce::Component* free, juce::Component* synced, bool sync)
{
    synced->setVisible (sync);   // (setVisible does nothing when the state is unchanged)
    free->setVisible (! sync);
}

//==============================================================================
NoiseFilterPage::NoiseFilterPage (ElukkaNoiseProcessor& p) : Page (p)
{
    noiseP = &makePanel ("NOISE");
    addAndMakeVisible (noiseViews);
    filterP = &makePanel ("FILTER");
    colour = &makeKnob ("noise_colour", "Colour", "Noise colour: brown (dark) through pink to white (bright)");
    stereo = &makeKnob ("noise_stereo", "Stereo", "0% = mono (the same noise in both channels), 100% = independent left and right noise, 200% = extra wide");
    pan = &makeKnob ("pan", "Pan", "Position of the noise in the stereo field, before the delay and reverb");
    level = &makeKnob ("out_level", "Level", "Output level, before delay and reverb");
    attack = &makeKnob ("env_attack", "Attack", "Fade-in after note on");
    release = &makeKnob ("env_release", "Release", "Fade-out after note off (delay and reverb tails ring on by themselves)");
    vel = &makeKnob ("vel_sens", "Velocity", "How much note velocity changes the level");
    retrig = &makeToggle ("retrig", "Retrigger", "Restart the LFOs and S&H on every note. Off: they only restart when nothing was sounding (the gate has its own Retrig switch)");
    vary = &makeToggle ("noise_vary", "Vary", "On: every note and gate step gets fresh, random noise. Off: every note and gate step starts the same noise burst");
    type = &makeChoice ("flt_type", "Type");
    slope = &makeChoice ("flt_slope", "Slope");
    cutoff = &makeKnob ("flt_cutoff", "Cutoff", "Filter cutoff");
    res = &makeKnob ("flt_res", "Resonance");
    drive = &makeKnob ("flt_drive", "Drive", "Saturation before the filter");
    filter2P = &makePanel ("FILTER 2");
    type2 = &makeChoice ("flt2_type", "Type");
    slope2 = &makeChoice ("flt2_slope", "Slope");
    cutoff2 = &makeKnob ("flt2_cutoff", "Cutoff", "Second filter, after the main filter: a tone control for the brightness (low pass at 20 kHz = off)");
    res2 = &makeKnob ("flt2_res", "Resonance");
    drive2 = &makeKnob ("flt2_drive", "Drive", "Saturation before filter 2");
}

void NoiseFilterPage::resized()
{
    auto r = getLocalBounds();
    const int h = (r.getHeight() - 16) / 3;
    noiseP->setBounds (r.removeFromTop (h));
    r.removeFromTop (8);
    filterP->setBounds (r.removeFromTop (h));
    r.removeFromTop (8);
    filter2P->setBounds (r);
    // NOISE: two views in the title strip. NOISE = the noise and where it sits; LEVEL = how loud and the note envelope
    noiseViews.setBounds (noiseP->getX() + noiseP->titleEnd() + 14, noiseP->getY() + 3, noiseViews.idealWidth(), 18);
    const bool noiseView = noiseViews.index == 0;
    for (juce::Component* c : { (juce::Component*) colour, (juce::Component*) stereo, (juce::Component*) pan, (juce::Component*) vary }) c->setVisible (noiseView);
    for (juce::Component* c : { (juce::Component*) level, (juce::Component*) attack, (juce::Component*) release, (juce::Component*) vel, (juce::Component*) retrig }) c->setVisible (! noiseView);
    if (noiseView) spread (noiseP->contentInParent(), { knobCell (*colour), knobCell (*stereo), knobCell (*pan), toggleCell (*vary, 74) }, 40);
    else spread (noiseP->contentInParent(), { knobCell (*level), knobCell (*attack), knobCell (*release), knobCell (*vel), toggleCell (*retrig, 96) }, 40);
    // the two filters, laid out identically
    spread (filterP->contentInParent(), { choiceCell (*type, 100), choiceCell (*slope, 76), knobCell (*cutoff), knobCell (*res), knobCell (*drive) }, 40);
    spread (filter2P->contentInParent(), { choiceCell (*type2, 100), choiceCell (*slope2, 76), knobCell (*cutoff2), knobCell (*res2), knobCell (*drive2) }, 40);
}

//==============================================================================
namespace
{
// One LFO's wave over a cycle, on a grid like ElukkaSynth's shape editor, with the playhead and the output dot.
class WaveDisplay : public juce::Component
{
public:
    WaveDisplay (ElukkaNoiseProcessor& p, int i) : proc (p), index (i) {}
    void tick() { repaint(); }
    void paint (juce::Graphics& g) override
    {
        auto v = [this] (const juce::String& id) { return proc.apvts.getRawParameterValue (id)->load(); };
        const juce::String pre = "l" + juce::String (index + 1) + "_";
        const int wave = (int) v (pre + "wave");
        const bool uni = true;   // every source runs 0..100 % (as in ElukkaSynth)
        auto r = getLocalBounds().toFloat();
        g.setColour (Col::bg);
        g.fillRoundedRectangle (r, 4.0f);
        auto a = r.reduced (6.0f).withTrimmedLeft (34.0f);
        auto toScreen = [&] (float x, float y) { return juce::Point<float> (a.getX() + a.getWidth() * x, a.getBottom() - a.getHeight() * y); };
        for (int i = 0; i <= 16; ++i)
        {
            const auto p0 = toScreen ((float) i / 16.0f, 0.0f), p1 = toScreen ((float) i / 16.0f, 1.0f);
            g.setColour (i % 4 == 0 ? Col::border.brighter (0.5f) : Col::border.withAlpha (0.6f));
            g.drawLine (p0.x, p0.y, p1.x, p1.y, 1.0f);
        }
        for (int j = 0; j <= 8; ++j)
        {
            const auto p0 = toScreen (0.0f, (float) j / 8.0f), p1 = toScreen (1.0f, (float) j / 8.0f);
            g.setColour (j % 4 == 0 ? Col::border.brighter (0.5f) : Col::border.withAlpha (0.6f));
            g.drawLine (p0.x, p0.y, p1.x, p1.y, 1.0f);
        }
        g.setColour (Col::dim);
        g.setFont (juce::Font (juce::FontOptions (10.0f)));
        const char* labels[3] { uni ? "0%" : "-100%", uni ? "50%" : "0", "100%" };
        for (int k = 0; k < 3; ++k)
        {
            const auto p = toScreen (0.0f, (float) k * 0.5f);
            g.drawText (labels[k], juce::Rectangle<float> (r.getX() + 2.0f, p.y - 6.0f, 36.0f, 12.0f), juce::Justification::centredLeft);
        }
        // the wave (0..1 on screen); random waves show a fixed example pattern
        auto shapeAt = [&] (float ph)
        {
            static const float rnd[9] { 0.62f, 0.18f, 0.85f, 0.4f, 0.95f, 0.08f, 0.55f, 0.3f, 0.62f };
            const float x = ph - std::floor (ph);
            switch (wave)
            {
                case P::WTriangle:  return x < 0.5f ? 2.0f * x : 2.0f - 2.0f * x;
                case P::WSawUp:     return x;
                case P::WSawDown:   return 1.0f - x;
                case P::WSquare:    return x < 0.5f ? 1.0f : 0.0f;
                case P::WSampleHold: return rnd[juce::jmin (7, (int) (x * 8.0f))];
                case P::WSmoothRandom:
                {
                    const float s = x * 8.0f; const int k = juce::jmin (7, (int) s); const float t = 0.5f - 0.5f * std::cos ((s - (float) k) * juce::MathConstants<float>::pi);
                    return rnd[k] + (rnd[k + 1] - rnd[k]) * t;
                }
                default:            return 0.5f + 0.5f * std::sin (x * juce::MathConstants<float>::twoPi);
            }
        };
        juce::Path p;
        for (int i = 0; i <= 240; ++i)
        {
            const float x = (float) i / 240.0f;
            const auto s = toScreen (x, shapeAt (x));
            if (i == 0) p.startNewSubPath (s); else p.lineTo (s);
        }
        juce::Path fill (p);
        fill.lineTo (toScreen (1.0f, uni ? 0.0f : 0.5f));
        fill.lineTo (toScreen (0.0f, uni ? 0.0f : 0.5f));
        fill.closeSubPath();
        g.setColour (Col::accent.withAlpha (0.1f));
        g.fillPath (fill);
        g.setColour (Col::accent);
        g.strokePath (p, juce::PathStrokeType (1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        // playhead
        const float ph = proc.engine.uiLfoPhase[index].load();
        const auto top = toScreen (ph, 1.0f), bottom = toScreen (ph, 0.0f);
        g.setColour (Col::accent.withAlpha (0.35f));
        g.drawLine (top.x, top.y, bottom.x, bottom.y, 1.0f);
        const auto dot = toScreen (ph, shapeAt (ph));
        g.setColour (Col::bg);
        g.fillEllipse (dot.x - 4.5f, dot.y - 4.5f, 9.0f, 9.0f);
        g.setColour (Col::text);
        g.drawEllipse (dot.x - 4.5f, dot.y - 4.5f, 9.0f, 9.0f, 1.5f);
    }
private:
    ElukkaNoiseProcessor& proc;
    int index;
};

// The S&H's recent steps (as in ElukkaSynth's S&H RANDOM panel)
class ShPlot : public juce::Component
{
public:
    explicit ShPlot (ElukkaNoiseProcessor& p) : proc (p) {}
    void tick()
    {
        const float v = proc.engine.uiSh.load();
        if (v == last) return;
        last = v;
        history.push_back (v);
        while (history.size() > 24) history.pop_front();
        repaint();
    }
    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (Col::bg);
        g.fillRoundedRectangle (r, 4.0f);
        g.setColour (Col::border);
        g.drawHorizontalLine ((int) r.getCentreY(), r.getX(), r.getRight());
        if (history.empty()) return;
        const float w = r.getWidth() / 24.0f;
        juce::Path p;
        float x = r.getRight() - w * (float) history.size();
        for (size_t i = 0; i < history.size(); ++i)
        {
            const float y = r.getBottom() - 4.0f - juce::jlimit (0.0f, 1.0f, history[i]) * (r.getHeight() - 8.0f);   // 0 % bottom, 100 % top
            if (i == 0) p.startNewSubPath (x, y); else p.lineTo (x, y);
            p.lineTo (x + w, y);
            x += w;
        }
        g.setColour (Col::accent);
        g.strokePath (p, juce::PathStrokeType (1.8f));
    }
private:
    ElukkaNoiseProcessor& proc;
    std::deque<float> history;
    float last = 99.0f;
};
}

LfoPage::LfoPage (ElukkaNoiseProcessor& p) : Page (p)
{
    lfoP = &makePanel ("LFO");
    shP = &makePanel ("S&H RANDOM");
    for (int i = 0; i < 2; ++i)
    {
        const juce::String id = "l" + juce::String (i + 1) + "_";
        auto& u = units[i];
        u.wave = &makeChoice (id + "wave", "");
        u.sync = &makeToggle (id + "sync", "Sync", "Lock the rate to the host tempo");
        u.retrig = &makeToggle (id + "retrig", "Retrig", "On: restarts at Phase on every note. Off: free running; a synced LFO then follows the song position");
        u.rate = &makeKnob (id + "rate", "Rate", "Rate in Hz");
        u.div = &makeKnob (id + "div", "Rate", "Synced rate, from 8 bars to 1/32 (D = dotted, T = triplet)");
        u.phase = &makeKnob (id + "phase", "Phase", "Where the wave starts when it retriggers");
        u.depth = &makeKnob (id + "depth", "Depth", "Overall depth of this LFO");
        u.smooth = &makeKnob (id + "smooth", "Smooth", "Rounds off steps and corners in the output");
        auto d = std::make_unique<WaveDisplay> (p, i);
        u.display = d.get();
        addAndMakeVisible (*u.display);
        owned.push_back (std::move (d));
        tabs[i].setButtonText ("LFO " + juce::String (i + 1));
        tabs[i].setClickingTogglesState (true);
        tabs[i].setRadioGroupId (4242);
        tabs[i].onClick = [this, i] { showLfo (i); };
        tabs[i].setTooltip ("Show LFO " + juce::String (i + 1) + ". Right-click any knob and choose Modulate with to route it");
        addAndMakeVisible (tabs[i]);
    }
    shSync = &makeToggle ("sh_sync", "Sync", "Lock the steps to the host tempo");
    shRetrig = &makeToggle ("sh_retrig", "Retrig", "On: a new value and restarted steps on every note. Off with Sync: steps follow the song position");
    shRate = &makeKnob ("sh_rate", "Rate", "New random values per second");
    shDiv = &makeKnob ("sh_div", "Rate", "Synced step rate, from 8 bars to 1/32");
    shDepth = &makeKnob ("sh_depth", "Depth");
    shSmooth = &makeKnob ("sh_smooth", "Smooth", "Glide between values (0 = instant steps)");
    auto plot = std::make_unique<ShPlot> (p);
    shPlot = plot.get();
    addAndMakeVisible (*shPlot);
    owned.push_back (std::move (plot));
    shHint.setText ("Right-click any knob and choose S&H to map it.", juce::dontSendNotification);
    shHint.setColour (juce::Label::textColourId, Col::dim);
    shHint.setFont (juce::Font (juce::FontOptions (11.0f)));
    shHint.setJustificationType (juce::Justification::topLeft);
    shHint.setMinimumHorizontalScale (1.0f);
    addAndMakeVisible (shHint);
    tabs[0].setToggleState (true, juce::dontSendNotification);
    showLfo (0);
    tick();
}

void LfoPage::showLfo (int i)
{
    shown = i;
    for (int k = 0; k < 2; ++k)
    {
        auto& u = units[k];
        for (juce::Component* c : { (juce::Component*) u.wave, (juce::Component*) u.sync, (juce::Component*) u.retrig,
                                    (juce::Component*) u.phase, (juce::Component*) u.depth, (juce::Component*) u.smooth, u.display })
            c->setVisible (k == i);
        u.rate->setVisible (false);
        u.div->setVisible (false);
        tabs[k].setToggleState (k == i, juce::dontSendNotification);
    }
    const auto& u = units[i];
    syncSwap (u.rate, u.div, raw (i == 0 ? "l1_sync" : "l2_sync"));
    if (! u.rate->isVisible() && ! u.div->isVisible()) u.rate->setVisible (true);
}

void LfoPage::resized()
{
    auto r = getLocalBounds();
    shP->setBounds (r.removeFromRight (212));
    r.removeFromRight (8);
    lfoP->setBounds (r);

    // LFO panel: tabs beside the title, then wave and toggles, knobs down the left, the wave display
    for (int i = 0; i < 2; ++i) tabs[i].setBounds (lfoP->getX() + 48 + i * 62, lfoP->getY() + 4, 58, 18);
    for (auto& u : units)
    {
        auto c = lfoP->contentInParent().withTrimmedTop (7);
        auto top = c.removeFromTop (24);
        u.wave->setBounds (top.removeFromLeft (128));
        top.removeFromLeft (10);
        u.sync->setBounds (top.removeFromLeft (64));
        top.removeFromLeft (6);
        u.retrig->setBounds (top.removeFromLeft (70));
        c.removeFromTop (5);
        auto left = c.removeFromLeft (62);
        const int kh = left.getHeight() / 4;
        u.rate->setBounds (left.removeFromTop (kh));
        u.div->setBounds (u.rate->getBounds());
        u.phase->setBounds (left.removeFromTop (kh));
        u.depth->setBounds (left.removeFromTop (kh));
        u.smooth->setBounds (left);
        c.removeFromLeft (8);
        u.display->setBounds (c);
    }

    // S&H panel: toggles, knobs, the step plot, a hint
    auto s = shP->contentInParent().withTrimmedTop (4);
    auto top = s.removeFromTop (24);
    spread (top, { toggleCell (*shSync, 70), toggleCell (*shRetrig, 74) }, 12);
    s.removeFromTop (6);
    auto row = s.removeFromTop (70);
    shDiv->setBounds (0, 0, 0, 0);
    spread (row, { knobCell (*shRate, 58, 66), knobCell (*shDepth, 58, 66), knobCell (*shSmooth, 58, 66) }, 10);
    shDiv->setBounds (shRate->getBounds());
    shHint.setBounds (s.removeFromBottom (30));
    s.removeFromBottom (4);
    shPlot->setBounds (s.reduced (0, 4));
}

void LfoPage::paintOverChildren (juce::Graphics& g)
{
    // live output meter next to the LFO title (as in ElukkaSynth)
    auto r = juce::Rectangle<float> ((float) lfoP->getRight() - 86.0f, (float) lfoP->getY() + 9.0f, 74.0f, 8.0f);
    g.setColour (Col::bg);
    g.fillRoundedRectangle (r, 3.0f);
    const float v = juce::jlimit (-1.0f, 1.0f, meterValue);
    auto bar = r.withWidth (juce::jmax (0.0f, v) * r.getWidth());   // 0..100 %
    g.setColour (Col::accent);
    g.fillRoundedRectangle (bar, 3.0f);
}

void LfoPage::tick()
{
    Page::tick();
    const auto& u = units[shown];
    syncSwap (u.rate, u.div, raw (shown == 0 ? "l1_sync" : "l2_sync"));
    syncSwap (shRate, shDiv, raw ("sh_sync"));
    static_cast<WaveDisplay*> (u.display)->tick();
    static_cast<ShPlot*> (shPlot)->tick();
    const float v = shown == 0 ? proc.engine.uiLfo1.load() : proc.engine.uiLfo2.load();
    if (std::abs (v - meterValue) > 0.005f)
    {
        meterValue = v;
        repaint (lfoP->getRight() - 92, lfoP->getY() + 4, 90, 18);
    }
}

//==============================================================================
// Trance gate widgets

namespace
{
void setStep (APVTS& s, int i, int state)
{
    if (auto* p = s.getParameter (P::gateStep (i)))
    {
        p->beginChangeGesture();
        p->setValueNotifyingHost (p->convertTo0to1 ((float) state));
        p->endChangeGesture();
    }
}
int getStep (APVTS& s, int i) { return juce::jlimit (0, 2, (int) s.getRawParameterValue (P::gateStep (i))->load()); }

// 16 steps in four groups. Top button: on / off (drag to paint). Lower strip: tie into this step (continues the step
// before without a new attack). Right-click sets the pattern length. Above: the step that is playing.
class StepGrid : public juce::Component, public juce::SettableTooltipClient
{
public:
    StepGrid (ElukkaNoiseProcessor& p) : proc (p)
    {
        setTooltip ("Click or drag to switch steps on and off. Lower strip: tie a step to the one before (no new attack). Right-click a step to end the pattern there");
    }

    void tick()
    {
        int key = proc.engine.uiGateStep.load() + 100 * (int) proc.apvts.getRawParameterValue ("gate_len")->load();
        const bool random = isRandom();
        if (random) key += 10000 * (proc.engine.uiGateRandomBits.load() & 0xffff) + 7;
        for (int i = 0; i < P::numGateSteps; ++i) key = key * 3 + getStep (proc.apvts, i);
        key += proc.engine.uiNoteOn.load() ? 1 : 0;
        if (key != lastKey) { lastKey = key; repaint(); }
    }

    void paint (juce::Graphics& g) override
    {
        const int len = (int) proc.apvts.getRawParameterValue ("gate_len")->load();
        const int playing = proc.engine.uiGateStep.load();
        const bool active = proc.engine.uiNoteOn.load() && proc.apvts.getRawParameterValue ("gate_on")->load() > 0.5f;
        const bool random = isRandom();
        const int bits = proc.engine.uiGateRandomBits.load();
        for (int i = 0; i < P::numGateSteps; ++i)
        {
            const int st = random ? (((bits >> i) & 1) != 0 ? 1 : 0) : getStep (proc.apvts, i);
            const bool inLen = i < len;
            auto led = ledRect (i), btn = buttonRect (i), tie = tieRect (i);
            const float a = inLen ? 1.0f : 0.3f;
            // playing position
            g.setColour (active && i == playing ? Col::accent : Col::faint.withMultipliedAlpha (a));
            g.fillRoundedRectangle (led, 1.5f);
            // step button
            if (st != P::GOff)
            {
                juce::ColourGradient grad (Col::accent.withAlpha (0.95f * a), btn.getX(), btn.getY(), Col::accent.withAlpha (0.7f * a), btn.getX(), btn.getBottom(), false);
                g.setGradientFill (grad);
                g.fillRoundedRectangle (btn, 3.0f);
            }
            else
            {
                g.setColour (Col::panelHi.withMultipliedAlpha (inLen ? 1.0f : 0.5f));
                g.fillRoundedRectangle (btn, 3.0f);
            }
            g.setColour (random ? Col::dim.withAlpha (0.7f * a) : (st != P::GOff ? Col::accent.withAlpha (a) : Col::border));
            g.drawRoundedRectangle (btn.reduced (0.5f), 3.0f, 1.0f);
            if (active && i == playing)
            {
                g.setColour (Col::accent);
                g.drawRoundedRectangle (btn.expanded (1.5f), 4.0f, 1.5f);
            }
            // step number
            g.setColour (st != P::GOff ? Col::bg.withAlpha (0.6f * a) : Col::dim.withAlpha (a));
            g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
            g.drawText (juce::String (i + 1), btn.toNearestInt().removeFromBottom (16), juce::Justification::centred);
            // tie strip
            g.setColour (Col::panel.withMultipliedAlpha (a));
            g.fillRoundedRectangle (tie, 2.0f);
            g.setColour (Col::border.withMultipliedAlpha (a));
            g.drawRoundedRectangle (tie.reduced (0.5f), 2.0f, 1.0f);
            if (st == P::GTie && ! random)
            {
                // a bar reaching back into the previous step, ending in an arrow
                const float y = tie.getCentreY();
                const float x0 = i > 0 ? buttonRect (i - 1).getCentreX() : tie.getX() + 2.0f, x1 = tie.getCentreX() + 4.0f;
                g.setColour (Col::accent.withAlpha (a));
                g.drawLine (x0, y, x1 - 4.0f, y, 2.0f);
                juce::Path arrow;
                arrow.addTriangle (x1 - 5.0f, y - 4.0f, x1 - 5.0f, y + 4.0f, x1 + 2.0f, y);
                g.fillPath (arrow);
            }
        }
        if (random)
        {
            g.setColour (Col::dim);
            g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
            g.drawText ("RANDOM STEPS: A NEW PATTERN EVERY CYCLE", getLocalBounds().removeFromBottom (12), juce::Justification::centredRight);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (isRandom()) return;
        const int i = stepAt (e.position);
        if (i < 0) return;
        if (e.mods.isPopupMenu())
        {
            if (auto* p = proc.apvts.getParameter ("gate_len"))
            {
                p->beginChangeGesture();
                p->setValueNotifyingHost (p->convertTo0to1 ((float) (i + 1)));
                p->endChangeGesture();
            }
            repaint();
            return;
        }
        onTieRow = e.position.y >= tieRect (0).getY() - 2.0f;
        const int st = getStep (proc.apvts, i);
        paintState = onTieRow ? (st == P::GTie ? P::GOn : P::GTie) : (st == P::GOff ? P::GOn : P::GOff);
        lastPainted = -1;
        apply (i);
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (isRandom() || e.mods.isPopupMenu()) return;
        const int i = stepAt ({ e.position.x, onTieRow ? tieRect (0).getCentreY() : buttonRect (0).getCentreY() });
        if (i >= 0) apply (i);
    }

private:
    bool isRandom() const { return proc.apvts.getRawParameterValue ("gate_random")->load() > 0.5f; }
    void apply (int i)
    {
        if (i == lastPainted) return;
        lastPainted = i;
        if (getStep (proc.apvts, i) != paintState) setStep (proc.apvts, i, paintState);
        repaint();
    }
    float stepX (int i) const
    {
        const float gap = 4.0f, groupGap = 14.0f;
        const float w = ((float) getWidth() - 15.0f * gap - 3.0f * (groupGap - gap)) / 16.0f;
        return (float) i * (w + gap) + (float) (i / 4) * (groupGap - gap);
    }
    float stepW() const { return stepX (1) - stepX (0) - 4.0f; }
    juce::Rectangle<float> ledRect (int i) const    { return { stepX (i) + 6.0f, 0.0f, stepW() - 12.0f, 4.0f }; }
    juce::Rectangle<float> buttonRect (int i) const { return { stepX (i), 8.0f, stepW(), 44.0f }; }
    juce::Rectangle<float> tieRect (int i) const    { return { stepX (i), 56.0f, stepW(), 16.0f }; }
    int stepAt (juce::Point<float> p) const
    {
        for (int i = 0; i < P::numGateSteps; ++i)
            if (p.x >= stepX (i) - 2.0f && p.x < stepX (i) + stepW() + 2.0f) return i;
        return -1;
    }

    ElukkaNoiseProcessor& proc;
    int lastKey = -1, paintState = 1, lastPainted = -1;
    bool onTieRow = false;
};

// One step of the gate: the gain over time as the engine computes it (attack, hold, decay, sustain, release after the
// step length), on a black screen like A1TriggerGate's envelope view.
class GateEnvelopeView : public juce::Component
{
public:
    explicit GateEnvelopeView (ElukkaNoiseProcessor& p) : proc (p) {}

    void tick()
    {
        float key = 0.0f;
        for (auto* id : { "gate_att", "gate_hold", "gate_decay", "gate_sus", "gate_rel", "gate_duty", "gate_div" })
            key = key * 1.3f + proc.apvts.getRawParameterValue (id)->load();
        key += proc.engine.uiDest[P::DGateDepth].load() * 0.7f;
        key += (float) proc.uiHostBpm.load() * 0.01f;
        if (key != lastKey) { lastKey = key; repaint(); }
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (Col::bg);
        g.fillRoundedRectangle (r, 4.0f);
        g.setColour (Col::border);
        g.drawRoundedRectangle (r.reduced (0.5f), 4.0f, 1.0f);
        auto area = r.reduced (10.0f).withTrimmedTop (16.0f);

        auto v = [this] (const char* id) { return proc.apvts.getRawParameterValue (id)->load(); };
        const double stepMs = (double) P::gateDivBeats[juce::jlimit (0, P::gateDivNames.size() - 1, (int) v ("gate_div"))] * 60000.0 / proc.uiHostBpm.load();
        // depth where the modulation has it now
        const float depth = proc.apvts.getParameter ("gate_depth")->convertFrom0to1 (proc.engine.uiDest[P::DGateDepth].load());
        const float duty = v ("gate_duty");
        // simulate two steps at 4 kHz with the engine's equations, draw the second (steady state)
        constexpr double rate = 4000.0;
        const int n = juce::jmax (8, (int) (stepMs * 0.001 * rate));
        auto coef = [] (float ms) { return 1.0f - std::exp (-1.0f / (juce::jmax (0.1f, ms) * 0.001f * (float) rate)); };
        const float aA = coef (v ("gate_att")), dA = coef (v ("gate_decay")), rA = coef (v ("gate_rel")), sus = v ("gate_sus");
        const int hold = (int) (v ("gate_hold") * 0.001f * (float) rate);
        std::vector<float> out ((size_t) n);
        float env = 0.0f;
        for (int pass = 0; pass < 2; ++pass)
        {
            int stage = 0, holdLeft = 0;
            for (int i = 0; i < n; ++i)
            {
                if (i >= (int) ((double) duty * n)) stage = 3;
                switch (stage)
                {
                    case 0: env += (1.0f - env) * aA; if (env >= 0.995f) { stage = 1; holdLeft = hold; } break;
                    case 1: if (--holdLeft <= 0) stage = 2; break;
                    case 2: env += (sus - env) * dA; break;
                    default: env += (0.0f - env) * rA; break;
                }
                out[(size_t) i] = 1.0f - depth * (1.0f - env);
            }
        }
        // grid: quarter lines and the step length (dashed)
        g.setColour (Col::faint);
        for (int k = 1; k < 4; ++k) g.drawVerticalLine ((int) (area.getX() + area.getWidth() * (float) k / 4.0f), area.getY(), area.getBottom());
        {
            const float x = area.getX() + area.getWidth() * duty;
            const float dash[] { 3.0f, 3.0f };
            g.setColour (Col::dim);
            g.drawDashedLine ({ x, area.getY(), x, area.getBottom() }, dash, 2, 1.0f);
        }
        juce::Path fill, line;
        fill.startNewSubPath (area.getX(), area.getBottom());
        for (int i = 0; i < n; ++i)
        {
            const float x = area.getX() + area.getWidth() * (float) i / (float) (n - 1);
            const float y = area.getBottom() - area.getHeight() * out[(size_t) i];
            fill.lineTo (x, y);
            if (i == 0) line.startNewSubPath (x, y); else line.lineTo (x, y);
        }
        fill.lineTo (area.getRight(), area.getBottom());
        fill.closeSubPath();
        g.setColour (Col::accent.withAlpha (0.18f));
        g.fillPath (fill);
        g.setColour (Col::accent);
        g.strokePath (line, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour (Col::dim);
        g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
        g.drawText ("ONE STEP  " + (stepMs < 100.0 ? juce::String (stepMs, 1) : juce::String (juce::roundToInt (stepMs))) + " ms",
                    r.reduced (8.0f, 4.0f).removeFromTop (12.0f), juce::Justification::centredLeft);
    }

private:
    ElukkaNoiseProcessor& proc;
    float lastKey = -1.0f;
};

// "STEPS  -  16  +": the pattern length
class StepCount : public juce::Component
{
public:
    explicit StepCount (APVTS& s) : param (*s.getParameter ("gate_len")),
        attachment (param, [this] (float) { value.setText (param.getCurrentValueAsText(), juce::dontSendNotification); })
    {
        for (auto* b : { &minus, &plus }) addAndMakeVisible (b);
        minus.onClick = [this] { step (-1); };
        plus.onClick = [this] { step (1); };
        value.setJustificationType (juce::Justification::centred);
        value.setColour (juce::Label::backgroundColourId, Col::bg);
        value.setColour (juce::Label::outlineColourId, Col::border);
        value.setFont (juce::Font (juce::FontOptions (14.0f, juce::Font::bold)));
        addAndMakeVisible (value);
        label.setText ("Steps", juce::dontSendNotification);
        label.setJustificationType (juce::Justification::centred);
        label.setColour (juce::Label::textColourId, Col::dim);
        label.setFont (juce::Font (juce::FontOptions (10.5f)));
        addAndMakeVisible (label);
        attachment.sendInitialUpdate();
        setTooltip ("Pattern length in steps (right-clicking a step sets it too)");
    }
    void setTooltip (const juce::String& t) { value.setTooltip (t); minus.setTooltip (t); plus.setTooltip (t); }
    void resized() override
    {
        auto r = getLocalBounds();
        label.setBounds (r.removeFromBottom (13));
        auto row = r.withSizeKeepingCentre (r.getWidth(), 24);
        minus.setBounds (row.removeFromLeft (24));
        plus.setBounds (row.removeFromRight (24));
        value.setBounds (row.reduced (3, 0));
    }
private:
    void step (int d)
    {
        const int v = juce::jlimit (1, P::numGateSteps, (int) std::lround (param.convertFrom0to1 (param.getValue())) + d);
        attachment.setValueAsCompleteGesture ((float) v);
    }
    juce::RangedAudioParameter& param;
    juce::ParameterAttachment attachment;
    juce::TextButton minus { "-" }, plus { "+" };
    juce::Label value, label;
};
}

GatePage::GatePage (ElukkaNoiseProcessor& p) : Page (p)
{
    panel = &makePanel ("TRANCE GATE");
    addAndMakeVisible (gateViews);
    auto g = std::make_unique<StepGrid> (p);
    grid = g.get();
    addAndMakeVisible (*grid);
    owned.push_back (std::move (g));
    auto e = std::make_unique<GateEnvelopeView> (p);
    envView = e.get();
    addAndMakeVisible (*envView);
    owned.push_back (std::move (e));
    auto sc = std::make_unique<StepCount> (state());
    stepper = sc.get();
    addAndMakeVisible (*stepper);
    owned.push_back (std::move (sc));

    patternsBtn.setButtonText (juce::String::fromUTF8 ("Patterns  \xe2\x96\xbe"));
    patternsBtn.setTooltip ("Load a pattern into the steps, or clear, invert, shift or randomise them");
    patternsBtn.onClick = [this] { showPatternMenu(); };
    addAndMakeVisible (patternsBtn);

    onT = &makeToggle ("gate_on", "On", "Switch the gate on. With Noise Vary off every opening step restarts the same noise burst; with Vary on each step plays fresh random noise");
    randomT = &makeToggle ("gate_random", "Random", "A new random pattern on every cycle instead of the steps above");
    retrigT = &makeToggle ("gate_retrig", "Retrig", "On: the pattern restarts at step 1 on every note. Off: the steps follow the song position");
    divC = &makeChoice ("gate_div", "Division");
    depthK = &makeKnob ("gate_depth", "Depth", "How far the gate closes: 0% = no gating, 100% = silence between steps");
    swingK = &makeKnob ("gate_swing", "Swing", "Delays every second step: 50% = straight, 66% = triplet feel, 75% = hard shuffle");
    dutyK = &makeKnob ("gate_duty", "Step Len", "How much of each step the gate stays open before the release (the dashed line)");
    attK = &makeKnob ("gate_att", "Attack", "Fade-in at the start of each step");
    holdK = &makeKnob ("gate_hold", "Hold", "Time at full level before the decay");
    decK = &makeKnob ("gate_decay", "Decay", "Time to fall to the sustain level");
    susK = &makeKnob ("gate_sus", "Sustain", "Level held until the step length ends");
    relK = &makeKnob ("gate_rel", "Release", "Fade-out after the step length");
}

void GatePage::resized()
{
    panel->setBounds (getLocalBounds());
    auto r = panel->contentInParent().withTrimmedTop (4);
    grid->setBounds (r.removeFromTop (78));
    r.removeFromTop (8);
    envView->setBounds (r.removeFromLeft (212));
    r.removeFromLeft (10);
    auto rowA = r.removeFromTop (26);
    spread (rowA, { toggleCell (*onT, 54), toggleCell (*retrigT, 68), toggleCell (*randomT, 82), { &patternsBtn, 104, 24 } }, 16);
    r.removeFromTop (4);
    // STEPS or ENVELOPE (the buttons in the title strip): one evenly spaced row of knobs
    gateViews.setBounds (panel->getX() + panel->titleEnd() + 14, panel->getY() + 3, gateViews.idealWidth(), 18);
    const bool stepsView = gateViews.index == 0;
    for (juce::Component* c : { (juce::Component*) divC, (juce::Component*) depthK, (juce::Component*) swingK, (juce::Component*) dutyK, stepper }) c->setVisible (stepsView);
    for (juce::Component* c : { (juce::Component*) attK, (juce::Component*) holdK, (juce::Component*) decK, (juce::Component*) susK, (juce::Component*) relK }) c->setVisible (! stepsView);
    if (stepsView) spread (r, { choiceCell (*divC, 90), knobCell (*depthK), knobCell (*swingK), knobCell (*dutyK), { stepper, 96, 40 } }, 40);
    else spread (r, { knobCell (*attK), knobCell (*holdK), knobCell (*decK), knobCell (*susK), knobCell (*relK) }, 40);
}

void GatePage::tick()
{
    Page::tick();
    static_cast<StepGrid*> (grid)->tick();
    static_cast<GateEnvelopeView*> (envView)->tick();
}

void GatePage::showPatternMenu()
{
    juce::PopupMenu m;
    int id = 1;
    for (auto& pat : P::gatePatterns) m.addItem (id++, pat.name);
    m.addSeparator();
    m.addItem (100, "Clear");
    m.addItem (101, "Invert");
    m.addItem (102, "Shift Left");
    m.addItem (103, "Shift Right");
    m.addItem (104, "Remove Ties");
    m.addItem (105, "Randomise");
    juce::Component::SafePointer<GatePage> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&patternsBtn), [safe] (int r)
    {
        if (safe == nullptr || r == 0) return;
        auto& s = safe->state();
        int cur[P::numGateSteps];
        for (int i = 0; i < P::numGateSteps; ++i) cur[i] = getStep (s, i);
        int next[P::numGateSteps];
        const int numPatterns = (int) (sizeof (P::gatePatterns) / sizeof (P::gatePatterns[0]));
        juce::Random rng;
        for (int i = 0; i < P::numGateSteps; ++i)
        {
            if (r >= 1 && r <= numPatterns) next[i] = ((P::gatePatterns[r - 1].bits >> i) & 1) != 0 ? P::GOn : P::GOff;
            else if (r == 100) next[i] = P::GOff;
            else if (r == 101) next[i] = cur[i] == P::GOff ? P::GOn : P::GOff;
            else if (r == 102) next[i] = cur[(i + 1) % P::numGateSteps];
            else if (r == 103) next[i] = cur[(i + P::numGateSteps - 1) % P::numGateSteps];
            else if (r == 104) next[i] = cur[i] == P::GTie ? P::GOn : cur[i];
            else next[i] = rng.nextFloat() < 0.65f ? P::GOn : P::GOff;
        }
        if (r == 105) next[0] = P::GOn;
        for (int i = 0; i < P::numGateSteps; ++i)
            if (next[i] != cur[i]) setStep (s, i, next[i]);
    });
}

//==============================================================================
namespace
{
// Compressor graph: the transfer curve (input level across, output level up, -48..0 dB) for the current amount:
// 1:1 up to the knee (the big handle at the threshold), then flatter by the ratio. The area under the curve fills up
// to the level coming in.
class CompGraph : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit CompGraph (ElukkaNoiseProcessor& p) : proc (p)
    {
        setTooltip ("Compressor curve: input level across, output level up. The big point is the knee (threshold); "
                    "the line after it is flatter by the ratio. The fill shows the level coming in");
    }
    void tick()
    {
        const float lv = proc.engine.uiCompLevel.load();
        level = lv > level ? lv : juce::jmax (lv, level - 1.5f);   // falls back slowly, like a meter
        repaint();
    }
    void paint (juce::Graphics& g) override
    {
        const bool on = proc.apvts.getRawParameterValue ("comp_on")->load() > 0.5f;
        if (! on) g.beginTransparencyLayer (0.35f);   // dimmed while the compressor is off
        const float amount = proc.engine.uiDest[P::DCompAmt].load();
        auto r = getLocalBounds().toFloat();
        g.setColour (Col::bg);
        g.fillRoundedRectangle (r, 4.0f);
        auto a = r.reduced (6.0f, 6.0f).withTrimmedRight (24.0f);
        constexpr float lo = -48.0f;
        auto pt = [&] (float inDb)
        {
            const float outDb = inDb + Compressor::curveDb (inDb, amount);
            return juce::Point<float> (a.getX() + a.getWidth() * (inDb - lo) / -lo, a.getBottom() - a.getHeight() * juce::jlimit (0.0f, 1.0f, (outDb - lo) / -lo));
        };
        // grid: columns every 6 dB of input (alternate shading) and output lines with labels on the right
        for (int k = 0; k < 8; ++k)
            if (k % 2 == 1)
            {
                g.setColour (Col::panelHi.withAlpha (0.6f));
                g.fillRect (juce::Rectangle<float> (a.getX() + a.getWidth() * (float) k / 8.0f, a.getY(), a.getWidth() / 8.0f, a.getHeight()));
            }
        g.setFont (juce::Font (juce::FontOptions (9.5f)));
        for (float db : { -12.0f, -24.0f, -36.0f })
        {
            const float y = a.getBottom() - a.getHeight() * (db - lo) / -lo;
            g.setColour (Col::faint);
            g.drawHorizontalLine ((int) y, a.getX(), a.getRight());
            g.setColour (Col::dim);
            g.drawText (juce::String ((int) db), juce::Rectangle<float> (a.getRight() + 2.0f, y - 6.0f, 24.0f, 12.0f), juce::Justification::centredRight);
        }
        // the curve, and the fill under it up to the current input level
        juce::Path curve, fill;
        const float levelX = juce::jlimit (lo, 0.0f, level);
        fill.startNewSubPath (a.getX(), a.getBottom());
        for (int i = 0; i <= 96; ++i)
        {
            const float inDb = lo + -lo * (float) i / 96.0f;
            const auto p = pt (inDb);
            if (i == 0) curve.startNewSubPath (p); else curve.lineTo (p);
            if (inDb <= levelX) fill.lineTo (p);
        }
        const auto lp = pt (levelX);
        fill.lineTo (lp);
        fill.lineTo (lp.x, a.getBottom());
        fill.closeSubPath();
        g.setGradientFill (juce::ColourGradient (Col::accent.withAlpha (0.35f), lp.x, a.getY(), Col::accent.withAlpha (0.04f), a.getX(), a.getBottom(), false));
        g.fillPath (fill);
        g.setColour (Col::accent);
        g.strokePath (curve, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        // the knee handle at the threshold
        {
            const auto k = pt (-42.0f * amount);
            g.setColour (Col::bg);
            g.fillEllipse (k.x - 6.0f, k.y - 6.0f, 12.0f, 12.0f);
            g.setColour (Col::accent);
            g.drawEllipse (k.x - 6.0f, k.y - 6.0f, 12.0f, 12.0f, 1.6f);
            g.fillEllipse (k.x - 2.7f, k.y - 2.7f, 5.4f, 5.4f);
        }
        // the incoming level: a faint line where the fill ends
        if (level > lo + 0.5f)
        {
            g.setColour (Col::accent.withAlpha (0.25f));
            g.drawVerticalLine ((int) lp.x, a.getY(), a.getBottom());
        }
        g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
        g.setColour (Col::dim);
        g.drawText (juce::String::fromUTF8 ("IN \xe2\x96\xb6"), r.reduced (5.0f, 3.0f).toNearestInt(), juce::Justification::bottomRight);
        g.setColour (Col::border);
        g.drawRoundedRectangle (r.reduced (0.5f), 4.0f, 1.0f);
        if (! on) g.endTransparencyLayer();
    }
private:
    ElukkaNoiseProcessor& proc;
    float level = -100.0f;
};
}

EffectsPage::EffectsPage (ElukkaNoiseProcessor& p) : Page (p)
{
    const char* names[P::numFx] { "COMPRESSOR", "DELAY", "REVERB" };
    for (int k = 0; k < P::numFx; ++k)
    {
        auto sec = std::make_unique<Section> (names[k]);
        sections[k] = sec.get();
        addAndMakeVisible (*sec);
        sec->onGrab = [this, k] (int y)
        {
            dragging = k;
            grabOffset = y - sections[k]->getY();
            const int o = order();
            for (int i = 0; i < P::numFx; ++i) dragOrder[i] = P::fxOrders[o][i];
            sections[k]->toFront (false);
        };
        sec->onDrag = [this, k] (int y)
        {
            // the dragged panel follows the mouse; the others slide to the places the new order gives them
            auto* s = sections[k];
            const int top = juce::jlimit (0, getHeight() - s->getHeight(), y - grabOffset);
            s->setTopLeftPosition (s->getX(), top);
            // the position whose slot centre is nearest the dragged panel's centre
            int rest[P::numFx - 1], n = 0;
            for (int i = 0; i < P::numFx; ++i) if (dragOrder[i] != k) rest[n++] = dragOrder[i];
            int best[P::numFx] {}, bestDist = 1 << 30;
            for (int t = 0; t < P::numFx; ++t)
            {
                int cand[P::numFx], r = 0;
                for (int i = 0; i < P::numFx; ++i) cand[i] = i == t ? k : rest[r++];
                const int dist = std::abs (slotIn (cand, t).getCentreY() - (top + s->getHeight() / 2));
                if (dist < bestDist) { bestDist = dist; std::copy (cand, cand + P::numFx, best); }
            }
            bool changed = false;
            for (int i = 0; i < P::numFx; ++i) changed = changed || best[i] != dragOrder[i];
            if (changed)
                for (int i = 0; i < P::numFx; ++i)
                    if (best[i] != k)
                        juce::Desktop::getInstance().getAnimator().animateComponent (sections[best[i]], slotIn (best, i), 1.0f, 140, false, 0.0, 0.0);
            std::copy (best, best + P::numFx, dragOrder);
        };
        sec->onDrop = [this, k]
        {
            const int dropped = P::fxOrderIndex (dragOrder);
            if (auto* prm = proc.apvts.getParameter ("fx_order"); prm != nullptr && dropped != order())   // a click without a move writes nothing
            {
                prm->beginChangeGesture();
                prm->setValueNotifyingHost (prm->convertTo0to1 ((float) dropped));
                prm->endChangeGesture();
            }
            dragging = -1;
            place (true);   // the dropped panel glides into its slot
            juce::ignoreUnused (k);
        };
        owned.push_back (std::move (sec));
    }
    auto inSection = [this] (int k, juce::Component& c) { sections[k]->addAndMakeVisible (c); };

    cOn = &makeToggle ("comp_on", "On", "Compressor on / off (switching glides, so it never clicks)");
    cAmt = &makeKnob ("comp_amt", "Amount", "Compression: 0% = off. Threshold and ratio rise together (up to -42 dB at 8:1)");
    cSpeed = &makeKnob ("comp_speed", "Speed", "Slow = smooth glue for sweeps (30 ms attack, 400 ms release). Fast = punchy pumping on gated noise (1 ms / 30 ms)");
    cMakeup = &makeToggle ("comp_makeup", "Makeup", "Glides the gain back towards the loudness before compression (about 1 s averages)");
    auto m = std::make_unique<CompGraph> (p);
    meter = m.get();
    owned.push_back (std::move (m));
    for (juce::Component* c : { (juce::Component*) cOn, (juce::Component*) cAmt, (juce::Component*) cSpeed, (juce::Component*) cMakeup, meter }) inSection (0, *c);

    dOn = &makeToggle ("dly_on", "On");
    dSync = &makeToggle ("dly_sync", "Sync", "Delay time in note values of the host tempo");
    dTime = &makeKnob ("dly_time", "Time", "Delay time (a change fades to the new time in 30 ms: exact at once, no pitch bend)");
    dDiv = &makeKnob ("dly_division", "Time", "Delay time in note values, 1/1 to 1/32 (D = dotted, T = triplet)");
    dFb = &makeKnob ("dly_fb", "Feedback", "How much of each echo comes back. 100 % sustains: a limiter in the loop keeps it from running away");
    dFilter = &makeKnob ("dly_filter", "Filter", "Tone of the repeats: left = each echo darker (low pass closing), centre = neutral, right = each echo thinner and brighter (high pass rising)");
    dWidth = &makeKnob ("dly_width", "Width", "Stereo spread of the echoes: 0 = mono, 100 % = full ping-pong (the mono sum never cancels)");
    dOffset = &makeKnob ("dly_offset", "Offset", "Right channel's time against the left: -50 % .. +50 % (a swing or a dotted feel between the sides)");
    dWet = &makeKnob ("dly_wet", "Mix", "Delay mix (the DELAY knob on the main screen)");
    dMod = &makeKnob ("dly_mod", "Mod", "Modulation of the delay time (up to 6 ms, left and right in quadrature): subtle wobble to seasick. The rate follows: slow at low depth, 3 Hz at full");
    dDuck = &makeKnob ("dly_duck", "Duck", "Ducking: the dry signal pushes the echoes down while it plays, so gated pulses stay sharp and the echoes fill the gaps");
    dMode.addItemList (P::dlyModeNames, 1);
    dMode.setTooltip ("Mono: one line, centred echoes. Stereo: left and right lines. Ping-Pong: echoes alternate sides (feedback once per round trip). Wide Ping-Pong: the stereo input kept, every echo swaps sides");
    dModeAtt = std::make_unique<APVTS::ComboBoxAttachment> (state(), "dly_mode", dMode);
    dPreset.onClick = [this] { showFxMenu (proc.dlyPresets, dPreset); };
    dPreset.setTooltip ("Delay preset: the delay's knobs and how MACRO moves them. Independent of the instrument preset (and of the reverb)");
    dMacro.onClick = [this] { showMapEditor (proc.dlyPresets, dMacro); };
    dMacro.setTooltip ("How MACRO moves the delay (part of the delay preset)");
    {
        Knob* dk[P::numDlyMapDests] { dWet, dFb, dFilter, dWidth, dOffset, dMod, dDuck };   // in P::dlyMapDests order
        for (int d = 0; d < P::numDlyMapDests; ++d)
            dk[d]->setFxMap (&proc.engine.uiDlyMap[d], [this, d] { return proc.dlyPresets.map[d]; });
    }
    for (juce::Component* c : { (juce::Component*) dOn, (juce::Component*) dSync, (juce::Component*) &dMode, (juce::Component*) &dPreset, (juce::Component*) &dMacro,
                                (juce::Component*) dTime, (juce::Component*) dDiv, (juce::Component*) dFb, (juce::Component*) dFilter, (juce::Component*) dWidth,
                                (juce::Component*) dOffset, (juce::Component*) dWet, (juce::Component*) dMod, (juce::Component*) dDuck, (juce::Component*) &dViews }) inSection (1, *c);

    rOn = &makeToggle ("rev_on", "On");
    rSize = &makeKnob ("rev_size", "Size");
    rDecay = &makeKnob ("rev_decay", "Decay");
    rTone = &makeKnob ("rev_tone", "Tone", "Tone of the tail: left = darker (the highs die faster), centre = neutral, right = thinner (the lows removed)");
    rWet = &makeKnob ("rev_wet", "Mix", "Reverb mix (the REVERB knob on the main screen)");
    rPre = &makeKnob ("rev_pre", "Pre", "Pre-delay: a gap before the reverb starts, so a gated pulse stays separate from its tail");
    rWidth = &makeKnob ("rev_width", "Width", "Stereo spread of the tail: 0 = mono, 100 % = full (the sum to mono never cancels)");
    rDuck = &makeKnob ("rev_duck", "Duck", "Ducking: the dry signal pushes the reverb down while it plays, so gated pulses stay sharp and the tail blooms between them");
    rMod = &makeKnob ("rev_mod", "Mod", "Modulation of the tail: a little keeps it from ringing, a lot makes it swirl. The rate follows: slow at low depth, 3 Hz at full");
    rPreset.onClick = [this] { showFxMenu (proc.revPresets, rPreset); };
    rPreset.setTooltip ("Reverb preset: the reverb's knobs, its character and how MACRO moves them. Independent of the instrument preset (and of the delay)");
    rMacro.onClick = [this] { showMapEditor (proc.revPresets, rMacro); };
    rMacro.setTooltip ("How MACRO moves the reverb (part of the reverb preset)");
    {
        Knob* rk[P::numRevMapDests] { rWet, rDecay, rSize, rTone, rWidth, rMod, rPre, rDuck };   // in P::revMapDests order
        for (int d = 0; d < P::numRevMapDests; ++d)
            rk[d]->setFxMap (&proc.engine.uiRevMap[d], [this, d] { return proc.revPresets.map[d]; });
    }
    for (juce::Component* c : { (juce::Component*) rOn, (juce::Component*) rSize, (juce::Component*) rDecay, (juce::Component*) rWet,
                                (juce::Component*) rPre, (juce::Component*) rTone, (juce::Component*) rWidth,
                                (juce::Component*) rDuck, (juce::Component*) rMod, (juce::Component*) &rPreset, (juce::Component*) &rMacro, (juce::Component*) &rViews }) inSection (2, *c);
    updateFxButtons();
    tick();
}

int EffectsPage::order() const { return juce::jlimit (0, 5, (int) proc.apvts.getRawParameterValue ("fx_order")->load()); }

int EffectsPage::heightOf (int fx) const
{
    // one knob row each (v0.9.23: the delay's switches sit in its title strip and its first view); the compressor's graph row is the same height
    constexpr int gap = 6, compH = 100, delayH = 104;
    return fx == 0 ? compH : fx == 1 ? delayH : getHeight() - compH - delayH - 2 * gap;
}

juce::Rectangle<int> EffectsPage::slotIn (const int* ord, int i) const
{
    int y = 0;
    for (int j = 0; j < i; ++j) y += heightOf (ord[j]) + 6;
    return { 0, y, getWidth(), heightOf (ord[i]) };
}

void EffectsPage::place (bool animate)
{
    shownOrder = order();
    const char* names[P::numFx] { "COMPRESSOR", "DELAY", "REVERB" };
    for (int i = 0; i < P::numFx; ++i)
    {
        const int k = P::fxOrders[shownOrder][i];
        sections[k]->panel.title = juce::String (i + 1) + "   " + names[k];
        sections[k]->panel.repaint();
        const auto target = slotIn (P::fxOrders[shownOrder], i);
        auto& animator = juce::Desktop::getInstance().getAnimator();
        if (animate) animator.animateComponent (sections[k], target, 1.0f, 140, false, 0.0, 0.0);
        else { animator.cancelAnimation (sections[k], false); sections[k]->setBounds (target); }
    }
}

void EffectsPage::layoutSections()
{
    // inside each section (section coordinates): controls spread evenly over the panel's content row
    const auto row = [this] (int k) { return sections[k]->content(); };
    dDiv->setBounds (0, 0, 0, 0);
    const int graphH = row (0).getHeight();
    spread (row (0), { toggleCell (*cOn, 54), knobCell (*cAmt), knobCell (*cSpeed), toggleCell (*cMakeup, 86),
                       { meter, juce::jmax (150, juce::roundToInt (graphH * 2.4f)), graphH } }, 40);
    // delay and reverb: On, the preset menu and MACRO in the title strip with the TIME / TONE buttons; one knob row per view
    auto strip = [this] (int k, Toggle& on, juce::TextButton& preset, juce::TextButton& macro, ViewSwitch& views)
    {
        const int w = sections[k]->getWidth();
        preset.setBounds (w - 8 - 160, 3, 160, 18);
        macro.setBounds (preset.getX() - 6 - 60, 3, 60, 18);
        on.setBounds (macro.getX() - 8 - 50, 0, 50, 24);
        views.setBounds (sections[k]->panel.titleEnd() + 14, 3, views.idealWidth(), 18);
    };
    strip (1, *dOn, dPreset, dMacro, dViews);
    strip (2, *rOn, rPreset, rMacro, rViews);
    const bool dTimeView = dViews.index == 0, rTimeView = rViews.index == 0;
    for (juce::Component* c : { (juce::Component*) dSync, (juce::Component*) &dMode, (juce::Component*) dFb, (juce::Component*) dWet, (juce::Component*) dDuck }) c->setVisible (dTimeView);
    for (juce::Component* c : { (juce::Component*) dFilter, (juce::Component*) dWidth, (juce::Component*) dOffset, (juce::Component*) dMod }) c->setVisible (! dTimeView);
    if (dTimeView) spread (row (1), { toggleCell (*dSync, 62), { &dMode, 124, 22 }, knobCell (*dTime), knobCell (*dFb), knobCell (*dWet), knobCell (*dDuck) }, 40);
    else spread (row (1), { knobCell (*dFilter), knobCell (*dWidth), knobCell (*dOffset), knobCell (*dMod) }, 40);
    dDiv->setBounds (dTime->getBounds());
    if (! dTimeView) { dTime->setVisible (false); dDiv->setVisible (false); }
    else syncSwap (dTime, dDiv, raw ("dly_sync"));
    for (juce::Component* c : { (juce::Component*) rSize, (juce::Component*) rDecay, (juce::Component*) rPre, (juce::Component*) rWet, (juce::Component*) rDuck }) c->setVisible (rTimeView);
    for (juce::Component* c : { (juce::Component*) rTone, (juce::Component*) rWidth, (juce::Component*) rMod }) c->setVisible (! rTimeView);
    if (rTimeView) spread (row (2), { knobCell (*rSize), knobCell (*rDecay), knobCell (*rPre), knobCell (*rWet), knobCell (*rDuck) }, 40);
    else spread (row (2), { knobCell (*rTone), knobCell (*rWidth), knobCell (*rMod) }, 40);
}

void EffectsPage::resized()
{
    for (int k = 0; k < P::numFx; ++k) sections[k]->setSize (getWidth(), heightOf (k));
    layoutSections();
    place (false);
}

void EffectsPage::updateFxButtons()
{
    auto text = [] (FxPresetManager& m) { return m.currentName + juce::String::fromUTF8 ("  \xe2\x96\xbe"); };
    if (dPreset.getButtonText() != text (proc.dlyPresets)) dPreset.setButtonText (text (proc.dlyPresets));
    if (rPreset.getButtonText() != text (proc.revPresets)) rPreset.setButtonText (text (proc.revPresets));
}

void EffectsPage::showFxMenu (FxPresetManager& m, juce::Component& target)
{
    m.refresh();
    juce::PopupMenu menu;
    menu.addSectionHeader (juce::String (m.title()) + " presets");
    bool userHeader = false;
    for (int i = 0; i < (int) m.entries.size(); ++i)
    {
        auto& e = m.entries[(size_t) i];
        if (! e.isFactory && ! userHeader) { menu.addSeparator(); menu.addSectionHeader ("User"); userHeader = true; }
        menu.addItem (i + 1, e.name, true, i == m.currentIndex);
    }
    menu.addSeparator();
    constexpr int saveId = 100000, deleteId = 100001;
    menu.addItem (saveId, "Save as...");
    const bool isUser = m.currentIndex >= 0 && ! m.entries[(size_t) m.currentIndex].isFactory;
    menu.addItem (deleteId, "Delete \"" + m.currentName + "\"", isUser);
    juce::Component::SafePointer<EffectsPage> safe (this);
    FxPresetManager* mp = &m;
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&target).withMinimumWidth (juce::jmax (180, target.getWidth())),
                        [safe, mp] (int result)
                        {
                            if (safe == nullptr || result <= 0) return;
                            if (result == saveId) safe->saveFxDialog (*mp);
                            else if (result == deleteId) safe->deleteFxDialog (*mp);
                            else mp->load (result - 1);
                            safe->updateFxButtons();
                        });
}

void EffectsPage::saveFxDialog (FxPresetManager& m)
{
    auto* w = new juce::AlertWindow (juce::String ("Save ") + m.title() + " Preset", "Saved with its Macro mapping in " + m.userDir().getFullPathName(), juce::MessageBoxIconType::NoIcon);
    w->addTextEditor ("name", m.currentName, "Name:");
    w->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    juce::Component::SafePointer<EffectsPage> safe (this);
    FxPresetManager* mp = &m;
    if (auto* host = findParentComponentOfClass<DialogHost>()) host->track (*w);
    w->enterModalState (true, juce::ModalCallbackFunction::create ([safe, w, mp] (int result)
    {
        if (safe == nullptr || result != 1) return;
        const auto name = PresetManager::legalName (w->getTextEditorContents ("name"));
        juce::String problem;
        if (name.isEmpty()) problem = "Enter a name.";
        else if (mp->indexOf (name, true) >= 0) problem = "\"" + name + "\" is a factory preset: choose another name.";
        else if (! mp->save (name)) problem = "Could not write the preset file.";
        if (problem.isNotEmpty())
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, juce::String ("Save ") + mp->title() + " Preset", problem);
        safe->updateFxButtons();
    }), true);
}

void EffectsPage::deleteFxDialog (FxPresetManager& m)
{
    const int index = m.currentIndex;
    if (index < 0 || m.entries[(size_t) index].isFactory) return;
    const auto file = m.entries[(size_t) index].file;
    auto* w = new juce::AlertWindow (juce::String ("Delete ") + m.title() + " Preset", "Delete \"" + m.currentName + "\"?\nThe file is moved to the Trash.", juce::MessageBoxIconType::NoIcon);
    w->addButton ("Delete", 1);   // Return and Escape both cancel: only a click deletes
    w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    juce::Component::SafePointer<EffectsPage> safe (this);
    FxPresetManager* mp = &m;
    if (auto* host = findParentComponentOfClass<DialogHost>()) host->track (*w);
    w->enterModalState (true, juce::ModalCallbackFunction::create ([safe, mp, index, file] (int result)
    {
        if (safe == nullptr || result != 1) return;
        if (index >= (int) mp->entries.size() || mp->entries[(size_t) index].file != file || ! mp->deletePreset (index))
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Delete Preset", "Could not delete the preset.");
        safe->updateFxButtons();
    }), true);
}

void EffectsPage::showMapEditor (FxPresetManager& m, juce::Component& target)
{
    // inside the plugin window, so it stays on top and can take keyboard focus in hosts
    auto* top = getTopLevelComponent();
    const auto area = top->getLocalArea (&target, target.getLocalBounds());
    auto content = std::make_unique<FxMapEditor> (state(), m, top->getWidth() - 40, top->getHeight() - 40);
    if (auto* host = findParentComponentOfClass<DialogHost>()) host->launch (std::move (content), area, top);
    else juce::CallOutBox::launchAsynchronously (std::move (content), area, top);
}

void EffectsPage::tick()
{
    Page::tick();
    if (dViews.index == 0) syncSwap (dTime, dDiv, raw ("dly_sync"));
    updateFxButtons();
    static_cast<CompGraph*> (meter)->tick();
    // the order changed (preset, host, undo): slide into place (no animation before the page has a size)
    if (order() != shownOrder && dragging < 0) place (shownOrder >= 0 && getHeight() > 0);
}

//==============================================================================
// Modulation matrix

namespace
{
// Bipolar bar for a slot's amount (drag left / right, Shift = fine, double-click = invert)
class AmountBar : public juce::Component, public juce::SettableTooltipClient
{
public:
    AmountBar (APVTS& s, int k) : param (*s.getParameter (P::mx (k, "amt"))), attachment (param, [this] (float) { repaint(); })
    {
        attachment.sendInitialUpdate();
        setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
        setTooltip ("Amount: drag left or right (Shift = fine). Double-click inverts it");
    }
    bool dragging = false;
    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (Col::bg);
        g.fillRoundedRectangle (r, 3.0f);
        const float v = param.convertFrom0to1 (param.getValue()), cx = r.getCentreX();
        g.setColour (Col::accent.withAlpha (0.8f));
        g.fillRect (juce::Rectangle<float>::leftTopRightBottom (juce::jmin (cx, cx + v * r.getWidth() * 0.5f), r.getY() + 1.0f,
                                                                 juce::jmax (cx, cx + v * r.getWidth() * 0.5f), r.getBottom() - 1.0f));
        g.setColour (Col::dim);
        g.drawVerticalLine ((int) cx, r.getY(), r.getBottom());
        g.setColour (Col::border);
        g.drawRoundedRectangle (r.reduced (0.5f), 3.0f, 1.0f);
        g.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
        g.setColour (Col::text);
        const auto text = (v > 0.0f ? "+" : "") + juce::String (juce::roundToInt (v * 100.0f)) + "%";
        auto tr = v >= 0.0f ? r.withTrimmedRight (r.getWidth() * 0.5f).reduced (5.0f, 0.0f) : r.withTrimmedLeft (r.getWidth() * 0.5f).reduced (5.0f, 0.0f);
        g.drawText (text, tr, v >= 0.0f ? juce::Justification::centredRight : juce::Justification::centredLeft);
    }
    void mouseDown (const juce::MouseEvent&) override { start = param.convertFrom0to1 (param.getValue()); dragging = true; attachment.beginGesture(); }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        const float scale = (e.mods.isShiftDown() ? 0.2f : 1.0f) * 2.0f / (float) getWidth();
        attachment.setValueAsPartOfGesture (juce::jlimit (-1.0f, 1.0f, start + (float) e.getDistanceFromDragStartX() * scale));
    }
    void mouseUp (const juce::MouseEvent&) override { attachment.endGesture(); dragging = false; }
    void mouseDoubleClick (const juce::MouseEvent&) override { attachment.setValueAsCompleteGesture (-param.convertFrom0to1 (param.getValue())); }
private:
    juce::RangedAudioParameter& param;
    juce::ParameterAttachment attachment;
    float start = 0.0f;
};

// A slot's curve (drag up / down, double-click = straight)
class CurveBox : public juce::Component, public juce::SettableTooltipClient
{
public:
    CurveBox (APVTS& s, int k) : param (*s.getParameter (P::mx (k, "crv"))), attachment (param, [this] (float) { repaint(); })
    {
        attachment.sendInitialUpdate();
        setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
        setTooltip ("Curve: drag up for an early move, down for a late one. Double-click for straight");
    }
    bool dragging = false;
    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (Col::bg);
        g.fillRoundedRectangle (r, 3.0f);
        g.setColour (Col::border);
        g.drawRoundedRectangle (r.reduced (0.5f), 3.0f, 1.0f);
        auto a = r.reduced (4.0f);
        const float c = param.convertFrom0to1 (param.getValue());
        juce::Path p;
        for (int i = 0; i <= 24; ++i)
        {
            const float m = (float) i / 24.0f;
            const juce::Point<float> q (a.getX() + a.getWidth() * m, a.getBottom() - a.getHeight() * P::shapeMacro (m, c));
            if (i == 0) p.startNewSubPath (q); else p.lineTo (q);
        }
        g.setColour (Col::text);
        g.strokePath (p, juce::PathStrokeType (1.5f));
    }
    void mouseDown (const juce::MouseEvent&) override { start = param.convertFrom0to1 (param.getValue()); dragging = true; attachment.beginGesture(); }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        const float scale = e.mods.isShiftDown() ? 0.002f : 0.01f;
        attachment.setValueAsPartOfGesture (juce::jlimit (-1.0f, 1.0f, start - (float) e.getDistanceFromDragStartY() * scale));
    }
    void mouseUp (const juce::MouseEvent&) override { attachment.endGesture(); dragging = false; }
    void mouseDoubleClick (const juce::MouseEvent&) override { attachment.setValueAsCompleteGesture (0.0f); }
private:
    juce::RangedAudioParameter& param;
    juce::ParameterAttachment attachment;
    float start = 0.0f;
};

// One matrix row
class MatrixRow : public juce::Component
{
public:
    MatrixRow (ElukkaNoiseProcessor& p, int k) : proc (p), slot (k), amount (p.apvts, k), curve (p.apvts, k)
    {
        auto& s = p.apvts;
        number.setText (juce::String (k + 1), juce::dontSendNotification);
        number.setColour (juce::Label::textColourId, Col::dim);
        number.setJustificationType (juce::Justification::centred);
        source.addItemList (P::modSources, 1);
        source.setTooltip ("Source");
        srcAtt = std::make_unique<APVTS::ComboBoxAttachment> (s, P::mx (k, "src"), source);
        aux.addItemList ({ "---", "Macro", "LFO 1", "LFO 2", "S&H", "Velocity" }, 1);
        aux.setTooltip ("Aux source: scales the amount (Macro here = the macro decides how much the source moves the destination)");
        auxAtt = std::make_unique<APVTS::ComboBoxAttachment> (s, P::mx (k, "aux"), aux);
        // destinations grouped by section; the item id is the parameter's choice index + 1
        dest.addItem ("None", 1);
        for (auto* group : { "Filter", "Noise", "LFOs", "Random", "Gate", "Compressor", "Delay", "Reverb", "Output" })
        {
            dest.addSectionHeading (group);
            for (int d = 0; d < P::NumDst; ++d)
                if (juce::String (P::destGroup (d)) == group) dest.addItem (P::dests[d].name, d + 2);
        }
        dest.setTooltip ("Destination");
        dest.onChange = [this] { if (const int id = dest.getSelectedId(); id > 0) Mod::setParam (proc.apvts, P::mx (slot, "dst"), (float) (id - 1)); };
        clear.setButtonText (juce::String::fromUTF8 ("\xc3\x97"));
        clear.setTooltip ("Clear this row");
        clear.onClick = [this] { Mod::clear (proc.apvts, slot); };
        for (juce::Component* c : { (juce::Component*) &number, (juce::Component*) &source, (juce::Component*) &curve, (juce::Component*) &amount,
                                    (juce::Component*) &dest, (juce::Component*) &aux, (juce::Component*) &clear })
            addAndMakeVisible (c);
        tick();
    }
    bool busy() const { return amount.dragging || curve.dragging || source.isPopupActive() || dest.isPopupActive() || aux.isPopupActive(); }
    void tick()
    {
        const int id = Mod::dst (proc.apvts, slot) + 2;
        if (dest.getSelectedId() != id) dest.setSelectedId (id, juce::dontSendNotification);
        const bool used = Mod::used (proc.apvts, slot);
        if (used != wasUsed)
        {
            wasUsed = used;
            // an empty row shows only its source box (pick a source to start a new routing)
            for (juce::Component* c : { (juce::Component*) &curve, (juce::Component*) &amount, (juce::Component*) &dest, (juce::Component*) &aux, (juce::Component*) &clear })
                c->setVisible (used);
            repaint();
        }
    }
    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (wasUsed ? Col::panelHi : Col::panel);
        g.fillRoundedRectangle (r, 3.0f);
        if (! wasUsed)
        {
            g.setColour (Col::dim);
            g.setFont (juce::Font (juce::FontOptions (10.5f)));
            g.drawText ("<  pick a source to add a routing", r.withTrimmedLeft ((float) columnX[2]), juce::Justification::centredLeft);
        }
    }
    void resized() override
    {
        juce::Component* cols[] { &number, &source, &curve, &amount, &dest, &aux, &clear };
        for (int i = 0; i < 7; ++i) cols[i]->setBounds (columnX[i], 3, columnW[i], getHeight() - 6);
    }
    // columns (x, width) shared by the rows and the headings
    static constexpr int columnW[] { 22, 100, 38, 168, 156, 98, 24 };
    static constexpr int columnX[] { 4, 4 + 22 + 6, 4 + 22 + 6 + 100 + 6, 4 + 22 + 6 + 100 + 6 + 38 + 6, 4 + 22 + 6 + 100 + 6 + 38 + 6 + 168 + 6,
                                     4 + 22 + 6 + 100 + 6 + 38 + 6 + 168 + 6 + 156 + 6, 4 + 22 + 6 + 100 + 6 + 38 + 6 + 168 + 6 + 156 + 6 + 98 + 6 };

private:
    ElukkaNoiseProcessor& proc;
    int slot;
    bool wasUsed = true;
    juce::Label number;
    juce::ComboBox source, dest, aux;
    std::unique_ptr<APVTS::ComboBoxAttachment> srcAtt, auxAtt;
    AmountBar amount;
    CurveBox curve;
    juce::TextButton clear;
};
}

namespace
{
// Macro range: two handles (floor and ceiling) on one bar. Drag a handle, or the middle to move both; double-click
// resets to 0..100 %.
class MacroRange : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit MacroRange (APVTS& s) : lo (*s.getParameter ("macro_lo")), hi (*s.getParameter ("macro_hi")),
        loAtt (lo, [this] (float) { repaint(); }), hiAtt (hi, [this] (float) { repaint(); })
    {
        loAtt.sendInitialUpdate();
        hiAtt.sendInitialUpdate();
        setTooltip ("Macro range: the macro's 0..100 % plays this part of its travel. Raise the floor if nothing happens "
                    "below, say, 10 %, and your DAW automation can still run from 0 to 100 %. Double-click resets");
        setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
    }
    float loV() const { return lo.convertFrom0to1 (lo.getValue()); }
    float hiV() const { return hi.convertFrom0to1 (hi.getValue()); }
    void paint (juce::Graphics& g) override
    {
        auto r = bar().toFloat();
        g.setColour (Col::bg);
        g.fillRoundedRectangle (r, 3.0f);
        g.setColour (Col::border);
        g.drawRoundedRectangle (r.reduced (0.5f), 3.0f, 1.0f);
        const float x0 = xFor (loV()), x1 = xFor (hiV());
        g.setColour (Col::accent.withAlpha (0.25f));
        g.fillRect (juce::Rectangle<float>::leftTopRightBottom (x0, r.getY() + 1.0f, x1, r.getBottom() - 1.0f));
        g.setColour (Col::accent);
        for (float x : { x0, x1 }) g.fillRoundedRectangle (x - 2.5f, r.getY() - 2.0f, 5.0f, r.getHeight() + 4.0f, 2.0f);
        g.setColour (Col::text);
        g.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
        g.drawText (juce::String (juce::roundToInt (loV())) + "%  -  " + juce::String (juce::roundToInt (hiV())) + "%", r.toNearestInt(), juce::Justification::centred);
    }
    void mouseDown (const juce::MouseEvent& e) override
    {
        const float x = (float) e.x, d0 = std::abs (x - xFor (loV())), d1 = std::abs (x - xFor (hiV()));
        mode = d0 < 8.0f && d0 <= d1 ? 0 : d1 < 8.0f ? 1 : (x > xFor (loV()) && x < xFor (hiV()) ? 2 : (d0 < d1 ? 0 : 1));
        startLo = loV(); startHi = hiV();
        loAtt.beginGesture(); hiAtt.beginGesture();
        mouseDrag (e);
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        const float pct = (float) e.getDistanceFromDragStartX() / (float) bar().getWidth() * 100.0f;
        const float at = juce::jlimit (0.0f, 100.0f, ((float) e.x - (float) bar().getX()) / (float) bar().getWidth() * 100.0f);
        if (mode == 0) loAtt.setValueAsPartOfGesture (juce::jmin (at, hiV() - 1.0f));
        else if (mode == 1) hiAtt.setValueAsPartOfGesture (juce::jmax (at, loV() + 1.0f));
        else
        {
            const float shift = juce::jlimit (-startLo, 100.0f - startHi, pct);
            loAtt.setValueAsPartOfGesture (startLo + shift);
            hiAtt.setValueAsPartOfGesture (startHi + shift);
        }
    }
    void mouseUp (const juce::MouseEvent&) override { loAtt.endGesture(); hiAtt.endGesture(); }
    void mouseDoubleClick (const juce::MouseEvent&) override { loAtt.setValueAsCompleteGesture (0.0f); hiAtt.setValueAsCompleteGesture (100.0f); }
private:
    juce::Rectangle<int> bar() const { return getLocalBounds().reduced (3, 2); }
    float xFor (float v) const { auto b = bar(); return (float) b.getX() + (float) b.getWidth() * v * 0.01f; }
    juce::RangedAudioParameter &lo, &hi;
    juce::ParameterAttachment loAtt, hiAtt;
    int mode = 0;
    float startLo = 0.0f, startHi = 100.0f;
};
}

MatrixPage::MatrixPage (ElukkaNoiseProcessor& p) : Page (p)
{
    panel = &makePanel ("MOD MATRIX");
    rangeLabel.setText ("MACRO RANGE", juce::dontSendNotification);
    rangeLabel.setColour (juce::Label::textColourId, Col::dim);
    rangeLabel.setFont (juce::Font (juce::FontOptions (10.5f, juce::Font::bold)));
    rangeLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (rangeLabel);
    auto mr = std::make_unique<MacroRange> (state());
    range = mr.get();
    addAndMakeVisible (*range);
    owned.push_back (std::move (mr));
    help.setText ("Right-click any knob > Modulate with... adds a row. Aux scales the amount.", juce::dontSendNotification);
    help.setColour (juce::Label::textColourId, Col::dim);
    help.setFont (juce::Font (juce::FontOptions (11.0f)));
    help.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (help);
    viewport.setViewedComponent (&list, false);
    viewport.setScrollBarsShown (true, false);
    viewport.setScrollBarThickness (8);
    addAndMakeVisible (viewport);
    rebuild();
}

MatrixPage::~MatrixPage() { viewport.setViewedComponent (nullptr, false); }

std::vector<int> MatrixPage::wanted() const
{
    // the rows in use, then the first free one (to add a new row)
    std::vector<int> v;
    int firstFree = -1;
    for (int k = 0; k < P::numMatrixSlots; ++k)
    {
        if (Mod::used (proc.apvts, k)) v.push_back (k);
        else if (firstFree < 0) firstFree = k;
    }
    if (firstFree >= 0) v.push_back (firstFree);
    return v;
}

void MatrixPage::rebuild()
{
    rowComps.clear();
    shown = wanted();
    for (int k : shown)
    {
        auto row = std::make_unique<MatrixRow> (proc, k);
        list.addAndMakeVisible (*row);
        rowComps.push_back (std::move (row));
    }
    int used = 0;
    for (int k = 0; k < P::numMatrixSlots; ++k) used += Mod::used (proc.apvts, k) ? 1 : 0;
    panel->title = "MOD MATRIX   " + juce::String (used) + " / " + juce::String (P::numMatrixSlots) + " ROWS";
    panel->repaint();
    resized();
    repaint();
}

void MatrixPage::resized()
{
    panel->setBounds (getLocalBounds());
    // title row: the macro range on the right; the help line goes below the rows
    range->setBounds (panel->getRight() - 214, panel->getY() + 4, 204, 20);
    rangeLabel.setBounds (range->getX() - 96, range->getY(), 92, 20);
    auto r = panel->contentInParent().withTrimmedTop (6);
    help.setBounds (r.removeFromBottom (16));
    r.removeFromTop (18);   // column headings (painted)
    viewport.setBounds (r);
    const int rowH = 32;
    list.setSize (r.getWidth() - 10, juce::jmax (1, (int) rowComps.size() * (rowH + 3)));
    for (size_t i = 0; i < rowComps.size(); ++i)
        rowComps[i]->setBounds (0, (int) i * (rowH + 3), list.getWidth(), rowH);
}

void MatrixPage::paintOverChildren (juce::Graphics& g)
{
    g.setColour (Col::dim);
    g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
    const char* heads[] { "#", "SOURCE", "CURVE", "AMOUNT", "DESTINATION", "AUX SOURCE", "" };
    const int x0 = viewport.getX(), y = viewport.getY() - 18;
    for (int i = 0; i < 7; ++i)
        g.drawText (heads[i], x0 + MatrixRow::columnX[i], y, MatrixRow::columnW[i], 16, i == 0 ? juce::Justification::centred : juce::Justification::centredLeft);
}

void MatrixPage::tick()
{
    Page::tick();
    if (wanted() != shown)
    {
        bool busy = false;
        for (auto& c : rowComps) busy = busy || static_cast<MatrixRow*> (c.get())->busy();
        if (! busy) rebuild();
    }
    for (auto& c : rowComps) static_cast<MatrixRow*> (c.get())->tick();
}
