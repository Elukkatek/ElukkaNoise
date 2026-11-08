// Offline tests for ElukkaNoise: renders every factory preset through a macro rise and fall and checks for NaNs,
// silence, clipping, clicks and mono compatibility; checks stepped macro automation, note handling, preset switching
// mid-note, buffer-size and sample-rate consistency, state round trips; benchmarks CPU; loads the built VST3 like a
// host; snapshots the editor.
//
//   ElukkaNoiseTest                 run the test suite (exit code 1 on failure)
//   ElukkaNoiseTest --bench         CPU benchmark
//   ElukkaNoiseTest --probe-reverb [dir]  reverb measurements (level, T60, stereo, flutter, ripple)
//   ElukkaNoiseTest --host <vst3>   load the bundle through JUCE's VST3 hosting
//   ElukkaNoiseTest --ui <dir>      save editor snapshots as PNG
//   ElukkaNoiseTest --wav <dir>     render listening files for every preset
#include <JuceHeader.h>
#include "../Source/PluginEditor.h"
#include "../Source/UI/FxMapEditor.h"
#include "ClickProbe.h"
#include "BinaryData.h"
#include <iostream>
#include <iomanip>

struct FakePlayHead : juce::AudioPlayHead
{
    double ppq = 0.0, bpm = 130.0;
    bool playing = true;
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo p;
        p.setBpm (bpm);
        p.setPpqPosition (ppq);
        p.setIsPlaying (playing);
        return p;
    }
};

// Routes a modulation source in matrix slot k (tests)
static void setParam (ElukkaNoiseProcessor& p, const juce::String& id, float v);
static void route (ElukkaNoiseProcessor& p, int k, int src, int dest, float amount, int aux = P::SNone)
{
    setParam (p, P::mx (k, "src"), (float) src);
    setParam (p, P::mx (k, "dst"), (float) (dest + 1));
    setParam (p, P::mx (k, "amt"), amount);
    setParam (p, P::mx (k, "aux"), (float) aux);
}
// Removes every matrix row that targets dest
static void unroute (ElukkaNoiseProcessor& p, int dest)
{
    for (int k = 0; k < P::numMatrixSlots; ++k)
        if ((int) p.apvts.getRawParameterValue (P::mx (k, "dst"))->load() == dest + 1) setParam (p, P::mx (k, "src"), 0.0f);
}

static int failures = 0;
static void check (bool ok, const juce::String& what)
{
    std::cout << (ok ? "ok    " : "FAIL  ") << what << "\n";
    if (! ok) ++failures;
}

static void setParam (ElukkaNoiseProcessor& p, const juce::String& id, float v)
{
    auto* prm = p.apvts.getParameter (id);
    if (prm == nullptr) { std::cout << "TEST BUG: no parameter " << id << std::endl; std::abort(); }
    prm->setValueNotifyingHost (prm->convertTo0to1 (v));
}

// Renders `seconds` with a note held from noteOn to noteOff (seconds) and the macro following macroAt(t) (0..1),
// updated once per block (like host automation). Returns the whole render.
struct RenderOpts
{
    double sr = 48000.0;
    int block = 512;
    double seconds = 4.0, noteOn = 0.0, noteOff = 3.0;
    std::function<float (double)> macroAt;
    std::function<void (ElukkaNoiseProcessor&, double)> perBlock;
    FakePlayHead* playHead = nullptr;
};

static juce::AudioBuffer<float> render (ElukkaNoiseProcessor& proc, const RenderOpts& o)
{
    const int total = (int) (o.seconds * o.sr);
    juce::AudioBuffer<float> out (2, total);
    out.clear();
    proc.setPlayConfigDetails (0, 2, o.sr, o.block);
    FakePlayHead ph;
    FakePlayHead* usePh = o.playHead != nullptr ? o.playHead : &ph;
    proc.setPlayHead (usePh);
    proc.prepareToPlay (o.sr, o.block);
    juce::AudioBuffer<float> buf (2, o.block);
    const int onAt = (int) (o.noteOn * o.sr), offAt = (int) (o.noteOff * o.sr);
    bool onSent = false, offSent = false;
    for (int pos = 0; pos < total; pos += o.block)
    {
        const int n = juce::jmin (o.block, total - pos);
        const double t = (double) pos / o.sr;
        if (o.macroAt) setParam (proc, "macro", 100.0f * o.macroAt (t));
        if (o.perBlock) o.perBlock (proc, t);
        juce::MidiBuffer midi;
        if (! onSent && onAt < pos + n) { midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 110), juce::jmax (0, onAt - pos)); onSent = true; }
        if (! offSent && offAt < pos + n) { midi.addEvent (juce::MidiMessage::noteOff (1, 60), juce::jmax (0, offAt - pos)); offSent = true; }
        juce::AudioBuffer<float> slice (buf.getArrayOfWritePointers(), 2, 0, n);
        proc.processBlock (slice, midi);
        for (int c = 0; c < 2; ++c) out.copyFrom (c, pos, slice, c, 0, n);
        usePh->ppq += (double) n / o.sr * usePh->bpm / 60.0;
    }
    proc.setPlayHead (nullptr);
    return out;
}

static float rms (const juce::AudioBuffer<float>& b, int from, int to, int ch = -1)
{
    double s = 0.0; int count = 0;
    for (int c = (ch < 0 ? 0 : ch); c < (ch < 0 ? b.getNumChannels() : ch + 1); ++c)
        for (int i = juce::jmax (0, from); i < juce::jmin (b.getNumSamples(), to); ++i) { s += (double) b.getSample (c, i) * b.getSample (c, i); ++count; }
    return count > 0 ? (float) std::sqrt (s / count) : 0.0f;
}
static float peak (const juce::AudioBuffer<float>& b) { return b.getMagnitude (0, b.getNumSamples()); }
static bool hasNaN (const juce::AudioBuffer<float>& b)
{
    for (int c = 0; c < b.getNumChannels(); ++c)
        for (int i = 0; i < b.getNumSamples(); ++i) if (! std::isfinite (b.getSample (c, i))) return true;
    return false;
}
// spectral centroid-ish brightness: ratio of high-passed (2 kHz, one pole) RMS to total RMS over a range
static float brightness (const juce::AudioBuffer<float>& b, int from, int to, double sr)
{
    const float a = 1.0f - std::exp (-juce::MathConstants<float>::twoPi * 2000.0f / (float) sr);
    double hs = 0.0, ts = 0.0;
    float lp = 0.0f;
    const float* x = b.getReadPointer (0);
    for (int i = juce::jmax (1, from); i < juce::jmin (b.getNumSamples(), to); ++i)
    {
        lp += a * (x[i] - lp);
        const float h = x[i] - lp;
        hs += (double) h * h; ts += (double) x[i] * x[i];
    }
    return ts > 1.0e-12 ? (float) std::sqrt (hs / ts) : 0.0f;
}
static void writeWav (const juce::AudioBuffer<float>& b, double sr, const juce::File& f)
{
    f.deleteFile();
    juce::WavAudioFormat fmt;
    std::unique_ptr<juce::AudioFormatWriter> w (fmt.createWriterFor (new juce::FileOutputStream (f), sr, 2, 24, {}, 0));
    if (w != nullptr) w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
}

//==============================================================================
static int runBench()
{
    std::cout << "CPU benchmark (one core; percentage = render time / real time)\n";
    std::cout << "Machine: " << juce::SystemStats::getCpuModel() << ", " << juce::SystemStats::getNumCpus() << " cores, "
              << juce::SystemStats::getOperatingSystemName() << "\n";
    const auto& factory = PresetManager::factory();
    struct Case { const char* preset; double macro; };
    std::vector<Case> cases { { "Clean Noise Rise", 0.5 }, { "Hypnotic Pulse", 0.8 }, { "Machine Sweep", 1.0 }, { "Psy Chaos", 0.7 }, { "Metallic Motion", 0.9 }, { "Rolling Gate Rise", 0.8 }, { "Rolling Gate Rise + comp", 0.8 } };
    for (double sr : { 44100.0, 48000.0, 96000.0 })
        for (int block : { 64, 512 })
        {
            std::cout << std::fixed << std::setprecision (1) << "\n" << sr / 1000.0 << " kHz, " << block << " samples/block\n";
            for (auto& c : cases)
            {
                ElukkaNoiseProcessor proc;
                int idx = -1;
                const juce::String name = juce::String (c.preset).upToFirstOccurrenceOf (" + comp", false, false);
                for (int i = 0; i < (int) factory.size(); ++i) if (factory[(size_t) i].name == name) idx = i;
                proc.presets.load (idx);
                setParam (proc, "macro", (float) (c.macro * 100.0));
                if (juce::String (c.preset).endsWith ("+ comp")) { setParam (proc, "comp_amt", 0.7f); setParam (proc, "comp_speed", 1.0f); }
                const double seconds = 5.0;
                RenderOpts o; o.sr = sr; o.block = block; o.seconds = seconds; o.noteOn = 0.0; o.noteOff = seconds;
                render (proc, o);   // warm up
                const double t0 = juce::Time::getMillisecondCounterHiRes();
                render (proc, o);
                const double ms = juce::Time::getMillisecondCounterHiRes() - t0;
                std::cout << "  " << std::setw (26) << std::left << c.preset << std::right << std::setw (6) << std::setprecision (2) << 100.0 * ms / (seconds * 1000.0) << " %\n";
            }
            // several instances at once, sequentially on one core: the sum
            {
                std::vector<std::unique_ptr<ElukkaNoiseProcessor>> procs;
                for (int i = 0; i < 8; ++i)
                {
                    procs.push_back (std::make_unique<ElukkaNoiseProcessor>());
                    int idx = -1;
                    for (int k = 0; k < (int) factory.size(); ++k) if (factory[(size_t) k].name == cases[(size_t) (i % cases.size())].preset) idx = k;
                    procs.back()->presets.load (idx);
                    setParam (*procs.back(), "macro", 70.0f);
                }
                const double seconds = 3.0;
                double ms = 0.0;
                for (auto& p : procs)
                {
                    RenderOpts o; o.sr = sr; o.block = block; o.seconds = seconds; o.noteOn = 0.0; o.noteOff = seconds;
                    const double t0 = juce::Time::getMillisecondCounterHiRes();
                    render (*p, o);
                    ms += juce::Time::getMillisecondCounterHiRes() - t0;
                }
                std::cout << "  " << std::setw (26) << std::left << "8 instances (sum)" << std::right << std::setw (6) << std::setprecision (2) << 100.0 * ms / (seconds * 1000.0) << " %\n";
            }
        }
    return 0;
}

static int runHostLoad (const juce::String& path)
{
    juce::AudioPluginFormatManager fm;
    fm.addDefaultFormats();
    juce::OwnedArray<juce::PluginDescription> found;
    juce::KnownPluginList list;
    for (auto* format : fm.getFormats())
        list.scanAndAddFile (path, true, found, *format);
    if (found.isEmpty()) { std::cout << "Plugin not found\n"; return 1; }
    std::cout << "Found: " << found[0]->name << " (" << found[0]->pluginFormatName << ", instrument=" << (found[0]->isInstrument ? "yes" : "no") << ")\n";
    juce::String err;
    auto inst = fm.createPluginInstance (*found[0], 48000.0, 512, err);
    if (inst == nullptr) { std::cout << "Load failed: " << err << "\n"; return 1; }
    inst->setPlayConfigDetails (0, 2, 48000.0, 512);
    inst->prepareToPlay (48000.0, 512);
    std::cout << "Parameters: " << inst->getParameters().size() << "\n";
    juce::AudioProcessorParameter* macro = nullptr;
    for (auto* prm : inst->getParameters())
        if (prm->getName (64) == "MACRO") macro = prm;
    std::cout << "MACRO parameter exposed: " << (macro != nullptr ? "yes" : "NO") << "\n";
    auto renderPeak = [&] (float macroValue)
    {
        if (macro != nullptr) macro->setValueNotifyingHost (macroValue);
        juce::AudioBuffer<float> buf (2, 512);
        float pk = 0.0f;
        for (int b = 0; b < 200; ++b)
        {
            juce::MidiBuffer midi;
            if (b == 0) midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
            if (b == 150) midi.addEvent (juce::MidiMessage::noteOff (1, 60), 0);
            inst->processBlock (buf, midi);
            pk = std::max (pk, buf.getMagnitude (0, 512));
        }
        return pk;
    };
    std::cout << "Peak at macro 0%: " << renderPeak (0.0f) << ", at 100%: " << renderPeak (1.0f) << "\n";
    juce::MemoryBlock state;
    if (macro != nullptr) macro->setValueNotifyingHost (0.25f);
    inst->getStateInformation (state);
    if (macro != nullptr) macro->setValueNotifyingHost (0.9f);
    inst->setStateInformation (state.getData(), (int) state.getSize());
    std::cout << "State round-trip: " << (macro != nullptr && std::abs (macro->getValue() - 0.25f) < 0.01f ? "ok" : "FAIL") << " (" << state.getSize() << " bytes)\n";
    if (auto* ed = inst->createEditorIfNeeded())
    {
        std::cout << "Editor: " << ed->getWidth() << "x" << ed->getHeight() << "\n";
        delete ed;
    }
    inst->releaseResources();
    return 0;
}

static int runUi (const juce::File& dir, const juce::String& presetName)
{
    dir.createDirectory();
    {
        auto src = juce::ImageCache::getFromMemory (BinaryData::logo_png, BinaryData::logo_pngSize);
        int transparent = 0, opaque = 0;
        for (int y = 0; y < src.getHeight(); y += 10)
            for (int x = 0; x < src.getWidth(); x += 10)
                (src.getPixelAt (x, y).getFloatAlpha() < 0.5f ? transparent : opaque)++;
        std::cout << "logo: " << src.getWidth() << "x" << src.getHeight() << " format " << (int) src.getFormat() << " transparent " << transparent << " opaque " << opaque
                  << " corner " << src.getPixelAt (5, 5).toDisplayString (true) << " centre " << src.getPixelAt (500, 500).toDisplayString (true) << "\n";
    }
    ElukkaNoiseProcessor proc;
    proc.presets.load (proc.presets.indexOf (presetName, {}));
    setParam (proc, "macro", 63.0f);
    if (presetName == "Hypnotic Pulse")   // show the reversed effects order and a macro range
    {
        setParam (proc, "fx_order", 1.0f);
        setParam (proc, "macro_lo", 10.0f);
        setParam (proc, "macro_hi", 85.0f);
    }
    if (presetName == "Rolling Gate Rise")   // show ties in the snapshot
    {
        setParam (proc, P::gateStep (3), (float) P::GTie);
        setParam (proc, P::gateStep (6), (float) P::GOff);
        setParam (proc, P::gateStep (7), (float) P::GTie);
    }
    // run a little audio so the display shows something
    RenderOpts o; o.seconds = 0.5; o.noteOff = 0.5;
    render (proc, o);
    proc.engine.uiNoteOn = true;
    std::unique_ptr<juce::AudioProcessorEditor> ed (proc.createEditor());
    auto* editor = dynamic_cast<ElukkaNoiseEditor*> (ed.get());
    auto snap = [&] (const juce::String& name)
    {
        auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 1.0f);
        juce::File f = dir.getChildFile (name + ".png");
        f.deleteFile();
        juce::FileOutputStream os (f);
        juce::PNGImageFormat().writeImageToStream (img, os);
        std::cout << "saved " << f.getFullPathName() << " (" << img.getWidth() << "x" << img.getHeight() << ")\n";
    };
    // main screen (collapsed)
    for (auto* c : ed->getChildren())
        for (auto* gc : c->getChildren())
            if (auto* b = dynamic_cast<juce::TextButton*> (gc); b != nullptr && b->getButtonText().startsWith ("ADVANCED") && b->getToggleState())
                b->triggerClick();
    juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
    snap ("main");
    for (auto* c : ed->getChildren())
        for (auto* gc : c->getChildren())
            if (auto* b = dynamic_cast<juce::TextButton*> (gc); b != nullptr && b->getButtonText().startsWith ("ADVANCED") && ! b->getToggleState())
                b->triggerClick();
    juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
    std::vector<juce::Component*> pages;
    for (auto* c : ed->getChildren())
        for (auto* gc : c->getChildren())
            if (dynamic_cast<Page*> (gc) != nullptr) pages.push_back (gc);
    // every page in its first view, then the pages that have a second view (the buttons in the panel title strips) in it
    std::function<void (juce::Component&, const juce::String&)> press = [&] (juce::Component& root, const juce::String& text)
    {
        for (auto* c : root.getChildren())
        {
            if (auto* b = dynamic_cast<juce::TextButton*> (c); b != nullptr && b->getButtonText() == text) b->triggerClick();
            press (*c, text);
        }
    };
    for (size_t i = 0; i < pages.size(); ++i)
    {
        for (size_t j = 0; j < pages.size(); ++j) pages[j]->setVisible (i == j);
        snap ("page" + juce::String ((int) i));
        for (auto* view : { "LEVEL", "ENVELOPE", "TONE" })
        {
            press (*pages[i], view);
            juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
            if (pages[i]->getChildren().size() > 0 && dynamic_cast<Page*> (pages[i]) != nullptr)
            {
                // snapshot only if the page has such a view (a button with that text exists)
                bool has = false;
                std::function<void (juce::Component&)> find = [&] (juce::Component& r) { for (auto* c : r.getChildren()) { if (auto* b = dynamic_cast<juce::TextButton*> (c); b != nullptr && b->getButtonText() == view) has = true; find (*c); } };
                find (*pages[i]);
                if (has) snap ("page" + juce::String ((int) i) + "-" + juce::String (view).toLowerCase());
            }
        }
        for (auto* view : { "NOISE", "STEPS", "TIME" }) press (*pages[i], view);   // back to the first view
    }
    // EFFECTS: drag the reverb (bottom) up past the delay, snapshot mid-drag and after the drop
    for (size_t i = 0; i < pages.size(); ++i)
        if (auto* fx = dynamic_cast<EffectsPage*> (pages[i]))
        {
            for (size_t j = 0; j < pages.size(); ++j) pages[j]->setVisible (j == i);
            setParam (proc, "comp_amt", 0.6f);
            RenderOpts o2; o2.seconds = 1.0; o2.noteOff = 1.0;
            for (int t = 0; t < 40; ++t) { render (proc, o2); fx->tick(); if (t > 30) break; }
            std::vector<EffectsPage::Section*> secs;
            for (auto* c : fx->getChildren()) if (auto* sct = dynamic_cast<EffectsPage::Section*> (c)) secs.push_back (sct);
            snap ("effects");
            auto* reverb = secs[2];
            const int y0 = reverb->getY() + 10;
            reverb->onGrab (y0);
            reverb->onDrag (y0 - 60);
            reverb->onDrag (y0 - 130);
            juce::MessageManager::getInstance()->runDispatchLoopUntil (400);
            snap ("effects-dragging");
            reverb->onDrop();
            juce::MessageManager::getInstance()->runDispatchLoopUntil (400);
            snap ("effects-dropped");
    {
        FxMapEditor fx (proc.apvts, proc.revPresets);
        ed->addAndMakeVisible (fx);
        fx.setTopLeftPosition (70, 50);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
        snap ("fx-map-editor");
        ed->removeChildComponent (&fx);
    }
            std::cout << "order after the drop: " << P::fxOrderNames()[(int) proc.apvts.getRawParameterValue ("fx_order")->load()] << "\n";
            setParam (proc, "fx_order", 0.0f);
        }

    // the right-click window of the filter cutoff knob, on top of the matrix page
    {
        for (size_t j = 0; j < pages.size(); ++j) pages[j]->setVisible (j == 0);
        {
            KnobOptions opts (proc.apvts, "flt_cutoff");
            juce::CallOutBox box (opts, { 300, 300, 60, 40 }, ed.get());
            snap ("knob-options");
        }
        {
            ValueEntry entry (*proc.apvts.getParameter ("flt_res"));
            juce::CallOutBox box (entry, { 300, 300, 60, 40 }, ed.get());
            snap ("value-entry");
        }
    }
    juce::ignoreUnused (editor);
    return 0;
}

static int runWav (const juce::File& dir)
{
    dir.createDirectory();
    const auto& factory = PresetManager::factory();
    for (int i = 0; i < (int) factory.size(); ++i)
    {
        ElukkaNoiseProcessor proc;
        proc.seedRandom (1);
        proc.presets.load (i);
        RenderOpts o; o.seconds = 9.0; o.noteOn = 0.1; o.noteOff = 8.0;
        // rise over 4 s, hold, fall over 3 s
        o.macroAt = [] (double t) { return (float) (t < 4.0 ? t / 4.0 : t < 4.5 ? 1.0 : juce::jmax (0.0, 1.0 - (t - 4.5) / 3.0)); };
        auto b = render (proc, o);
        auto f = dir.getChildFile (juce::File::createLegalFileName (juce::String (i + 1).paddedLeft ('0', 2) + " " + factory[(size_t) i].name) + ".wav");
        writeWav (b, o.sr, f);
        std::cout << "wrote " << f.getFileName() << "\n";
    }
    return 0;
}

// --sweep [preset]: level and brightness with the macro held at 0, 25, 50, 75 and 100 % (for tuning presets)
static int runSweep (const juce::String& presetName)
{
    const auto& factory = PresetManager::factory();
    std::cout << std::setw (26) << std::left << "preset" << std::right;
    for (int m : { 0, 25, 50, 75, 100 }) std::cout << std::setw (16) << (juce::String (m) + "%  rms/bright");
    std::cout << "   peak\n";
    for (int i = 0; i < (int) factory.size(); ++i)
    {
        if (presetName.isNotEmpty() && factory[(size_t) i].name != presetName) continue;
        std::cout << std::setw (26) << std::left << factory[(size_t) i].name << std::right;
        float pk = 0.0f;
        for (int m : { 0, 25, 50, 75, 100 })
        {
            ElukkaNoiseProcessor proc;
            proc.seedRandom (1);
            proc.presets.load (i);
            setParam (proc, "macro", (float) m);
            RenderOpts o; o.seconds = 2.0; o.noteOn = 0.0; o.noteOff = 2.0;
            auto b = render (proc, o);
            pk = juce::jmax (pk, peak (b));
            std::cout << std::setw (8) << std::fixed << std::setprecision (1) << juce::Decibels::gainToDecibels (rms (b, (int) (0.5 * o.sr), (int) (2.0 * o.sr)))
                      << std::setw (8) << std::setprecision (2) << brightness (b, (int) (0.5 * o.sr), (int) (2.0 * o.sr), o.sr);
        }
        std::cout << std::setw (7) << std::setprecision (2) << pk << "\n";
    }
    return 0;
}

// --probe-delay: Machine Sweep, reverb off, delay mix constant or jumping, click report for each case
static int runProbeDelay()
{
    for (int mode = 0; mode < 4; ++mode)
    {
        ElukkaNoiseProcessor proc;
        proc.seedRandom (8);
        proc.presets.load (proc.presets.indexOf ("Machine Sweep", "FX"));
        setParam (proc, "macro", 60.0f);
        setParam (proc, "rev_on", 0.0f);
        if (mode == 1) setParam (proc, "dly_wet", 0.6f);
        if (mode == 3) setParam (proc, "gate_on", 0.0f);
        RenderOpts o; o.seconds = 2.5; o.noteOn = 0.05; o.noteOff = 1.0;
        if (mode >= 2) o.perBlock = [] (ElukkaNoiseProcessor& p, double t) { setParam (p, "dly_wet", t > 0.5 ? 0.6f : 0.0f); };
        auto b = render (proc, o);
        const auto c = detectClicks (b, 8.0f, (int) (0.05 * o.sr) + 64);
        const char* names[] { "delay 0 %", "delay 60 % constant", "delay 0 -> 60 % at 0.5 s", "same, gate off" };
        std::cout << names[mode] << ": clicks " << c.clicks << " worst " << c.worstSigmas << " first at " << (c.firstAt >= 0 ? c.firstAt / o.sr : -1.0) << " s\n";
        if (mode == 2 && c.firstAt > 0)
        {
            const int at = c.firstAt;
            float lv = 0.0f;
            for (int k = at - 2000; k < at + 2000; ++k) lv = juce::jmax (lv, std::abs (b.getSample (0, k)));
            std::cout << "  local peak " << lv << "; samples L/R:\n";
            for (int k = at - 6; k < at + 6; ++k) std::cout << "  " << k << "  " << b.getSample (0, k) << "  " << b.getSample (1, k) << "\n";
            // rms per 10 ms around it
            for (double t = at / o.sr - 0.05; t < at / o.sr + 0.05; t += 0.01) std::cout << "  rms@" << t << " " << rms (b, (int) (t * o.sr), (int) ((t + 0.01) * o.sr)) << "\n";
        }
    }
    return 0;
}

// --probe-switch <from> <to>: switch presets mid-note at several moments, report clicks and the samples around them
static int runProbeSwitch (const juce::String& from, const juce::String& to)
{
    for (double at : { 0.6, 0.65, 0.7, 0.75, 0.8, 0.85, 0.9 })
    {
        ElukkaNoiseProcessor proc;
        proc.seedRandom (4);
        proc.presets.load (proc.presets.indexOf (from, {}));
        setParam (proc, "macro", 50.0f);
        RenderOpts o; o.seconds = 1.4; o.noteOn = 0.05; o.noteOff = 1.35;
        bool done = false;
        o.perBlock = [&] (ElukkaNoiseProcessor& p, double t) { if (! done && t >= at) { p.presets.load (p.presets.indexOf (to, {})); setParam (p, "macro", 50.0f); done = true; } };
        auto b = render (proc, o);
        const auto c = detectClicks (b, 8.0f, (int) (0.05 * o.sr) + 64);
        std::cout << "switch at " << at << ": clicks " << c.clicks << " worst " << c.worstSigmas << (c.firstAt >= 0 ? " at " + juce::String (c.firstAt / o.sr, 4) : juce::String()) << "\n";
        if (c.firstAt > 0)
            for (int k = c.firstAt - 4; k < c.firstAt + 4; ++k) std::cout << "   " << k << "  " << b.getSample (0, k) << "  " << b.getSample (1, k) << "\n";
    }
    return 0;
}

// --probe-reverb [dir]: the reverb alone (Init, 100 % wet) on a 1 s white-noise note at a few settings. Level during
// the note and along the tail, the T60 the tail actually shows, stereo correlation and mono-sum loss, flutter (the tail
// envelope's autocorrelation at 3..60 ms) and spectral ripple (the tail's log spectrum against its 1/6-octave
// smoothing, 300 Hz..8 kHz) plus the band shares early and late in the tail. With a directory: writes the renders.
struct ReverbProbe
{
    float noteRms, tail1, tail2, t60, corr, monoDb, flutter, ripple;
    float early[5], late[5];   // < 150, 150-500, 500-2k, 2k-8k, > 8k Hz
    float t60Band[3];          // 150-500, 500-2k, 2k-8k Hz (two-pole band limits), from the same 1.2 .. 2.2 s slope
};
static ReverbProbe probeReverb (const juce::AudioBuffer<float>& b, double sr)
{
    ReverbProbe r {};
    const int S = (int) sr;
    r.noteRms = rms (b, S / 2, S);
    r.tail1 = rms (b, (int) (1.2 * sr), (int) (1.5 * sr));
    r.tail2 = rms (b, (int) (2.0 * sr), (int) (2.5 * sr));
    // T60 from the energy slope 1.2 .. 2.2 s (100 ms windows, least squares on dB)
    {
        double sx = 0, sy = 0, sxx = 0, sxy = 0; int n = 0;
        for (int w = 0; w < 10; ++w)
        {
            const double x = 1.25 + 0.1 * w, y = juce::Decibels::gainToDecibels (rms (b, (int) ((1.2 + 0.1 * w) * sr), (int) ((1.3 + 0.1 * w) * sr)), -120.0f);
            sx += x; sy += y; sxx += x * x; sxy += x * y; ++n;
        }
        const double slope = (n * sxy - sx * sy) / (n * sxx - sx * sx);   // dB per second (negative)
        r.t60 = slope < -0.01 ? (float) (-60.0 / slope) : 999.0f;
    }
    // the same slope per band
    {
        const float lo[3] { 150.0f, 500.0f, 2000.0f }, hi[3] { 500.0f, 2000.0f, 8000.0f };
        for (int band = 0; band < 3; ++band)
        {
            const float aL = 1.0f - std::exp (-juce::MathConstants<float>::twoPi * hi[band] / (float) sr), aH = 1.0f - std::exp (-juce::MathConstants<float>::twoPi * lo[band] / (float) sr);
            float l1 = 0, l2 = 0, h1 = 0, h2 = 0;
            std::vector<double> e (10, 0.0);
            for (int i = (int) (1.0 * sr); i < (int) (2.2 * sr); ++i)
            {
                const float x = b.getSample (0, i);
                l1 += aL * (x - l1); l2 += aL * (l1 - l2);
                h1 += aH * (l2 - h1); h2 += aH * (h1 - h2);
                const float y = l2 - h2;
                const int w = (int) ((i / sr - 1.2) * 10.0);
                if (w >= 0 && w < 10) e[(size_t) w] += (double) y * y;
            }
            double sx = 0, sy = 0, sxx = 0, sxy = 0;
            for (int w = 0; w < 10; ++w)
            {
                const double x = 1.25 + 0.1 * w, y = 10.0 * std::log10 (e[(size_t) w] + 1.0e-20);
                sx += x; sy += y; sxx += x * x; sxy += x * y;
            }
            const double slope = (10 * sxy - sx * sy) / (10 * sxx - sx * sx);
            r.t60Band[band] = slope < -0.01 ? (float) (-60.0 / slope) : 999.0f;
        }
    }
    // stereo over 1.1 .. 2.1 s
    {
        double ll = 0, rr = 0, lr = 0, mm = 0;
        for (int i = (int) (1.1 * sr); i < (int) (2.1 * sr); ++i)
        {
            const double l = b.getSample (0, i), rt = b.getSample (1, i);
            ll += l * l; rr += rt * rt; lr += l * rt; mm += 0.25 * (l + rt) * (l + rt);
        }
        r.corr = (float) (lr / std::sqrt (juce::jmax (1.0e-30, ll * rr)));
        r.monoDb = (float) (10.0 * std::log10 (juce::jmax (1.0e-30, mm) / juce::jmax (1.0e-30, 0.5 * (ll + rr))));
    }
    // flutter: envelope (|x| smoothed 1 ms) divided by its 60 ms average, autocorrelation at lags 3 .. 60 ms
    {
        std::vector<float> env;
        const float a1 = 1.0f - std::exp (-1.0f / (0.001f * (float) sr)), a60 = 1.0f - std::exp (-1.0f / (0.06f * (float) sr));
        float e1 = 0.0f, e60 = 0.0f;
        for (int i = (int) (1.0 * sr); i < (int) (2.2 * sr); ++i)
        {
            const float x = std::abs (b.getSample (0, i));
            e1 += a1 * (x - e1); e60 += a60 * (e1 - e60);
            if (i >= (int) (1.1 * sr)) env.push_back (e60 > 1.0e-9f ? e1 / e60 - 1.0f : 0.0f);
        }
        double var = 0; for (float v : env) var += v * v;
        float worst = 0.0f;
        for (int lagMs = 3; lagMs <= 60; ++lagMs)
        {
            const int lag = (int) (lagMs * 0.001 * sr);
            double acc = 0; for (size_t i = (size_t) lag; i < env.size(); ++i) acc += env[i] * env[i - (size_t) lag];
            worst = juce::jmax (worst, (float) (acc / juce::jmax (1.0e-30, var)));
        }
        r.flutter = worst;
    }
    // spectrum: 4096-point frames over 1.05 .. 1.6 s, averaged; bands; ripple against a 1/6-octave smoothing
    {
        constexpr int order = 12, N = 1 << order;
        juce::dsp::FFT fft (order);
        std::vector<float> acc ((size_t) N / 2, 0.0f), buf ((size_t) 2 * N);
        auto spectrum = [&] (double from, double to, std::vector<float>& out)
        {
            std::fill (out.begin(), out.end(), 0.0f);
            int frames = 0;
            for (int start = (int) (from * sr); start + N <= (int) (to * sr); start += N / 2, ++frames)
            {
                for (int i = 0; i < N; ++i)
                {
                    const float w = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) N);
                    buf[(size_t) i] = b.getSample (0, start + i) * w;
                }
                std::fill (buf.begin() + N, buf.end(), 0.0f);
                fft.performRealOnlyForwardTransform (buf.data(), true);
                for (int k = 0; k < N / 2; ++k) out[(size_t) k] += buf[(size_t) (2 * k)] * buf[(size_t) (2 * k)] + buf[(size_t) (2 * k + 1)] * buf[(size_t) (2 * k + 1)];
            }
            for (auto& v : out) v /= (float) juce::jmax (1, frames);
        };
        auto bands = [&] (const std::vector<float>& sp, float* out)
        {
            const float edges[6] { 0.0f, 150.0f, 500.0f, 2000.0f, 8000.0f, 1.0e9f };
            double tot = 0, e[5] {};
            for (int k = 1; k < N / 2; ++k)
            {
                const float hz = (float) k * (float) sr / (float) N;
                for (int j = 0; j < 5; ++j) if (hz >= edges[j] && hz < edges[j + 1]) e[j] += sp[(size_t) k];
                tot += sp[(size_t) k];
            }
            for (int j = 0; j < 5; ++j) out[j] = (float) (e[j] / juce::jmax (1.0e-30, tot));
        };
        spectrum (1.05, 1.6, acc);
        bands (acc, r.early);
        std::vector<float> lateSp ((size_t) N / 2);
        spectrum (1.8, 2.35, lateSp);
        bands (lateSp, r.late);
        // ripple
        std::vector<float> logMag ((size_t) N / 2);
        for (int k = 1; k < N / 2; ++k) logMag[(size_t) k] = 10.0f * std::log10 (acc[(size_t) k] + 1.0e-20f);
        double var = 0; int cnt = 0;
        for (int k = 1; k < N / 2; ++k)
        {
            const float hz = (float) k * (float) sr / (float) N;
            if (hz < 300.0f || hz > 8000.0f) continue;
            const int lo = juce::jmax (1, (int) (k / std::pow (2.0f, 1.0f / 12.0f))), hi = juce::jmin (N / 2 - 1, (int) (k * std::pow (2.0f, 1.0f / 12.0f)));
            double m = 0; for (int j = lo; j <= hi; ++j) m += logMag[(size_t) j];
            m /= (hi - lo + 1);
            const double d = logMag[(size_t) k] - m;
            var += d * d; ++cnt;
        }
        r.ripple = (float) std::sqrt (var / juce::jmax (1, cnt));
    }
    return r;
}
static int runProbeReverb (const juce::File& dir)
{
    const double sr = 48000.0;
    if (dir != juce::File()) dir.createDirectory();
    struct Setting { const char* name; std::vector<std::pair<const char*, float>> values; };
    std::vector<Setting> settings {
        { "default", {} },
        { "riser",   { { "rev_size", 1.0f }, { "rev_decay", 6.7f }, { "rev_tone", -0.75f }, { "rev_pre", 10.0f } } },
        { "bright",  { { "rev_tone", 0.0f } } },
        { "short",   { { "rev_size", 0.3f }, { "rev_decay", 0.5f }, { "rev_tone", -0.25f } } },
        { "long",    { { "rev_size", 1.0f }, { "rev_decay", 15.0f }, { "rev_tone", -0.33f } } },
    };
    std::cout << std::setw (10) << std::left << "setting" << std::right << " note  tail1.2 tail2.0  T60  T60 150-500/500-2k/2k-8k  corr  mono  flutter ripple | early <150 150-500 500-2k 2k-8k >8k | late <150 150-500 500-2k 2k-8k >8k\n";
    for (auto& st : settings)
    {
        ElukkaNoiseProcessor proc;
        proc.seedRandom (15);
        proc.presets.load (proc.presets.indexOf ("Init", "Init"));
        setParam (proc, "rev_wet", 1.0f);
        for (auto& [id, v] : st.values) setParam (proc, id, v);
        RenderOpts o; o.sr = sr; o.seconds = 3.0; o.noteOn = 0.0; o.noteOff = 1.0;
        auto b = render (proc, o);
        if (dir != juce::File()) writeWav (b, sr, dir.getChildFile (juce::String ("reverb-") + st.name + ".wav"));
        const auto r = probeReverb (b, sr);
        std::cout << std::setw (10) << std::left << st.name << std::right << std::fixed << std::setprecision (1)
                  << std::setw (6) << juce::Decibels::gainToDecibels (r.noteRms) << std::setw (8) << juce::Decibels::gainToDecibels (r.tail1)
                  << std::setw (8) << juce::Decibels::gainToDecibels (r.tail2) << std::setw (6) << r.t60
                  << std::setw (9) << r.t60Band[0] << std::setw (8) << r.t60Band[1] << std::setw (8) << r.t60Band[2] << std::setprecision (2)
                  << std::setw (7) << r.corr << std::setw (6) << r.monoDb << std::setw (8) << r.flutter << std::setw (7) << r.ripple << " |";
        for (float v : r.early) std::cout << std::setw (6) << (int) std::lround (100.0f * v);
        std::cout << "  |";
        for (float v : r.late) std::cout << std::setw (6) << (int) std::lround (100.0f * v);
        std::cout << "\n";
    }
    return 0;
}

// --probe-t60: the reverb class on its own, driven by an impulse: the T60 per octave band from the Schroeder
// backward-integrated energy decay (-5 .. -25 dB), against the Decay set. For checking the loop's loss, not a test.
static int runProbeT60()
{
    const double sr = 48000.0;
    // steady sines through the reverb (Decay 4 s, LP 4 kHz): the level each settles at shows the loop gain per frequency
    for (float hz : { 300.0f, 1000.0f, 2000.0f, 4000.0f, 7000.0f, 10000.0f })
    {
        NoiseReverb r;
        r.prepare (sr);
        NoiseReverb::Params p;
        p.mix = 1.0f; p.decay = 4.0f; p.lpHz = 4000.0f; p.hpHz = 110.0f; p.mod = 0.0f; p.preDelayMs = 0.0f;
        const int total = (int) (6.0 * sr), block = 64;
        std::vector<float> L ((size_t) total), R ((size_t) total);
        for (int i = 0; i < total; ++i) L[(size_t) i] = R[(size_t) i] = 0.1f * std::sin (juce::MathConstants<float>::twoPi * hz * (float) i / (float) sr);
        for (int pos = 0; pos < total; pos += block) r.process (L.data() + pos, R.data() + pos, block, p);
        double e = 0.0; for (int i = (int) (4.0 * sr); i < total; ++i) e += (double) L[(size_t) i] * L[(size_t) i];
        std::cout << "sine " << std::setw (6) << (int) hz << " Hz: settled wet level " << std::fixed << std::setprecision (1) << 10.0 * std::log10 (e / (2.0 * sr)) << " dB\n";
    }
    struct S { const char* name; float decay, size, lp, hp, mod; };
    const float edges[8] { 0.0f, 75.0f, 150.0f, 300.0f, 600.0f, 1200.0f, 2400.0f, 4800.0f };
    std::cout << std::setw (16) << std::left << "setting" << std::right << "   all";
    for (int b = 1; b < 8; ++b) std::cout << std::setw (6) << (int) edges[b];
    std::cout << "   (band T60 in s; bands start at the Hz shown, an octave wide)\n";
    for (const S& st : { S { "3 s mod 0", 3.0f, 0.65f, 20000.0f, 20.0f, 0.0f }, S { "3 s mod .25", 3.0f, 0.65f, 20000.0f, 20.0f, 0.25f },
                         S { "12 s mod 0", 12.0f, 1.0f, 20000.0f, 60.0f, 0.0f }, S { "12 s mod .25", 12.0f, 1.0f, 20000.0f, 60.0f, 0.25f },
                         S { "12 s mod 1", 12.0f, 1.0f, 20000.0f, 60.0f, 1.0f }, S { "15 s lp 1200", 15.0f, 0.65f, 1200.0f, 110.0f, 0.25f },
                         S { "4 s lp 4k", 4.0f, 0.65f, 4000.0f, 110.0f, 0.25f }, S { "0.5 s", 0.5f, 0.3f, 8000.0f, 110.0f, 0.25f } })
    {
        NoiseReverb r;
        r.prepare (sr);
        NoiseReverb::Params p;
        p.mix = 1.0f; p.decay = st.decay; p.size = st.size; p.lpHz = st.lp; p.hpHz = st.hp; p.mod = st.mod; p.preDelayMs = 0.0f; p.duck = 0.0f;
        const int total = (int) ((1.0 + 2.5 * st.decay) * sr), block = 64;
        std::vector<float> L ((size_t) total, 0.0f), R ((size_t) total, 0.0f);
        L[(size_t) (0.5 * sr)] = R[(size_t) (0.5 * sr)] = 1.0f;
        for (int pos = 0; pos < total; pos += block) r.process (L.data() + pos, R.data() + pos, juce::jmin (block, total - pos), p);
        std::cout << std::setw (16) << std::left << st.name << std::right << std::fixed << std::setprecision (1);
        for (int band = 0; band < 8; ++band)
        {
            std::vector<float> x (L);
            if (band > 0)
            {
                const float aL = 1.0f - std::exp (-juce::MathConstants<float>::twoPi * 2.0f * edges[band] / (float) sr), aH = 1.0f - std::exp (-juce::MathConstants<float>::twoPi * edges[band] / (float) sr);
                float l[4] {}, h[4] {};
                for (auto& v : x) { float t = v; for (auto& q : l) { q += aL * (t - q); t = q; } for (auto& q : h) { q += aH * (t - q); t -= q; } v = t; }
            }
            // Schroeder: energy remaining from each point on; the time between -5 and -25 dB (after 0.7 s: the tail is filled)
            std::vector<double> edc ((size_t) total);
            double acc = 0.0;
            for (int i = total - 1; i >= 0; --i) { acc += (double) x[(size_t) i] * x[(size_t) i]; edc[(size_t) i] = acc; }
            const int from = (int) (0.58 * sr);
            const double top = 10.0 * std::log10 (edc[(size_t) from] + 1.0e-30);
            int t5 = -1, t25 = -1;
            for (int i = from; i < total; ++i)
            {
                const double db = 10.0 * std::log10 (edc[(size_t) i] + 1.0e-30) - top;
                if (t5 < 0 && db <= -5.0) t5 = i;
                if (db <= -25.0) { t25 = i; break; }
            }
            std::cout << std::setw (6) << (t5 >= 0 && t25 > t5 ? 60.0 / 20.0 * (t25 - t5) / sr : -1.0);
        }
        std::cout << "\n";
    }
    return 0;
}

// --bench-reverb [dir]: the reverb class alone at 48 kHz (30 s of noise in 128-sample pieces): render time as a share
// of real time, and (with a directory) a reference render to compare an optimised version against.
static int runBenchReverb (const juce::File& dir)
{
    const double sr = 48000.0;
    for (int pass = 0; pass < 2; ++pass)
    {
        NoiseReverb r;
        r.prepare (sr);
        NoiseReverb::Params p;
        p.mix = 0.5f; p.decay = 6.7f; p.size = 1.0f; p.lpHz = 1200.0f; p.hpHz = 110.0f; p.duck = 0.5f;
        juce::Random rng (5);
        const int total = (int) (30.0 * sr), block = 128;
        std::vector<float> L ((size_t) total), R ((size_t) total);
        for (int i = 0; i < total; ++i) { L[(size_t) i] = rng.nextFloat() * 0.6f - 0.3f; R[(size_t) i] = rng.nextFloat() * 0.6f - 0.3f; }
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        for (int pos = 0; pos < total; pos += block) r.process (L.data() + pos, R.data() + pos, block, p);
        const double ms = juce::Time::getMillisecondCounterHiRes() - t0;
        if (pass == 1)
        {
            std::cout << "reverb alone: " << std::fixed << std::setprecision (3) << 100.0 * ms / 30000.0 << " % of real time at 48 kHz\n";
            if (dir != juce::File())
            {
                juce::AudioBuffer<float> b (2, total);
                b.copyFrom (0, 0, L.data(), total); b.copyFrom (1, 0, R.data(), total);
                auto f = dir.getChildFile ("reverb-bench.wav");
                if (f.existsAsFile())
                {
                    // compare against the reference render written by an earlier version
                    juce::WavAudioFormat fmt;
                    std::unique_ptr<juce::AudioFormatReader> rd (fmt.createReaderFor (new juce::FileInputStream (f), true));
                    juce::AudioBuffer<float> ref (2, total);
                    rd->read (&ref, 0, total, 0, true, true);
                    double diff = 0.0, sig = 0.0; float maxDiff = 0.0f;
                    for (int c = 0; c < 2; ++c)
                        for (int i = 0; i < total; ++i)
                        {
                            const float d = b.getSample (c, i) - ref.getSample (c, i);
                            diff += (double) d * d; sig += (double) ref.getSample (c, i) * ref.getSample (c, i);
                            maxDiff = juce::jmax (maxDiff, std::abs (d));
                        }
                    std::cout << "against " << f.getFileName() << ": difference " << std::setprecision (1) << 10.0 * std::log10 (diff / juce::jmax (1.0e-30, sig))
                              << " dB relative, max " << std::setprecision (6) << maxDiff << " (24-bit WAV reference: ~-110 dB is identical)\n";
                }
                else
                {
                    dir.createDirectory();
                    writeWav (b, sr, f);
                    std::cout << "wrote " << f.getFullPathName() << "\n";
                }
            }
        }
    }
    return 0;
}

// --bench-delay: the delay class alone at 48 kHz (30 s of noise in 128-sample pieces): render time as a share of real time.
static int runBenchDelay()
{
    const double sr = 48000.0;
    double best = 1.0e9;
    for (int pass = 0; pass < 3; ++pass)
    {
        StereoDelay d;
        d.prepare (sr);
        StereoDelay::Params p;
        p.wet = 0.5f; p.feedback = 0.6f; p.timeSeconds = 0.346f;
        juce::Random rng (5);
        const int total = (int) (30.0 * sr), block = 128;
        std::vector<float> L ((size_t) total), R ((size_t) total);
        for (int i = 0; i < total; ++i) { L[(size_t) i] = rng.nextFloat() * 0.6f - 0.3f; R[(size_t) i] = rng.nextFloat() * 0.6f - 0.3f; }
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        for (int pos = 0; pos < total; pos += block) d.process (L.data() + pos, R.data() + pos, block, p);
        best = juce::jmin (best, juce::Time::getMillisecondCounterHiRes() - t0);
    }
    std::cout << "delay alone: " << std::fixed << std::setprecision (3) << 100.0 * best / 30000.0 << " % of real time at 48 kHz\n";
    return 0;
}

// --probe-chain [noDelay] [noReverb]: the preset-switching chain test with effects optionally forced off
static int runProbeChain (bool noDelay, bool noReverb)
{
    const double sr = 48000.0;
    ElukkaNoiseProcessor proc;
    proc.seedRandom (4);
    proc.presets.load (0);
    RenderOpts o; o.sr = sr; o.seconds = 4.0; o.noteOn = 0.05; o.noteOff = 3.5;
    setParam (proc, "macro", 50.0f);
    int lastIdx = -1;
    const int n = (int) PresetManager::factory().size();
    o.perBlock = [&] (ElukkaNoiseProcessor& p, double t)
    {
        const int idx = juce::jlimit (0, n - 1, (int) (t / 0.2));
        if (idx != lastIdx)
        {
            p.presets.load (idx); setParam (p, "macro", 50.0f); lastIdx = idx;
            if (noDelay) setParam (p, "dly_on", 0.0f);
            if (noReverb) setParam (p, "rev_on", 0.0f);
        }
    };
    auto b = render (proc, o);
    const auto c = detectClicks (b, 8.0f, (int) (0.05 * sr) + 64);
    std::cout << "delay " << (noDelay ? "off" : "on") << ", reverb " << (noReverb ? "off" : "on") << ": clicks " << c.clicks << " worst " << c.worstSigmas
              << (c.firstAt >= 0 ? " first at " + juce::String (c.firstAt / sr, 4) + " s" : juce::String()) << "\n";
    return 0;
}

// --makeup: loudness with the compressor (makeup on) against the same render without it. Held notes at several
// amounts and speeds on several presets, plus amount sweeps (slow, and an instant jump) while a note is held.
// Same seed and Vary off, so the noise is identical with and without: differences are the compressor's alone.
static int runMakeup()
{
    const double sr = 48000.0;
    struct Mat { const char* preset; float macro; };
    const Mat mats[] { { "Clean Noise Rise", 50.0f }, { "Clean Noise Rise", 100.0f }, { "Rolling Gate Rise", 50.0f },
                       { "Random Circuit", 60.0f }, { "Psy Filter", 60.0f }, { "Acid Noise", 50.0f } };
    auto renderWith = [&] (const Mat& m, std::function<float (double)> amountAt, float speed, double seconds)
    {
        ElukkaNoiseProcessor proc;
        proc.seedRandom (21);
        proc.presets.load (proc.presets.indexOf (m.preset, {}));
        setParam (proc, "macro", m.macro);
        setParam (proc, "comp_speed", speed);
        setParam (proc, "comp_makeup", 1.0f);
        RenderOpts o; o.sr = sr; o.seconds = seconds; o.noteOn = 0.0; o.noteOff = seconds;
        o.perBlock = [amountAt] (ElukkaNoiseProcessor& p, double t) { setParam (p, "comp_amt", amountAt (t)); };
        return render (proc, o);
    };
    auto windowDb = [&] (const juce::AudioBuffer<float>& b, double from, double len) { return juce::Decibels::gainToDecibels (rms (b, (int) (from * sr), (int) ((from + len) * sr)), -120.0f); };
    std::cout << "Held (2.5..5 s), loudness with compression minus without, dB\n";
    std::cout << std::setw (30) << std::left << "material" << std::right << "  speed   amt 25%   50%    75%   100%\n";
    float worstHeld = 0.0f;
    for (auto& m : mats)
    {
        auto dry = renderWith (m, [] (double) { return 0.0f; }, 0.5f, 5.0);
        const float d = windowDb (dry, 2.5, 2.5);
        for (float speed : { 0.0f, 0.5f, 1.0f })
        {
            std::cout << std::setw (30) << std::left << (juce::String (m.preset) + " @" + juce::String ((int) m.macro) + "%") << std::right << std::fixed << std::setprecision (1) << std::setw (6) << speed;
            for (float a : { 0.25f, 0.5f, 0.75f, 1.0f })
            {
                auto wet = renderWith (m, [a] (double) { return a; }, speed, 5.0);
                const float diff = windowDb (wet, 2.5, 2.5) - d;
                worstHeld = juce::jmax (worstHeld, std::abs (diff));
                std::cout << std::setw (7) << diff;
            }
            std::cout << "\n";
        }
    }
    std::cout << "worst held mismatch " << worstHeld << " dB\n\nSweeps: 400 ms windows every 100 ms (momentary loudness), with minus without; largest mismatch and largest change between neighbouring windows\n";
    float worstSweep = 0.0f, worstJump = 0.0f;
    for (auto& m : mats)
        for (int kind = 0; kind < 3; ++kind)
        {
            auto amountAt = kind == 0 ? std::function<float (double)> ([] (double t) { return (float) juce::jlimit (0.0, 1.0, (t - 1.0) / 2.0); })
                          : kind == 1 ? std::function<float (double)> ([] (double t) { return t < 2.0 ? 0.0f : 0.8f; })
                                      : std::function<float (double)> ([] (double) { return 0.8f; });
            auto dry = renderWith (m, [] (double) { return 0.0f; }, 0.5f, 5.0);
            auto wet = renderWith (m, amountAt, 0.5f, 5.0);
            float worst = 0.0f, jump = 0.0f, prev = 0.0f;
            for (double t = 0.5; t < 4.5; t += 0.1)
            {
                const float diff = windowDb (wet, t, 0.4) - windowDb (dry, t, 0.4);   // momentary loudness: 400 ms windows
                worst = juce::jmax (worst, std::abs (diff));
                if (t > 0.55) jump = juce::jmax (jump, std::abs (diff - prev));
                prev = diff;
            }
            worstSweep = juce::jmax (worstSweep, worst);
            worstJump = juce::jmax (worstJump, jump);
            std::cout << std::setw (30) << std::left << (juce::String (m.preset) + " @" + juce::String ((int) m.macro) + "%") << std::right
                      << (kind == 0 ? "  sweep 0->100 % in 2 s" : kind == 1 ? "  jump 0->80 %        " : "  held at 80 %        ") << "  worst " << std::setw (5) << worst << " dB   jump " << std::setw (5) << jump << " dB\n";
        }
    std::cout << "worst sweep mismatch " << worstSweep << " dB, worst window-to-window jump " << worstJump << " dB\n";
    return 0;
}

// --probe: prints the samples around the worst click of the stepped-automation scenario
static int runProbe()
{
    ElukkaNoiseProcessor proc;
    proc.seedRandom (2);
    proc.presets.load (0);
    RenderOpts o; o.seconds = 3.0; o.noteOn = 0.05; o.noteOff = 2.5;
    o.macroAt = [] (double t) { return (float) juce::jlimit (0.0, 1.0, std::floor ((t - 0.5) * 10.0) / 10.0); };
    auto b = render (proc, o);
    const auto c = detectClicks (b, 8.0f, (int) (0.05 * o.sr) + 64);
    std::cout << "clicks " << c.clicks << " worst " << c.worstSigmas << " first at " << c.firstAt << "\n";
    if (c.firstAt > 10)
        for (int i = c.firstAt - 8; i < c.firstAt + 8; ++i)
            std::cout << i << ": " << b.getSample (0, i) << "  " << b.getSample (1, i) << "\n";
    return 0;
}

//==============================================================================

// Effect presets switched every 0.4 s at a given macro, delay only / reverb only / both: where the clicks are
static int runProbeFxSwitch (float macro)
{
    const double sr = 44100.0;
    for (int variant = 0; variant < 3; ++variant)
    {
        ElukkaNoiseProcessor proc;
        proc.seedRandom (21);
        proc.presets.load (proc.presets.indexOf ("Clean Noise Rise", "Risers / Falls"));
        setParam (proc, "macro", macro);
        RenderOpts o; o.sr = sr; o.seconds = 4.5; o.noteOn = 0.05; o.noteOff = 2.5;
        int last = -1;
        o.perBlock = [&] (ElukkaNoiseProcessor& p, double t)
        {
            const int step = (int) (t / 0.4);
            if (step == last) return;
            last = step;
            if (variant != 1) p.dlyPresets.load (step % (int) p.dlyPresets.entries.size());
            if (variant != 0) p.revPresets.load ((step * 2) % (int) p.revPresets.entries.size());
            setParam (p, "dly_wet", variant == 1 ? 0.0f : 0.4f); setParam (p, "rev_wet", variant == 0 ? 0.0f : 0.4f);
        };
        auto b = render (proc, o);
        const auto cl = detectClicks (b, 8.0f, (int) (0.05 * sr) + 64);
        std::cout << (variant == 0 ? "delay only " : variant == 1 ? "reverb only" : "both       ") << ": clicks " << cl.clicks << " worst " << cl.worstSigmas << " sigma"
                  << (cl.firstAt >= 0 ? " first at " + juce::String (cl.firstAt / sr, 3) + " s (step " + juce::String ((int) (cl.firstAt / sr / 0.4)) + ")" : juce::String()) << " peak " << peak (b) << "\n";
    }
    return 0;
}

// Each delay knob that differs between two delay presets alternating alone every 0.4 s on dark material (Clean Rise at
// macro 0, note off at 2.5 s), then the whole preset: which change clicks
static int runProbeFxKnobs (const juce::String& presetA, const juce::String& presetB)
{
    const double sr = 44100.0;
    const auto& list = FxPresetManager::factory (FxPresetManager::Delay);
    const FxPreset* fa = nullptr; const FxPreset* fb = nullptr;
    for (auto& f : list) { if (f.name == presetA) fa = &f; if (f.name == presetB) fb = &f; }
    if (fa == nullptr || fb == nullptr) { std::cout << "unknown preset\n"; return 1; }
    ElukkaNoiseProcessor ref;
    auto valueIn = [&] (const FxPreset& f, const juce::String& id)
    {
        for (auto& [k, v] : f.values) if (k == id) return v;
        auto* prm = ref.apvts.getParameter (id);
        return prm->convertFrom0to1 (prm->getDefaultValue());
    };
    std::vector<std::pair<juce::String, std::pair<float, float>>> ks;
    for (auto* prm : ref.getParameters())
        if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (prm))
            if (rp->paramID.startsWith ("dly_") && rp->paramID != "dly_wet")
                if (const float va = valueIn (*fa, rp->paramID), vb = valueIn (*fb, rp->paramID); std::abs (va - vb) > 1.0e-6f) ks.push_back ({ rp->paramID, { va, vb } });
    ks.push_back ({ "preset", { 0.0f, 0.0f } });
    for (auto& k : ks)
    {
        ElukkaNoiseProcessor proc;
        proc.seedRandom (21);
        proc.presets.load (proc.presets.indexOf ("Clean Noise Rise", "Risers / Falls"));
        proc.dlyPresets.loadByName (presetA);
        setParam (proc, "macro", 0.0f); setParam (proc, "dly_wet", 0.4f); setParam (proc, "rev_on", 0.0f);
        RenderOpts o; o.sr = sr; o.seconds = 4.5; o.noteOn = 0.05; o.noteOff = 2.5;
        int last = -1;
        o.perBlock = [&] (ElukkaNoiseProcessor& p, double t)
        {
            const int step = (int) (t / 0.4);
            if (step == last) return;
            last = step;
            if (k.first == "preset") { p.dlyPresets.loadByName (step % 2 == 0 ? presetA : presetB); setParam (p, "dly_wet", 0.4f); }
            else setParam (p, k.first, step % 2 == 0 ? k.second.first : k.second.second);
        };
        auto b = render (proc, o);
        const auto cl = detectClicks (b, 8.0f, (int) (0.05 * sr) + 64);
        std::cout << std::setw (14) << std::left << k.first << std::right << " " << std::setw (7) << k.second.first << " <-> " << std::setw (7) << k.second.second
                  << ": clicks " << cl.clicks << " worst " << std::fixed << std::setprecision (1) << cl.worstSigmas << " sigma"
                  << (cl.firstAt >= 0 ? " first at " + juce::String (cl.firstAt / sr, 3) + " s" : juce::String()) << "\n";
    }
    return 0;
}

int main (int argc, char** argv)
{
    std::cout << std::unitbuf;   // unbuffered: a crash in CI still shows the last check that ran
    juce::ScopedJuceInitialiser_GUI init;
    if (argc > 1 && juce::String (argv[1]) == "--sweep") return runSweep (argc > 2 ? juce::String (argv[2]) : juce::String());
    if (argc > 1 && juce::String (argv[1]) == "--probe") return runProbe();
    if (argc > 1 && juce::String (argv[1]) == "--probe-delay") return runProbeDelay();
    if (argc > 3 && juce::String (argv[1]) == "--probe-fxknobs") return runProbeFxKnobs (argv[2], argv[3]);
    if (argc > 1 && juce::String (argv[1]) == "--probe-fxswitch") return runProbeFxSwitch (argc > 2 ? (float) juce::String (argv[2]).getDoubleValue() : 0.0f);
    if (argc > 1 && juce::String (argv[1]) == "--makeup") return runMakeup();
    if (argc > 1 && juce::String (argv[1]) == "--probe-reverb") return runProbeReverb (argc > 2 ? juce::File (argv[2]) : juce::File());
    if (argc > 1 && juce::String (argv[1]) == "--probe-t60") return runProbeT60();
    if (argc > 1 && juce::String (argv[1]) == "--bench-reverb") return runBenchReverb (argc > 2 ? juce::File (argv[2]) : juce::File());
    if (argc > 1 && juce::String (argv[1]) == "--bench-delay") return runBenchDelay();
    if (argc > 1 && juce::String (argv[1]) == "--probe-chain")
    {
        for (auto [d, r] : { std::pair<bool, bool> { false, false }, { true, false }, { false, true } }) runProbeChain (d, r);
        return 0;
    }
    if (argc > 3 && juce::String (argv[1]) == "--probe-switch") return runProbeSwitch (argv[2], argv[3]);
    if (argc > 1 && juce::String (argv[1]) == "--bench") return runBench();
    if (argc > 2 && juce::String (argv[1]) == "--host") return runHostLoad (argv[2]);
    if (argc > 2 && juce::String (argv[1]) == "--ui") return runUi (juce::File (argv[2]), argc > 3 ? juce::String (argv[3]) : juce::String ("Rolling Gate Rise"));
    if (argc > 2 && juce::String (argv[1]) == "--wav") return runWav (juce::File (argv[2]));

    const double sr = 48000.0;
    const auto& factory = PresetManager::factory();
    std::cout << "ElukkaNoise offline tests: " << factory.size() << " factory presets\n\n";

    // 1. Every preset: rise 0 -> 100 % over 2 s with a note held, then fall 100 -> 0 % over 1.5 s, release, tail.
    // The instrument alone: since v0.9.20 the delay and reverb follow their own presets (tested in 'Effect presets').
    std::cout << "== Factory presets: rise, fall, release ==\n";
    for (int i = 0; i < (int) factory.size(); ++i)
    {
        ElukkaNoiseProcessor proc;
        proc.seedRandom (1);
        proc.presets.load (i);
        setParam (proc, "dly_on", 0.0f); setParam (proc, "rev_on", 0.0f);
        RenderOpts o; o.sr = sr; o.seconds = 6.0; o.noteOn = 0.05; o.noteOff = 4.0;
        o.macroAt = [] (double t) { return (float) (t < 2.0 ? t / 2.0 : t < 2.5 ? 1.0 : juce::jmax (0.0, 1.0 - (t - 2.5) / 1.5)); };
        auto b = render (proc, o);
        const auto name = factory[(size_t) i].name;
        const float pk = peak (b);
        const float lo = rms (b, (int) (0.3 * sr), (int) (0.6 * sr)), hi = rms (b, (int) (2.0 * sr), (int) (2.5 * sr));
        const float brLo = brightness (b, (int) (0.3 * sr), (int) (0.6 * sr), sr), brHi = brightness (b, (int) (2.0 * sr), (int) (2.5 * sr), sr);
        const float tail = rms (b, (int) (5.5 * sr), (int) (6.0 * sr));
        // mono compatibility: the mono sum should keep most of the energy
        juce::AudioBuffer<float> mono (1, b.getNumSamples());
        for (int s = 0; s < b.getNumSamples(); ++s) mono.setSample (0, s, 0.5f * (b.getSample (0, s) + b.getSample (1, s)));
        const float monoDb = juce::Decibels::gainToDecibels (rms (mono, (int) (1.0 * sr), (int) (2.5 * sr)) / juce::jmax (1.0e-6f, rms (b, (int) (1.0 * sr), (int) (2.5 * sr))));
        const auto clicks = detectClicks (b, 8.0f, (int) (0.05 * sr) + 64);
        const bool isInit = factory[(size_t) i].category == "Init";   // plain noise: the macro moves nothing
        const bool ok = ! hasNaN (b) && pk < 1.0f && pk > 0.02f && lo > 1.0e-4f && clicks.clicks == 0 && monoDb > -6.0f && (isInit || brHi > brLo);
        std::cout << (ok ? "ok    " : "FAIL  ") << std::setw (26) << std::left << name << std::right
                  << " peak " << std::fixed << std::setprecision (2) << pk
                  << "  rms 0%/100% " << std::setprecision (1) << juce::Decibels::gainToDecibels (lo) << "/" << juce::Decibels::gainToDecibels (hi) << " dB"
                  << "  bright " << std::setprecision (2) << brLo << ">" << brHi
                  << "  mono " << std::setprecision (1) << monoDb << " dB"
                  << "  tail " << juce::Decibels::gainToDecibels (tail, -120.0f) << " dB"
                  << "  clicks " << clicks.clicks << " (worst " << std::setprecision (1) << clicks.worstSigmas << " sigma"
                  << (clicks.firstAt >= 0 ? " at " + juce::String (clicks.firstAt / sr, 3) + " s" : "") << ")\n";
        if (! ok) ++failures;
    }

    // 2. Stepped macro automation: 10 coarse steps of 10 % each 0.1 s (as a host sending stepped automation) must not click.
    std::cout << "\n== Stepped automation ==\n";
    {
        ElukkaNoiseProcessor proc;
        proc.seedRandom (2);
        proc.presets.load (proc.presets.indexOf ("Clean Noise Rise", "Risers / Falls"));
        RenderOpts o; o.sr = sr; o.seconds = 3.0; o.noteOn = 0.05; o.noteOff = 2.5;
        o.macroAt = [] (double t) { return (float) juce::jlimit (0.0, 1.0, std::floor ((t - 0.5) * 10.0) / 10.0); };
        auto b = render (proc, o);
        const auto clicks = detectClicks (b, 8.0f, (int) (0.05 * sr) + 64);
        check (clicks.clicks == 0 && ! hasNaN (b), "Clean Rise: 10 % macro steps every 0.1 s, no clicks (worst " + juce::String (clicks.worstSigmas, 1) + " sigma)");
        // instant 0 -> 100 % jump and back
        RenderOpts o2 = o;
        o2.macroAt = [] (double t) { return (float) (t > 1.0 && t < 1.5 ? 1.0 : 0.0); };
        auto b2 = render (proc, o2);
        const auto c2 = detectClicks (b2, 8.0f, (int) (0.05 * sr) + 64);
        check (c2.clicks == 0, "Clean Rise: instant 0 -> 100 -> 0 % jumps, no clicks (worst " + juce::String (c2.worstSigmas, 1) + " sigma)");
        // the smoothed macro must still follow quickly: brightness 0.1 s after the jump must be close to the final
        const float brAfter = brightness (b2, (int) (1.1 * sr), (int) (1.2 * sr), sr), brFinal = brightness (b2, (int) (1.3 * sr), (int) (1.5 * sr), sr);
        check (brAfter > 0.8f * brFinal, "Macro smoothing is quick: brightness 100 ms after a jump is " + juce::String (100.0f * brAfter / juce::jmax (1.0e-6f, brFinal), 0) + "% of the final");
    }

    // 3. Notes: retrigger while held, overlapping notes, fast repeated notes, release tail.
    std::cout << "\n== MIDI notes ==\n";
    {
        ElukkaNoiseProcessor proc;
        proc.seedRandom (3);
        proc.presets.load (proc.presets.indexOf ("Spinning Rise", "Risers / Falls"));
        setParam (proc, "macro", 60.0f);
        setParam (proc, "rev_on", 0.0f); setParam (proc, "dly_on", 0.0f);   // this test checks the voice's own release
        proc.setPlayConfigDetails (0, 2, sr, 256);
        proc.prepareToPlay (sr, 256);
        const int total = (int) (3.0 * sr);
        juce::AudioBuffer<float> out (2, total);
        juce::AudioBuffer<float> buf (2, 256);
        struct Ev { double t; bool on; int note; };
        std::vector<Ev> evs { { 0.05, true, 60 }, { 0.5, true, 64 }, { 0.8, false, 60 }, { 1.2, true, 67 }, { 1.21, false, 67 }, { 1.22, true, 67 }, { 1.6, false, 64 }, { 2.0, false, 67 } };
        size_t next = 0;
        for (int pos = 0; pos < total; pos += 256)
        {
            juce::MidiBuffer midi;
            while (next < evs.size() && (int) (evs[next].t * sr) < pos + 256)
            {
                const int at = juce::jmax (0, (int) (evs[next].t * sr) - pos);
                midi.addEvent (evs[next].on ? juce::MidiMessage::noteOn (1, evs[next].note, (juce::uint8) 100) : juce::MidiMessage::noteOff (1, evs[next].note), at);
                ++next;
            }
            proc.processBlock (buf, midi);
            for (int c = 0; c < 2; ++c) out.copyFrom (c, pos, buf, c, 0, juce::jmin (256, total - pos));
        }
        const auto clicks = detectClicks (out, 8.0f, (int) (0.05 * sr) + 64);
        check (clicks.clicks == 0 && ! hasNaN (out), "Overlapping and retriggered notes, no clicks (worst " + juce::String (clicks.worstSigmas, 1) + " sigma)");
        check (rms (out, (int) (0.9 * sr), (int) (1.1 * sr)) > 1.0e-3f, "Sound continues while a second note is still held after the first is released");
        check (rms (out, (int) (2.8 * sr), (int) (3.0 * sr)) < 2.0e-3f, "Silence 0.8 s after the last note off (release tail finished)");
        check (rms (out, (int) (2.0 * sr), (int) (2.02 * sr)) > 1.0e-3f, "Sound still present right at note off (release starts from the sounding level)");
    }

    // 4. Preset switching during a held note must not click.
    std::cout << "\n== Preset switching mid-note ==\n";
    {
        ElukkaNoiseProcessor proc;
        proc.seedRandom (4);
        proc.presets.load (0);
        RenderOpts o; o.sr = sr; o.seconds = 4.0; o.noteOn = 0.05; o.noteOff = 3.5;
        setParam (proc, "macro", 50.0f);
        int lastIdx = -1;
        o.perBlock = [&] (ElukkaNoiseProcessor& p, double t)
        {
            const int idx = juce::jlimit (0, (int) factory.size() - 1, (int) (t / 0.2));
            if (idx != lastIdx) { p.presets.load (idx); setParam (p, "macro", 50.0f); lastIdx = idx; }
        };
        auto b = render (proc, o);
        const auto clicks = detectClicks (b, 8.0f, (int) (0.05 * sr) + 64);
        if (clicks.clicks > 0) std::cout << "      first click at " << clicks.firstAt / sr << " s\n";
        check (clicks.clicks == 0 && ! hasNaN (b), "Every factory preset switched in every 0.2 s while a note is held, no clicks (worst " + juce::String (clicks.worstSigmas, 1) + " sigma)");
    }

    // 5. Buffer size and sample rate consistency: the level envelope of a macro sweep must match.
    std::cout << "\n== Buffer size / sample rate consistency ==\n";
    {
        auto envelope = [&] (double rate, int block)
        {
            ElukkaNoiseProcessor proc;
            proc.seedRandom (5);
            proc.presets.load (proc.presets.indexOf ("Clean Noise Rise", "Risers / Falls"));
            RenderOpts o; o.sr = rate; o.block = block; o.seconds = 3.0; o.noteOn = 0.05; o.noteOff = 2.8;
            o.macroAt = [] (double t) { return (float) juce::jlimit (0.0, 1.0, (t - 0.3) / 2.0); };
            auto b = render (proc, o);
            std::vector<float> env;
            for (double t = 0.5; t < 2.5; t += 0.25) env.push_back (juce::Decibels::gainToDecibels (rms (b, (int) (t * rate), (int) ((t + 0.25) * rate))));
            return env;
        };
        const auto ref = envelope (48000.0, 512);
        for (auto [rate, block] : { std::pair<double, int> { 48000.0, 64 }, { 48000.0, 2048 }, { 44100.0, 512 }, { 96000.0, 512 }, { 96000.0, 32 } })
        {
            const auto e = envelope (rate, block);
            float worst = 0.0f;
            juce::String detail;
            for (size_t i = 0; i < e.size(); ++i) { worst = juce::jmax (worst, std::abs (e[i] - ref[i])); detail += juce::String (e[i] - ref[i], 1) + " "; }
            if (worst >= 1.2f) std::cout << "      per 0.25 s step (dB): " << detail << "\n";
            check (worst < 1.2f, "Level envelope at " + juce::String (rate / 1000.0, 1) + " kHz / " + juce::String (block) + " samples within 1.2 dB of 48 kHz / 512 (worst " + juce::String (worst, 2) + " dB)");
        }
    }

    // 6. Extreme settings stay bounded: full resonance, full drive, full delay feedback, long reverb.
    std::cout << "\n== Stability ==\n";
    {
        ElukkaNoiseProcessor proc;
        proc.seedRandom (6);
        proc.presets.load (0);
        setParam (proc, "flt_res", 1.0f); setParam (proc, "flt_drive", 1.0f); setParam (proc, "out_level", 24.0f);
        setParam (proc, "dly_on", 1.0f); setParam (proc, "dly_fb", 1.0f); setParam (proc, "dly_wet", 1.0f);
        setParam (proc, "rev_on", 1.0f); setParam (proc, "rev_decay", 20.0f); setParam (proc, "rev_wet", 1.0f); setParam (proc, "rev_size", 1.0f);
        setParam (proc, "l1_rate", 50.0f); setParam (proc, "sh_rate", 200.0f);
        route (proc, 10, P::SLfo1, P::DCutoff, 1.0f);
        route (proc, 11, P::SSH, P::DPan, 1.0f);
        RenderOpts o; o.sr = sr; o.seconds = 8.0; o.noteOn = 0.05; o.noteOff = 4.0;
        o.macroAt = [] (double t) { return (float) (0.5 + 0.5 * std::sin (t * 3.0)); };
        auto b = render (proc, o);
        check (! hasNaN (b) && peak (b) <= 1.0f, "Full resonance, drive, +24 dB, delay feedback 100 %, 20 s reverb, 50 Hz LFO, 200 Hz S&H: finite, peak " + juce::String (peak (b), 3));
        const float t1 = rms (b, (int) (5.0 * sr), (int) (5.5 * sr)), t2 = rms (b, (int) (7.5 * sr), (int) (8.0 * sr));
        check (t2 <= t1 * 1.05f, "Tail decays after note off with feedback at 100 % (" + juce::String (juce::Decibels::gainToDecibels (t1), 1) + " -> " + juce::String (juce::Decibels::gainToDecibels (t2), 1) + " dB)");
    }

    // 7. Gate and S&H timing: a synced 1/16 gate at 130 bpm has steps of 115 ms.
    std::cout << "\n== Gate ==\n";
    {
        ElukkaNoiseProcessor proc;
        proc.seedRandom (7);
        proc.presets.load (0);
        setParam (proc, "macro", 80.0f);
        setParam (proc, "dly_on", 0.0f); setParam (proc, "rev_on", 0.0f);   // the gate alone (the effects' presets would fill the gaps)
        setParam (proc, "gate_on", 1.0f); setParam (proc, "gate_div", 5.0f); setParam (proc, "gate_depth", 1.0f); setParam (proc, "gate_duty", 0.5f);
        setParam (proc, "gate_att", 1.0f); setParam (proc, "gate_rel", 5.0f);
        RenderOpts o; o.sr = sr; o.seconds = 2.0; o.noteOn = 0.0; o.noteOff = 2.0;
        auto b = render (proc, o);
        // count envelope dips
        int dips = 0; bool low = false;
        const double step = 60.0 / 130.0 * 0.25;
        for (double t = 0.0; t < 2.0 - 0.005; t += 0.005)
        {
            const float r = rms (b, (int) (t * sr), (int) ((t + 0.005) * sr));
            const bool isLow = r < 1.0e-3f;
            if (isLow && ! low) ++dips;
            low = isLow;
        }
        const int expected = (int) (2.0 / step);
        check (std::abs (dips - expected) <= 1, "1/16 gate at 130 bpm: " + juce::String (dips) + " closed steps in 2 s (expected " + juce::String (expected) + ")");
        const auto clicks = detectClicks (b, 8.0f, 64);
        check (clicks.clicks == 0, "Gate with 1 ms attack / 5 ms release, no clicks (worst " + juce::String (clicks.worstSigmas, 1) + " sigma)");
    }

    // 7b. Tempo sync: a 1/4 pan LFO at 130 bpm swings every 461.5 ms; a 1/8 delay repeats after 230.8 ms.
    std::cout << "\n== Tempo sync ==\n";
    {
        ElukkaNoiseProcessor proc;
        proc.seedRandom (71);
        proc.presets.load (0);
        setParam (proc, "macro", 100.0f); setParam (proc, "noise_stereo", 0.0f);   // mono noise, so the pan alone moves the image
        setParam (proc, "dly_on", 0.0f); setParam (proc, "rev_on", 0.0f);
        setParam (proc, "l1_wave", (float) P::WSine); setParam (proc, "l1_sync", 1.0f); setParam (proc, "l1_div", 10.0f);   // 1/4
        route (proc, 10, P::SLfo1, P::DPan, 0.5f);
        RenderOpts o; o.sr = sr; o.seconds = 4.0; o.noteOn = 0.0; o.noteOff = 4.0;
        auto b = render (proc, o);
        // left-channel level over 5 ms windows: find the period between dips
        std::vector<double> dips;
        float prev2 = 1.0f, prev1 = 1.0f;
        for (double t = 0.2; t < 4.0 - 0.005; t += 0.005)
        {
            const float v = rms (b, (int) (t * sr), (int) ((t + 0.005) * sr), 0);
            if (prev1 < prev2 && prev1 < v && prev1 < 0.02f) dips.push_back (t - 0.005);
            prev2 = prev1; prev1 = v;
        }
        double period = 0.0;
        if (dips.size() >= 3) period = (dips.back() - dips.front()) / (double) (dips.size() - 1);
        check (std::abs (period - 60.0 / 130.0) < 0.012, "Synced 1/4 LFO at 130 bpm: pan period " + juce::String (period * 1000.0, 1) + " ms (expected 461.5)");
    }
    {
        ElukkaNoiseProcessor proc;
        proc.seedRandom (72);
        proc.presets.load (0);
        setParam (proc, "macro", 100.0f); setParam (proc, "env_attack", 0.5f); setParam (proc, "env_release", 5.0f);
        setParam (proc, "dly_on", 1.0f); setParam (proc, "dly_sync", 1.0f); setParam (proc, "dly_division", 6.0f);   // 1/8
        setParam (proc, "dly_fb", 0.0f); setParam (proc, "dly_wet", 0.5f); setParam (proc, "dly_mode", (float) P::DmStereo);
        RenderOpts o; o.sr = sr; o.seconds = 1.0; o.noteOn = 0.1; o.noteOff = 0.13;   // a 30 ms burst and its echo
        auto b = render (proc, o);
        double burstAt = -1.0, echoAt = -1.0;
        for (double t = 0.0; t < 1.0 - 0.002; t += 0.002)
        {
            const float v = rms (b, (int) (t * sr), (int) ((t + 0.002) * sr), 0);
            if (v > 0.01f && burstAt < 0.0) burstAt = t;
            else if (v > 0.01f && burstAt >= 0.0 && t > burstAt + 0.1 && echoAt < 0.0) echoAt = t;
        }
        const double gap = echoAt - burstAt;
        check (std::abs (gap - 60.0 / 130.0 * 0.5) < 0.006, "Synced 1/8 delay at 130 bpm: echo after " + juce::String (gap * 1000.0, 1) + " ms (expected 230.8)");
    }

    // 7c. The stretched risers: the cutoff keeps moving right up to 100 % and tops out where the old wall was.
    std::cout << "\n== No wall before 100 % ==\n";
    for (auto [name, amount] : { std::pair<const char*, float> { "Clean Noise Rise", 0.597f }, { "Spinning Rise", 0.6f }, { "Wide Rotation", 0.58f },
                                 { "Industrial Pressure", 0.6f }, { "Blackout Fall", 0.58f } })
    {
        float topHz = 0.0f, knobHz = 0.0f;   // where the macro's push (the preset's amount) lands the knob at 100 %
        {
            ElukkaNoiseProcessor ref;
            ref.presets.load (ref.presets.indexOf (name, "Risers / Falls"));
            const auto rg = ref.apvts.getParameter ("flt_cutoff")->getNormalisableRange();
            knobHz = ref.apvts.getRawParameterValue ("flt_cutoff")->load();
            topHz = rg.convertFrom0to1 (juce::jlimit (0.0f, 1.0f, rg.convertTo0to1 (knobHz) + amount));
        }
        auto cutoffAt = [&, nm = juce::String (name)] (float m)
        {
            ElukkaNoiseProcessor proc;
            proc.presets.load (proc.presets.indexOf (nm, "Risers / Falls"));
            setParam (proc, "macro", m);
            RenderOpts o; o.seconds = 0.3; o.noteOff = 0.3;
            render (proc, o);
            return proc.apvts.getParameter ("flt_cutoff")->convertFrom0to1 (proc.engine.uiCutoff.load());
        };
        const float c90 = cutoffAt (90.0f), c95 = cutoffAt (95.0f), c100 = cutoffAt (100.0f);
        check (c90 < c95 && c95 < c100 && std::abs (c100 - topHz) < topHz * 0.02f,
               juce::String (name) + ": cutoff still rising at the top (90/95/100 % = " + juce::String (c90, 0) + " / " + juce::String (c95, 0) + " / " + juce::String (c100, 0) + " Hz; knob " + juce::String (knobHz, 0) + " Hz + push = " + juce::String (topHz, 0) + " Hz)");
    }

    // 7d. DELAY and REVERB knobs (the delay / reverb mix): both effects are on in every preset, each knob adds an
    // audible tail on its own, and turning either up mid-note never clicks. Riser / fall presets share one space setup.
    std::cout << "\n== Delay and reverb knobs ==\n";
    {
        int bothOn = 0, fxKept = 0, varyOff = 0;
        int louder[2] {}, clickFree[2] {};
        const char* ids[2] { "dly_wet", "rev_wet" };
        for (int i = 0; i < (int) factory.size(); ++i)
        {
            {
                ElukkaNoiseProcessor proc;
                proc.dlyPresets.loadByName ("Modulated Echo"); proc.revPresets.loadByName ("Modulated Space");   // v0.9.21: a preset loads the effect presets it names
                proc.presets.load (i);
                auto v = [&] (const char* id) { return proc.apvts.getRawParameterValue (id)->load(); };
                if (v ("dly_on") > 0.5f && v ("rev_on") > 0.5f) ++bothOn;
                if (v ("noise_vary") < 0.5f) ++varyOff;
                if (proc.dlyPresets.currentName == factory[(size_t) i].delayPreset && proc.revPresets.currentName == factory[(size_t) i].reverbPreset) ++fxKept;
                else std::cout << "      " << factory[(size_t) i].name << ": loading it did not select its effect presets\n";
            }
            for (int e = 0; e < 2; ++e)
            {
                auto tailWith = [&] (float mix, ClickReport* clicks)
                {
                    ElukkaNoiseProcessor proc;
                    proc.seedRandom (8);
                    proc.presets.load (i);
                    setParam (proc, "macro", 60.0f);
                    // measure the echoes alone: reverb off (the delay mix also feeds the reverb less dry) and a short note
                    // release (Hypnotic Pulse's 2 s release would otherwise fill the window)
                    if (e == 0) { setParam (proc, "rev_on", 0.0f); setParam (proc, "env_release", 60.0f); }
                    else setParam (proc, "dly_on", 0.0f);   // and the reverb without a long delay tail filling the window
                    const float base = proc.apvts.getRawParameterValue (ids[e])->load();
                    RenderOpts o; o.sr = sr; o.seconds = 2.5; o.noteOn = 0.05; o.noteOff = 1.0;
                    if (clicks != nullptr) o.perBlock = [&, base] (ElukkaNoiseProcessor& p, double t) { setParam (p, ids[e], t > 0.5 ? mix : base); };
                    else if (mix > 0.0f) setParam (proc, ids[e], mix);
                    auto b = render (proc, o);
                    if (clicks != nullptr) *clicks = detectClicks (b, 8.0f, (int) (0.05 * sr) + 64);
                    return rms (b, (int) (1.3 * sr), (int) (1.8 * sr));
                };
                const float before = tailWith (0.0f, nullptr), after = tailWith (0.6f, nullptr);
                ClickReport cr;
                tailWith (0.6f, &cr);
                if (after > before * 1.41f + 1.0e-4f) ++louder[e];
                else std::cout << "      " << factory[(size_t) i].name << ": " << ids[e] << " at 60 % gives tail " << juce::Decibels::gainToDecibels (before) << " -> " << juce::Decibels::gainToDecibels (after) << " dB\n";
                if (cr.clicks == 0) ++clickFree[e];
                else std::cout << "      " << factory[(size_t) i].name << ": " << ids[e] << " jump clicked (worst " << cr.worstSigmas << " sigma)\n";
            }
        }
        const int n = (int) factory.size();
        check (bothOn == n, "Delay and reverb are on in every factory preset (" + juce::String (bothOn) + " / " + juce::String (n) + ")");
        check (varyOff == n, "Noise Vary is off in every factory preset (" + juce::String (varyOff) + " / " + juce::String (n) + ")");
        check (fxKept == n, "Every factory preset loads the delay and reverb presets it names (" + juce::String (fxKept) + " / " + juce::String (n) + ")");
        check (louder[0] == n, "DELAY knob at 60 % adds at least 3 dB of echoes after note off in every preset, reverb off (" + juce::String (louder[0]) + " / " + juce::String (n) + ")");
        check (louder[1] == n, "REVERB knob at 60 % adds at least 3 dB of tail in every preset (" + juce::String (louder[1]) + " / " + juce::String (n) + ")");
        check (clickFree[0] == n && clickFree[1] == n, "DELAY and REVERB knobs jumping up mid-note never click (" + juce::String (clickFree[0]) + " + " + juce::String (clickFree[1]) + " / " + juce::String (2 * n) + ")");
    }

    // 7e. Gate sequencer: ties, swing, sustain, synced speed doubling.
    std::cout << "\n== Gate sequencer ==\n";
    {
        // open / close events of a gated render (5 ms windows, threshold halfway between floor and top)
        auto openings = [&] (const juce::AudioBuffer<float>& b, double from, double to, std::vector<double>* times = nullptr)
        {
            // 2 ms windows; open above 60 % of the loudest window, closed again below 30 % (hysteresis)
            std::vector<float> lv;
            for (double t = from; t < to - 0.002; t += 0.002) lv.push_back (rms (b, (int) (t * sr), (int) ((t + 0.002) * sr)));
            const float top = *std::max_element (lv.begin(), lv.end());
            int count = 0; bool high = false;
            for (size_t i = 0; i < lv.size(); ++i)
            {
                if (! high && lv[i] > 0.6f * top) { high = true; ++count; if (times) times->push_back (from + 0.002 * (double) i); }
                else if (high && lv[i] < 0.3f * top) high = false;
            }
            return count;
        };
        auto gated = [&] (std::function<void (ElukkaNoiseProcessor&)> setup, double seconds = 2.0)
        {
            ElukkaNoiseProcessor proc;
            proc.seedRandom (9);
            proc.presets.load (0);
            setParam (proc, "macro", 100.0f);
            setParam (proc, "dly_on", 0.0f); setParam (proc, "rev_on", 0.0f);
            setParam (proc, "gate_on", 1.0f); setParam (proc, "gate_depth", 1.0f); setParam (proc, "gate_div", 5.0f);   // 1/16
            setParam (proc, "gate_att", 0.5f); setParam (proc, "gate_rel", 3.0f); setParam (proc, "gate_duty", 0.5f);
            setup (proc);
            RenderOpts o; o.sr = sr; o.seconds = seconds; o.noteOn = 0.0; o.noteOff = seconds;
            return render (proc, o);
        };
        const double step = 60.0 / 130.0 * 0.25;
        {
            // steps: On Tie Off Off repeated -> one opening per 4 steps, lasting about 1.5 steps
            auto b = gated ([] (ElukkaNoiseProcessor& p)
            {
                for (int i = 0; i < P::numGateSteps; ++i) setParam (p, P::gateStep (i), (float) (i % 4 == 0 ? P::GOn : i % 4 == 1 ? P::GTie : P::GOff));
            });
            const int n = openings (b, 0.0, 2.0);
            const int expected = (int) std::ceil (2.0 / (4.0 * step));
            check (std::abs (n - expected) <= 1, "Ties: On-Tie-Off-Off opens once per 4 steps (" + juce::String (n) + " openings, expected " + juce::String (expected) + ")");
            // the tied step is still open at 1.4 steps after its start
            const float during = rms (b, (int) ((4.0 * step + 1.4 * step) * sr), (int) ((4.0 * step + 1.45 * step) * sr));
            check (during > 0.05f, "Ties: the gate stays open through the tied step (level " + juce::String (during, 3) + ")");
            const auto c = detectClicks (b, 8.0f, 64);
            check (c.clicks == 0, "Ties with 0.5 ms attack: no clicks (worst " + juce::String (c.worstSigmas, 1) + " sigma)");
        }
        {
            // note retrigger: a note 0.1 s into the song (off the 1/16 grid). On: steps count from the note. Off: they
            // stay on the song grid.
            auto firstSteps = [&] (bool retrig)
            {
                ElukkaNoiseProcessor proc;
                proc.seedRandom (9);
                proc.presets.load (proc.presets.indexOf ("Clean Noise Rise", "Risers / Falls"));
                setParam (proc, "macro", 100.0f);
                setParam (proc, "gate_on", 1.0f); setParam (proc, "gate_depth", 1.0f); setParam (proc, "gate_div", 5.0f);
                setParam (proc, "gate_att", 0.5f); setParam (proc, "gate_rel", 3.0f); setParam (proc, "gate_duty", 0.5f);
                setParam (proc, "gate_retrig", retrig ? 1.0f : 0.0f);
                setParam (proc, "env_attack", 0.5f);
                RenderOpts o; o.sr = sr; o.block = 64; o.seconds = 1.2; o.noteOn = 0.1; o.noteOff = 1.2;
                auto b = render (proc, o);
                std::vector<double> t;
                openings (b, 0.09, 1.2, &t);
                return t;
            };
            const auto on = firstSteps (true), off = firstSteps (false);
            double worstOn = 0.0, worstOff = 0.0;
            for (size_t k = 0; k < juce::jmin ((size_t) 6, on.size()); ++k)
                worstOn = juce::jmax (worstOn, std::abs (on[k] - (0.1 + (double) k * step)));
            for (size_t k = 0; k < juce::jmin ((size_t) 6, off.size()); ++k)
            {
                const double grid = std::ceil ((off[k] - 0.004) / step) * step;   // nearest song-grid step at or after it
                worstOff = juce::jmax (worstOff, std::abs (off[k] - grid));
            }
            check (on.size() >= 6 && worstOn < 0.004, "Gate Retrig on: steps start on the note, 0.1 s into the song (worst " + juce::String (worstOn * 1000.0, 1) + " ms)");
            check (off.size() >= 6 && worstOff < 0.004 && std::abs (off[0] - 0.1) > 0.008, "Gate Retrig off: steps stay on the song's 1/16 grid (worst " + juce::String (worstOff * 1000.0, 1) + " ms)");
        }
        {
            // swing 66 %: the second step of each pair starts 2/3 into the pair
            std::vector<double> t;
            auto b = gated ([] (ElukkaNoiseProcessor& p) { setParam (p, "gate_swing", 66.0f); });
            openings (b, 0.0, 2.0, &t);
            double worst = 0.0;
            for (size_t i = 0; i + 1 < t.size(); ++i)
            {
                const double gap = t[i + 1] - t[i], expect = (i % 2 == 0 ? 0.66 : 0.34) * 2.0 * step;
                worst = juce::jmax (worst, std::abs (gap - expect));
            }
            check (t.size() > 8 && worst < 0.005, "Swing 66 %: steps alternate long / short (worst timing error " + juce::String (worst * 1000.0, 1) + " ms)");
        }
        {
            // sustain 30 %, decay 10 ms, long steps: the late part of a step sits at 30 % of the peak
            auto b = gated ([] (ElukkaNoiseProcessor& p)
            {
                setParam (p, "gate_div", 1.0f); setParam (p, "gate_duty", 0.9f);   // 1/4 steps, open 90 %
                setParam (p, "gate_decay", 10.0f); setParam (p, "gate_sus", 0.3f); setParam (p, "gate_hold", 20.0f);
            });
            const double q = 60.0 / 130.0;
            const float peak = rms (b, (int) ((q + 0.005) * sr), (int) ((q + 0.02) * sr)), late = rms (b, (int) ((q + 0.3) * sr), (int) ((q + 0.38) * sr));
            check (std::abs (late / peak - 0.3f) < 0.06f, "Hold 20 ms, decay to 30 % sustain: late level is " + juce::String (100.0f * late / peak, 0) + "% of the start");
            const auto c = detectClicks (b, 8.0f, 64);
            check (c.clicks == 0, "AHDSR gate: no clicks (worst " + juce::String (c.worstSigmas, 1) + " sigma)");
        }
        {
            // the gate switched on and off every 0.13 s (mid-step, often while closed): no clicks
            auto b = gated ([] (ElukkaNoiseProcessor&) {});
            ElukkaNoiseProcessor proc;
            proc.seedRandom (9);
            proc.presets.load (proc.presets.indexOf ("Clean Noise Rise", "Risers / Falls"));
            setParam (proc, "macro", 100.0f);
            setParam (proc, "gate_depth", 1.0f); setParam (proc, "gate_div", 5.0f); setParam (proc, "gate_att", 0.5f); setParam (proc, "gate_rel", 3.0f);
            RenderOpts o; o.sr = sr; o.seconds = 2.0; o.noteOn = 0.0; o.noteOff = 2.0;
            o.perBlock = [] (ElukkaNoiseProcessor& p, double t) { setParam (p, "gate_on", std::fmod (t, 0.26) < 0.13 ? 1.0f : 0.0f); };
            auto c2 = render (proc, o);
            juce::ignoreUnused (b);
            const auto c = detectClicks (c2, 8.0f, 64);
            check (c.clicks == 0, "Gate switched on and off mid-step: no clicks (worst " + juce::String (c.worstSigmas, 1) + " sigma)");
        }
        {
            // random steps and 16 short steps at 1/32 with 0.5 ms attack: still click-free
            auto b = gated ([] (ElukkaNoiseProcessor& p) { setParam (p, "gate_random", 1.0f); setParam (p, "gate_div", 7.0f); setParam (p, "gate_len", 7.0f); });
            const auto c = detectClicks (b, 8.0f, 64);
            check (c.clicks == 0, "Random steps, 1/32, 7-step pattern: no clicks (worst " + juce::String (c.worstSigmas, 1) + " sigma)");
        }
        {
            // the gate retriggers the noise: steps 3 and 6 of a note hold the same noise (correlation ~1, best lag within
            // one control block); a window from the middle of a step does not (sanity check)
            auto stepsOf = [&] (bool vary, int note2Offset)
            {
                ElukkaNoiseProcessor proc;
                proc.seedRandom (12);
                proc.presets.load (0);
                setParam (proc, "macro", 100.0f);
                setParam (proc, "dly_on", 0.0f); setParam (proc, "rev_on", 0.0f);
                setParam (proc, "noise_vary", vary ? 1.0f : 0.0f);
                setParam (proc, "gate_on", 1.0f); setParam (proc, "gate_depth", 1.0f); setParam (proc, "gate_div", 5.0f);
                setParam (proc, "gate_att", 0.5f); setParam (proc, "gate_rel", 3.0f); setParam (proc, "gate_duty", 0.5f);
                RenderOpts o; o.sr = sr; o.seconds = 2.0; o.noteOn = 0.0; o.noteOff = 2.0;
                juce::ignoreUnused (note2Offset);
                return render (proc, o);
            };
            auto corr = [&] (const juce::AudioBuffer<float>& a, int s1, const juce::AudioBuffer<float>& b, int s2, int len)
            {
                double best = 0.0;
                for (int lag = -20; lag <= 20; ++lag)
                {
                    double xy = 0.0, xx = 0.0, yy = 0.0;
                    for (int i = 0; i < len; ++i)
                    {
                        const double x = a.getSample (0, s1 + i), y = b.getSample (0, s2 + lag + i);
                        xy += x * y; xx += x * x; yy += y * y;
                    }
                    best = juce::jmax (best, xy / std::sqrt (juce::jmax (1.0e-20, xx * yy)));
                }
                return best;
            };
            const int stepS = (int) (step * sr), off = (int) (0.006 * sr), len = (int) (0.04 * sr);
            auto b = stepsOf (false, 0);
            const double same = corr (b, 3 * stepS + off, b, 6 * stepS + off, len);
            const double other = corr (b, 3 * stepS + off, b, 6 * stepS + off + 600, len);
            check (same > 0.95 && other < 0.4, "Vary off: gate steps retrigger the same noise, steps 3 and 6 match (correlation " + juce::String (same, 3)
                                               + "; shifted window " + juce::String (other, 3) + ")");
            // Vary on: every step plays fresh, random noise
            auto v = stepsOf (true, 0);
            const double fresh = corr (v, 3 * stepS + off, v, 6 * stepS + off, len);
            check (fresh < 0.4, "Noise Vary on: gate steps play random noise (steps 3 and 6 correlation " + juce::String (fresh, 3) + ")");
        }
        {
            // Rolling Gate Rise: the gate speed doubles from 1/8 to 1/16 to 1/32 as the macro rises
            auto countAt = [&] (float m)
            {
                ElukkaNoiseProcessor proc;
                proc.seedRandom (10);
                proc.presets.load (proc.presets.indexOf ("Rolling Gate Rise", "Risers / Falls"));
                setParam (proc, "macro", m);
                setParam (proc, "gate_depth", 1.0f);   // full depth so every closed step is measurable
                unroute (proc, P::DGateDepth);   // full depth
                unroute (proc, P::DCutoff);      // filter open, so only the gate shapes the level
                setParam (proc, "flt_cutoff", 6000.0f);
                RenderOpts o; o.sr = sr; o.seconds = 2.2; o.noteOn = 0.0; o.noteOff = 2.2;
                auto b = render (proc, o);
                return openings (b, 0.2, 2.2);
            };
            const int a = countAt (10.0f), b2 = countAt (50.0f), c = countAt (90.0f);
            check (std::abs (b2 - 2 * a) <= 2 && std::abs (c - 2 * b2) <= 3, "Rolling Gate Rise doubles its gate speed: " + juce::String (a) + " / " + juce::String (b2) + " / " + juce::String (c) + " openings in 2 s at 10 / 50 / 90 %");
        }
    }

    // 7f. Modulation matrix: aux source, new destinations, defaults, the S&H band-pass presets.
    std::cout << "\n== Modulation matrix ==\n";
    {
        // LFO 1 -> cutoff with the macro as aux: at macro 0 the cutoff stands still, at 100 % it swings
        auto swing = [&] (float macro)
        {
            ElukkaNoiseProcessor proc;
            proc.presets.load (0);
            unroute (proc, P::DCutoff);
            setParam (proc, "flt_cutoff", 1000.0f);
            setParam (proc, "macro", macro);
            setParam (proc, "l1_sync", 0.0f); setParam (proc, "l1_rate", 4.0f);
            route (proc, 10, P::SLfo1, P::DCutoff, 0.3f, P::SMacro);
            proc.setPlayConfigDetails (0, 2, sr, 512);
            proc.prepareToPlay (sr, 512);
            juce::AudioBuffer<float> buf (2, 512);
            float lo = 1.0f, hi = 0.0f;
            for (int i = 0; i < 200; ++i)
            {
                juce::MidiBuffer midi;
                if (i == 0) midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
                proc.processBlock (buf, midi);
                if (i > 20) { const float v = proc.engine.uiDest[P::DCutoff].load(); lo = juce::jmin (lo, v); hi = juce::jmax (hi, v); }
            }
            return hi - lo;
        };
        const float at0 = swing (0.0f), at50 = swing (50.0f), at100 = swing (100.0f);
        check (at0 < 0.002f && std::abs (at50 - 0.15f) < 0.02f && std::abs (at100 - 0.3f) < 0.03f,
               "Aux source: macro sets how far LFO 1 moves the cutoff (swing " + juce::String (at0, 3) + " / " + juce::String (at50, 3) + " / " + juce::String (at100, 3) + " at macro 0 / 50 / 100 %)");

        // sources are unipolar (as in ElukkaSynth): LFO 1 at +30 % on the cutoff moves it from the knob up to knob + 30 %
        {
            ElukkaNoiseProcessor proc;
            proc.presets.load (proc.presets.indexOf ("Init", "Init"));
            setParam (proc, "flt_cutoff", 1000.0f);
            setParam (proc, "l1_sync", 0.0f); setParam (proc, "l1_rate", 4.0f);
            route (proc, 0, P::SLfo1, P::DCutoff, 0.3f);
            proc.setPlayConfigDetails (0, 2, sr, 512);
            proc.prepareToPlay (sr, 512);
            juce::AudioBuffer<float> buf (2, 512);
            float lo = 1.0f, hi = 0.0f;
            for (int i = 0; i < 200; ++i)
            {
                juce::MidiBuffer midi;
                if (i == 0) midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
                proc.processBlock (buf, midi);
                if (i > 20) { const float v = proc.engine.uiDest[P::DCutoff].load(); lo = juce::jmin (lo, v); hi = juce::jmax (hi, v); }
            }
            const float base = proc.apvts.getParameter ("flt_cutoff")->getValue();
            check (std::abs (lo - base) < 0.01f && std::abs (hi - (base + 0.3f)) < 0.01f,
                   "Unipolar sources: LFO 1 at +30 % moves the cutoff from its knob (" + juce::String (base, 3) + ") up to knob + 30 % (" + juce::String (lo, 3) + " .. " + juce::String (hi, 3) + ")");
        }
        // mapping from a knob's right-click window: amount = what is left above the knob (knob + amount = 100 %)
        {
            ElukkaNoiseProcessor proc;
            proc.presets.load (proc.presets.indexOf ("Init", "Init"));
            setParam (proc, "flt_res", 0.3f);
            const int k = Mod::add (proc.apvts, P::SLfo2, P::DRes);
            const float amt = proc.apvts.getRawParameterValue (P::mx (k, "amt"))->load();
            const auto reach = Mod::reach (proc.apvts, P::DRes);
            check (k >= 0 && std::abs (amt - 0.7f) < 0.001f && std::abs (reach.getEnd() - 1.0f) < 0.001f,
                   "Modulate with: resonance at 30 % gets +70 %, so knob + amount = 100 % (amount " + juce::String (amt, 3) + ")");
        }
        // new defaults: LFOs synced (8 bars, 1/4), S&H synced 1/16 with 50 ms smoothing
        {
            ElukkaNoiseProcessor proc;
            proc.presets.load (proc.presets.indexOf ("Init", "Init"));
            auto v = [&] (const char* id) { return proc.apvts.getRawParameterValue (id)->load(); };
            check (v ("l1_sync") > 0.5f && P::syncRateNames[(int) v ("l1_div")] == "8 Bars" && v ("l2_sync") > 0.5f && P::syncRateNames[(int) v ("l2_div")] == "1/4"
                   && v ("sh_sync") > 0.5f && P::syncRateNames[(int) v ("sh_div")] == "1/16" && std::abs (v ("sh_smooth") - 50.0f) < 0.01f,
                   "Defaults: LFO 1 synced 8 bars, LFO 2 synced 1/4, S&H synced 1/16 with 50 ms smoothing");
        }

        // every destination answers modulation: macro +50 % moves each one off its knob
        int moved = 0;
        juce::String stuck;
        for (int d = 0; d < P::NumDst; ++d)
        {
            ElukkaNoiseProcessor proc;
            proc.presets.load (0);
            for (int k = 0; k < P::numMatrixSlots; ++k) setParam (proc, P::mx (k, "src"), 0.0f);
            route (proc, 0, P::SMacro, d, d == P::DAmp ? -0.5f : (proc.apvts.getParameter (P::dests[d].paramID) != nullptr && proc.apvts.getParameter (P::dests[d].paramID)->getValue() > 0.5f ? -0.4f : 0.4f));
            setParam (proc, "macro", 100.0f);
            RenderOpts o; o.seconds = 0.2; o.noteOff = 0.2;
            render (proc, o);
            const float base = d == P::DAmp ? 1.0f : proc.apvts.getParameter (P::dests[d].paramID)->getValue();
            if (std::abs (proc.engine.uiDest[d].load() - base) > 0.3f) ++moved; else stuck += juce::String (P::dests[d].name) + " ";
        }
        check (moved == P::NumDst, "All " + juce::String ((int) P::NumDst) + " destinations follow the matrix (" + juce::String (moved) + " moved)" + (stuck.isEmpty() ? "" : ": stuck " + stuck));

        ElukkaNoiseProcessor any;
        check (any.apvts.getParameter ("dly_sat") == nullptr, "Delay saturation removed (v0.9.3)");

        for (auto [name, beats, bandPass] : { std::tuple<const char*, float, bool> { "Random Circuit", 0.25f, true }, { "Psy Chaos", 0.25f, true }, { "Glitch Storm", 0.125f, true },
                                              { "Digital Machinery", 0.5f, false } })
        {
            ElukkaNoiseProcessor proc;
            const int idx = proc.presets.indexOf (name, "FX");
            proc.presets.load (juce::jmax (0, idx));
            auto v = [&] (const char* id) { return proc.apvts.getRawParameterValue (id)->load(); };
            bool shToCutoff = false;
            for (int k = 0; k < P::numMatrixSlots; ++k)
                if ((int) v (P::mx (k, "src").toRawUTF8()) == P::SSH && (int) v (P::mx (k, "dst").toRawUTF8()) == P::DCutoff + 1) shToCutoff = true;
            check (idx >= 0 && ((int) v ("flt_type") == P::FBandPass) == bandPass && v ("sh_sync") > 0.5f && std::abs (P::syncRateBeats[(int) v ("sh_div")] - beats) < 1.0e-4f
                   && shToCutoff && v ("gate_on") > 0.5f,
                   juce::String (name) + ": " + (bandPass ? "band pass" : "low pass") + ", synced S&H (" + juce::String (beats * 4.0f, 2) + " beats) on the cutoff, trance gate on");
        }

        // S&H Jump, fixed at 15 % (no knob): bigger steps, fewer near-repeats, still 0..1; the LFOs keep plain random
        {
            struct Stats { double meanStep = 0.0, small = 0.0; bool inRange = true; };
            auto steps = [] (float jump)
            {
                LFO l;
                l.prepare (48000.0, 99u);
                LFO::Params p; p.wave = P::WSampleHold; p.unipolar = true; p.smoothMs = 0.0f; p.rateHz = 100.0; p.jump = jump;
                Stats s;
                float prev = l.process (p, 480);
                const int n = 20000;
                for (int i = 0; i < n; ++i)
                {
                    const float v = l.process (p, 480);   // 480 samples at 100 Hz = one new value per call
                    s.meanStep += std::abs (v - prev) / n;
                    if (std::abs (v - prev) < 0.1f) s.small += 1.0 / n;
                    if (v < 0.0f || v > 1.0f) s.inRange = false;
                    prev = v;
                }
                return s;
            };
            const auto plain = steps (0.0f), fixed = steps (0.15f);
            ElukkaNoiseProcessor any;
            check (fixed.inRange && fixed.meanStep > plain.meanStep + 0.03 && fixed.small < plain.small && any.apvts.getParameter ("sh_jump") == nullptr,
                   "S&H Jump fixed at 15 %: average step " + juce::String (juce::roundToInt (plain.meanStep * 100.0)) + " % -> "
                   + juce::String (juce::roundToInt (fixed.meanStep * 100.0)) + " %, steps under 10 %: " + juce::String (juce::roundToInt (plain.small * 100.0)) + " % -> "
                   + juce::String (juce::roundToInt (fixed.small * 100.0)) + " %; no Jump parameter");

            ElukkaNoiseProcessor proc;
            proc.seedRandom (21);
            proc.presets.load (proc.presets.indexOf ("Random Circuit", "FX"));
            setParam (proc, "macro", 60.0f);
            RenderOpts o; o.sr = 48000.0; o.seconds = 4.0; o.noteOn = 0.05; o.noteOff = 3.5;
            auto b = render (proc, o);
            const auto cl = detectClicks (b, 8.0f, 2400 + 64);
            check (cl.clicks == 0 && ! hasNaN (b), "Random Circuit (with the fixed Jump): no clicks (worst " + juce::String (cl.worstSigmas, 1) + " sigma)");
        }
    }

    // 7g. Typing values: what a knob shows can be typed back, percentages as whole numbers.
    std::cout << "\n== Typed values ==\n";
    {
        ElukkaNoiseProcessor proc;
        auto typed = [&] (const char* id, const char* text)
        {
            auto* p = proc.apvts.getParameter (id);
            return p->convertFrom0to1 (p->getValueForText (text));
        };
        struct Case { const char* id; const char* text; float expect; };
        const Case cases[] { { "flt_res", "50", 0.5f }, { "flt_res", "50%", 0.5f }, { "dly_wet", "25", 0.25f }, { "noise_stereo", "150", 1.5f },
                             { "macro", "63", 63.0f }, { "flt_cutoff", "2k", 2000.0f }, { "flt_cutoff", "2.5 kHz", 2500.0f }, { "flt_cutoff", "440", 440.0f },
                             { "env_release", "1.5 s", 1500.0f }, { "env_release", "300", 300.0f }, { "rev_decay", "4", 4.0f },
                             { "pan", "30L", -0.3f }, { "pan", "C", 0.0f }, { "pan", "45 R", 0.45f }, { "noise_colour", "pink", 0.5f },
                             { "out_level", "-6", -6.0f }, { "gate_swing", "60", 60.0f } };
        int ok = 0;
        for (auto& c : cases)
        {
            const float v = typed (c.id, c.text);
            if (std::abs (v - c.expect) <= std::abs (c.expect) * 0.002f + 1.0e-4f) ++ok;
            else std::cout << "      " << c.id << " typed \"" << c.text << "\" gave " << v << " (expected " << c.expect << ")\n";
        }
        check (ok == (int) std::size (cases), "Typed values read as shown: 50 = 50 %, 2k = 2 kHz, 1.5 s, 30L, pink ... (" + juce::String (ok) + " / " + juce::String ((int) std::size (cases)) + ")");
        // every float parameter: its own text reads back to (nearly) the same value
        int round = 0, total = 0;
        for (auto* prm : proc.getParameters())
            if (auto* fp = dynamic_cast<juce::AudioParameterFloat*> (prm))
                for (float x : { 0.13f, 0.5f, 0.87f })
                {
                    ++total;
                    auto* rp = static_cast<juce::RangedAudioParameter*> (fp);
                    const auto text = rp->getText (x, 64);
                    const float back = rp->getValueForText (text);
                    // compare in real units, allowing the display's rounding
                    const float a = fp->convertFrom0to1 (x), b = fp->convertFrom0to1 (back);
                    const float tol = 0.006f * (fp->range.end - fp->range.start) + 0.01f * std::abs (a);
                    if (std::abs (a - b) <= tol) ++round;
                    else if (total - round < 6) std::cout << "      " << fp->paramID << ": " << a << " -> \"" << text << "\" -> " << b << "\n";
                }
        check (round == total, "Every float parameter's displayed text types back to the same value (" + juce::String (round) + " / " + juce::String (total) + ")");
    }

    // 7h. Init: plain white noise. Nothing modulates it, the gate is off, the spectrum is flat (an octave band at
    // 1 kHz and one at 8 kHz carry the energy white noise has: 8x more at 8 kHz, +9 dB), the macro changes nothing.
    std::cout << "\n== Init ==\n";
    {
        auto renderInit = [&] (float macro)
        {
            ElukkaNoiseProcessor proc;
            proc.seedRandom (13);
            proc.presets.load (proc.presets.indexOf ("Init", "Init"));
            setParam (proc, "macro", macro);
            setParam (proc, "dly_on", 0.0f); setParam (proc, "rev_on", 0.0f);   // the effects follow their own presets' macro mappings
            RenderOpts o; o.sr = sr; o.seconds = 2.0; o.noteOn = 0.0; o.noteOff = 2.0;
            return std::make_pair (render (proc, o), &proc == nullptr);
        };
        ElukkaNoiseProcessor proc;
        const int idx = proc.presets.indexOf ("Init", "Init");
        proc.presets.load (juce::jmax (0, idx));
        int rows = 0;
        for (int k = 0; k < P::numMatrixSlots; ++k) rows += proc.apvts.getRawParameterValue (P::mx (k, "src"))->load() > 0.5f ? 1 : 0;
        auto v = [&] (const char* id) { return proc.apvts.getRawParameterValue (id)->load(); };
        check (idx >= 0 && rows == 0 && v ("gate_on") < 0.5f && v ("dly_wet") == 0.0f && v ("rev_wet") == 0.0f && v ("noise_colour") == 1.0f
               && v ("flt_cutoff") >= 19999.0f && v ("flt_res") == 0.0f && std::abs (v ("hp_cutoff") - 500.0f) < 1.0f,
               "Init: white noise, high pass 500 Hz, filter open, no modulation, no gate, no delay or reverb");
        auto a = renderInit (0.0f).first, b = renderInit (100.0f).first;
        // band energy with a 4th-order band pass (two cascaded SVFs) around f
        auto band = [&] (const juce::AudioBuffer<float>& x, float f)
        {
            const float g = std::tan (juce::MathConstants<float>::pi * f / (float) sr), k = 1.41421356f;
            const float a1 = 1.0f / (1.0f + g * (g + k)), a2 = g * a1, a3 = g * a2;
            float s[4] {};
            double e = 0.0;
            for (int n = (int) (0.2 * sr); n < x.getNumSamples(); ++n)
            {
                float y = x.getSample (0, n);
                for (int st = 0; st < 2; ++st)
                {
                    float& i1 = s[st * 2]; float& i2 = s[st * 2 + 1];
                    const float v3 = y - i2, v1 = a1 * i1 + a2 * v3, v2 = i2 + a2 * i1 + a3 * v3;
                    i1 = 2.0f * v1 - i1; i2 = 2.0f * v2 - i2;
                    y = v1 * k;
                }
                e += (double) y * y;
            }
            return e;
        };
        // the same measurement on reference white noise (the band filters' own tilt cancels out)
        juce::AudioBuffer<float> ref (1, a.getNumSamples());
        juce::Random rng (99);
        for (int n = 0; n < ref.getNumSamples(); ++n) ref.setSample (0, n, rng.nextFloat() * 2.0f - 1.0f);
        const float tilt = (float) (10.0 * std::log10 (band (a, 8000.0f) / band (a, 1000.0f)));
        const float refTilt = (float) (10.0 * std::log10 (band (ref, 8000.0f) / band (ref, 1000.0f)));
        check (std::abs (tilt - refTilt) < 0.5f, "Init spectrum is white: 8 kHz vs 1 kHz band " + juce::String (tilt, 2) + " dB, reference white noise " + juce::String (refTilt, 2) + " dB");
        const float la = rms (a, (int) (0.5 * sr), (int) (2.0 * sr)), lb = rms (b, (int) (0.5 * sr), (int) (2.0 * sr));
        check (std::abs (juce::Decibels::gainToDecibels (lb / la)) < 0.2f, "Init: the macro changes nothing in the instrument (" + juce::String (juce::Decibels::gainToDecibels (la), 1) + " dB at 0 % and 100 %, effects off)");
    }

    // 7i. v0.9.10: macro range, effects order, reverb filters, wider resonance
    std::cout << "\n== Filter 2 ==\n";
    {
        // Init (white noise, main filter open, effects off): the share above 2 kHz with filter 2 at its default (open),
        // as a 300 Hz low pass, a 5 kHz high pass and a 1 kHz band pass
        auto tone = [&] (std::function<void (ElukkaNoiseProcessor&)> setup, float macro = 0.0f)
        {
            ElukkaNoiseProcessor proc;
            proc.seedRandom (7);
            proc.presets.load (proc.presets.indexOf ("Init", "Init"));
            setParam (proc, "dly_on", 0.0f); setParam (proc, "rev_on", 0.0f);
            setParam (proc, "hp_cutoff", 20.0f);   // full-range white noise (Init's high pass is 500 Hz since v0.9.22)
            setup (proc);
            setParam (proc, "macro", macro);
            RenderOpts o; o.sr = sr; o.seconds = 1.0; o.noteOff = 1.0;
            auto b = render (proc, o);
            return std::make_pair (brightness (b, (int) (0.2 * sr), (int) (1.0 * sr), sr), rms (b, (int) (0.2 * sr), (int) (1.0 * sr)));
        };
        auto v = [] (ElukkaNoiseProcessor& p, const char* id) { return p.apvts.getRawParameterValue (id)->load(); };
        {
            ElukkaNoiseProcessor proc;
            proc.presets.load (proc.presets.indexOf ("Init", "Init"));
            check ((int) v (proc, "flt2_type") == P::FLowPass && (int) v (proc, "flt2_slope") == 1 && v (proc, "flt2_cutoff") >= 19999.0f && v (proc, "flt2_res") == 0.0f,
                   "Filter 2 defaults: 12 dB low pass at 20 kHz, no resonance (open)");
        }
        const auto open = tone ([] (ElukkaNoiseProcessor&) {});
        const auto lp = tone ([] (ElukkaNoiseProcessor& p) { setParam (p, "flt2_cutoff", 300.0f); });
        const auto hp = tone ([] (ElukkaNoiseProcessor& p) { setParam (p, "flt2_type", (float) P::FHighPass); setParam (p, "flt2_cutoff", 5000.0f); });
        const auto bp = tone ([] (ElukkaNoiseProcessor& p) { setParam (p, "flt2_type", (float) P::FBandPass); setParam (p, "flt2_cutoff", 1000.0f); });
        const auto lp24 = tone ([] (ElukkaNoiseProcessor& p) { setParam (p, "flt2_cutoff", 300.0f); setParam (p, "flt2_slope", 0.0f); });
        check (open.first > 0.75f && lp.first < 0.25f * open.first && hp.first > open.first && hp.second < 0.9f * open.second
               && bp.first < 0.8f * open.first && bp.first > 2.0f * lp.first && bp.second < 0.7f * open.second && lp24.first < 0.6f * lp.first,
               "Filter 2 shapes the tone after the main filter: open = white (2 kHz+ share " + juce::String (open.first, 2) + "), 300 Hz LP " + juce::String (lp.first, 2)
               + " (24 dB " + juce::String (lp24.first, 2) + "), 5 kHz HP " + juce::String (hp.first, 2) + " at " + juce::String (juce::Decibels::gainToDecibels (hp.second / open.second), 1)
               + " dB, 1 kHz BP " + juce::String (bp.first, 2) + " at " + juce::String (juce::Decibels::gainToDecibels (bp.second / open.second), 1) + " dB");

        // at its default the filter is out of the path (a hair below 20 kHz it runs and takes the top octave off white noise);
        // it crossfades in and out, so moving the knob off / back to 20 kHz mid-note never clicks
        {
            auto renderAt = [&] (float cutoff)
            {
                ElukkaNoiseProcessor proc;
                proc.seedRandom (7);
                proc.presets.load (proc.presets.indexOf ("Init", "Init"));
                setParam (proc, "dly_on", 0.0f); setParam (proc, "rev_on", 0.0f);
                setParam (proc, "flt2_cutoff", cutoff);
                RenderOpts o; o.sr = sr; o.seconds = 1.0; o.noteOff = 1.0;
                return render (proc, o);
            };
            auto a = renderAt (20000.0f), b = renderAt (19980.0f);
            juce::AudioBuffer<float> d (2, a.getNumSamples());
            for (int c = 0; c < 2; ++c) for (int i = 0; i < a.getNumSamples(); ++i) d.setSample (c, i, a.getSample (c, i) - b.getSample (c, i));
            const float diffDb = juce::Decibels::gainToDecibels (rms (d, 0, d.getNumSamples()) / juce::jmax (1.0e-9f, rms (a, 0, a.getNumSamples())));
            // engaging and disengaging mid-note: cutoff 20 kHz <-> 1 kHz every 0.25 s
            ElukkaNoiseProcessor proc;
            proc.seedRandom (7);
            proc.presets.load (proc.presets.indexOf ("Init", "Init"));
            setParam (proc, "dly_on", 0.0f); setParam (proc, "rev_on", 0.0f);
            RenderOpts o; o.sr = sr; o.seconds = 2.0; o.noteOff = 2.0;
            o.perBlock = [] (ElukkaNoiseProcessor& p, double t) { setParam (p, "flt2_cutoff", (int) (t / 0.25) % 2 == 0 ? 20000.0f : 1000.0f); };
            auto e = render (proc, o);
            const auto c = detectClicks (e, 8.0f, (int) (0.05 * sr));
            check (diffDb < -3.0f && diffDb > -20.0f && c.clicks == 0 && ! hasNaN (e), "Filter 2 at its default is out of the path (running at 19.98 kHz it changes the output by " + juce::String (diffDb, 1)
                                                      + " dB: the top octave); engaging / disengaging it every 0.25 s mid-note never clicks (worst " + juce::String (c.worstSigmas, 1) + " sigma)");
        }

        // drive (v0.9.22): saturation before filter 2, as on the main filter: louder peaks flattened, a different signal
        {
            auto driven = [&] (float drive)
            {
                ElukkaNoiseProcessor proc;
                proc.seedRandom (7);
                proc.presets.load (proc.presets.indexOf ("Init", "Init"));
                setParam (proc, "dly_on", 0.0f); setParam (proc, "rev_on", 0.0f);
                setParam (proc, "flt2_drive", drive);   // cutoff left open: drive alone takes the filter out of its bypass
                RenderOpts o; o.sr = sr; o.seconds = 1.0; o.noteOff = 1.0;
                return render (proc, o);
            };
            auto a = driven (0.0f), b = driven (1.0f);
            const float crestA = peak (a) / rms (a, (int) (0.2 * sr), a.getNumSamples()), crestB = peak (b) / rms (b, (int) (0.2 * sr), b.getNumSamples());
            const auto cl = detectClicks (b, 8.0f, (int) (0.05 * sr));
            check (crestB < 0.85f * crestA && cl.clicks == 0 && ! hasNaN (b), "Filter 2 Drive 100 % saturates before the filter: crest factor " + juce::String (crestA, 2) + " -> " + juce::String (crestB, 2));
        }

        // the matrix reaches it: the macro closing filter 2 darkens the sound; the knob itself stays
        {
            ElukkaNoiseProcessor proc;
            proc.presets.load (proc.presets.indexOf ("Init", "Init"));
            setParam (proc, "dly_on", 0.0f); setParam (proc, "rev_on", 0.0f);
            route (proc, 0, P::SMacro, P::DCutoff2, -0.6f);
            setParam (proc, "macro", 100.0f);
            RenderOpts o; o.sr = sr; o.seconds = 1.0; o.noteOff = 1.0;
            auto b = render (proc, o);
            const float dark = brightness (b, (int) (0.2 * sr), (int) (1.0 * sr), sr);
            check (dark < 0.5f * open.first && v (proc, "flt2_cutoff") >= 19999.0f && proc.engine.uiDest[P::DCutoff2].load() < 0.5f,
                   "Macro -> Filter 2 Cutoff -60 % at 100 %: 2 kHz+ share " + juce::String (dark, 2) + " (open " + juce::String (open.first, 2) + "), the knob untouched");
        }

        // type and slope switched every 0.25 s mid-note at 1 kHz with resonance: crossfaded, no clicks
        {
            ElukkaNoiseProcessor proc;
            proc.seedRandom (7);
            proc.presets.load (proc.presets.indexOf ("Init", "Init"));
            setParam (proc, "dly_on", 0.0f); setParam (proc, "rev_on", 0.0f);
            setParam (proc, "flt2_cutoff", 1000.0f); setParam (proc, "flt2_res", 0.4f);
            RenderOpts o; o.sr = sr; o.seconds = 2.0; o.noteOff = 2.0;
            o.perBlock = [] (ElukkaNoiseProcessor& p, double t)
            {
                const int step = (int) (t / 0.25);
                setParam (p, "flt2_type", (float) (step % 3));
                setParam (p, "flt2_slope", (float) ((step / 3) % 2));
            };
            auto b = render (proc, o);
            const auto c = detectClicks (b, 8.0f, (int) (0.05 * sr));
            check (c.clicks == 0 && ! hasNaN (b), "Filter 2 type and slope switched every 0.25 s mid-note never click (worst " + juce::String (c.worstSigmas, 1) + " sigma)");
        }

        // the project state and an instrument preset carry it
        {
            ElukkaNoiseProcessor a;
            setParam (a, "flt2_type", (float) P::FHighPass); setParam (a, "flt2_slope", 0.0f); setParam (a, "flt2_cutoff", 800.0f); setParam (a, "flt2_res", 0.35f);
            juce::MemoryBlock mb;
            a.getStateInformation (mb);
            ElukkaNoiseProcessor b;
            b.setStateInformation (mb.getData(), (int) mb.getSize());
            auto t = a.presets.toTree ("F2", "User");
            ElukkaNoiseProcessor c;
            c.presets.applyTree (t);
            auto same = [&] (ElukkaNoiseProcessor& p) { return (int) v (p, "flt2_type") == P::FHighPass && (int) v (p, "flt2_slope") == 0 && std::abs (v (p, "flt2_cutoff") - 800.0f) < 1.0f && std::abs (v (p, "flt2_res") - 0.35f) < 0.001f; };
            check (same (b) && same (c) && P::destForParam ("flt2_cutoff") == P::DCutoff2 && P::destForParam ("flt2_res") == P::DRes2 && juce::String (P::destGroup (P::DRes2)) == "Filter",
                   "Filter 2 settings round-trip through the project state and an instrument preset; both knobs are Filter destinations");
        }
    }

    std::cout << "\n== Macro range / effects order / reverb filters / resonance ==\n";
    {
        auto cutoffWith = [&] (float lo, float hi, float macro)
        {
            ElukkaNoiseProcessor proc;
            proc.presets.load (proc.presets.indexOf ("Clean Noise Rise", "Risers / Falls"));
            setParam (proc, "macro_lo", lo); setParam (proc, "macro_hi", hi); setParam (proc, "macro", macro);
            RenderOpts o; o.sr = sr; o.seconds = 0.4; o.noteOff = 0.4;
            render (proc, o);
            return proc.engine.uiDest[P::DCutoff].load();
        };
        const float a = cutoffWith (10.0f, 100.0f, 0.0f), b = cutoffWith (0.0f, 100.0f, 10.0f);
        const float c = cutoffWith (0.0f, 80.0f, 100.0f), d = cutoffWith (0.0f, 100.0f, 80.0f);
        const float e = cutoffWith (10.0f, 80.0f, 50.0f), f = cutoffWith (0.0f, 100.0f, 45.0f);
        check (std::abs (a - b) < 0.002f && std::abs (c - d) < 0.002f && std::abs (e - f) < 0.002f,
               "Macro range: floor 10 % at macro 0 = old 10 %, ceiling 80 % at macro 100 = old 80 %, 10..80 at 50 = old 45 %");

        auto fx = [&] (std::function<float (double)> orderAt, ClickReport* clicks)
        {
            ElukkaNoiseProcessor proc;
            proc.seedRandom (14);
            proc.presets.load (proc.presets.indexOf ("Hypnotic Pulse", "FX"));
            setParam (proc, "macro", 60.0f);
            setParam (proc, "dly_wet", 0.2f); setParam (proc, "rev_wet", 0.35f);
            RenderOpts o; o.sr = sr; o.seconds = 3.0; o.noteOn = 0.05; o.noteOff = 2.5;
            o.perBlock = [orderAt] (ElukkaNoiseProcessor& p, double t) { setParam (p, "fx_order", orderAt (t)); };
            auto b2 = render (proc, o);
            if (clicks != nullptr) *clicks = detectClicks (b2, 8.0f, (int) (0.05 * sr) + 64);
            return b2;
        };
        auto d0 = fx ([] (double) { return 0.0f; }, nullptr), d1 = fx ([] (double) { return 1.0f; }, nullptr);
        double diff = 0.0, ref = 0.0;
        for (int i = (int) (0.5 * sr); i < (int) (2.5 * sr); ++i) { const double x = d0.getSample (0, i) - d1.getSample (0, i); diff += x * x; ref += (double) d0.getSample (0, i) * d0.getSample (0, i); }
        const float diffDb = (float) (10.0 * std::log10 (diff / ref));
        ClickReport cr;
        fx ([] (double t) { return std::fmod (t, 0.6) < 0.3 ? 0.0f : 1.0f; }, &cr);
        check (diffDb > -30.0f && cr.clicks == 0, "Effects order: reverb > delay differs from delay > reverb (" + juce::String (diffDb, 1)
                                                  + " dB); swapping every 0.3 s mid-note never clicks (worst " + juce::String (cr.worstSigmas, 1) + " sigma)");

        // reverb filters: wet-only tails, high band (above 3 kHz) and low band (below 200 Hz) shares
        auto tail = [&] (float tone)   // v0.9.23: one Tone knob (-0.87 = low pass 800 Hz, +0.78 = high pass 1 kHz)
        {
            ElukkaNoiseProcessor proc;
            proc.seedRandom (15);
            proc.presets.load (proc.presets.indexOf ("Init", "Init"));
            setParam (proc, "hp_cutoff", 20.0f);   // full-range white noise in (Init's high pass is 500 Hz since v0.9.22)
            setParam (proc, "rev_wet", 1.0f); setParam (proc, "rev_tone", tone); setParam (proc, "rev_decay", 3.0f);
            RenderOpts o; o.sr = sr; o.seconds = 2.0; o.noteOn = 0.0; o.noteOff = 0.5;
            return render (proc, o);
        };
        auto share = [&] (const juce::AudioBuffer<float>& x, float hz, bool above)
        {
            const float a1 = 1.0f - std::exp (-juce::MathConstants<float>::twoPi * hz / (float) sr);
            double lpE = 0.0, all = 0.0; float l1 = 0.0f, l2 = 0.0f;
            for (int i = (int) (0.6 * sr); i < (int) (1.6 * sr); ++i)
            {
                const float v = x.getSample (0, i);
                l1 += a1 * (v - l1); l2 += a1 * (l1 - l2);
                const float part = above ? v - l2 : l2;
                lpE += (double) part * part; all += (double) v * v;
            }
            return (float) (lpE / juce::jmax (1.0e-20, all));
        };
        const auto open = tail (0.0f), dark = tail (-0.87f), thin = tail (0.78f);
        const float hiOpen = share (open, 3000.0f, true), hiDark = share (dark, 3000.0f, true);
        const float loOpen = share (open, 200.0f, false), loThin = share (thin, 200.0f, false);
        check (hiDark < hiOpen * 0.3f && loThin < loOpen * 0.3f && std::abs (P::reverbToneLp (-0.87f) - 800.0f) < 20.0f && std::abs (P::reverbToneHp (0.78f) - 1000.0f) < 30.0f,
               "Reverb Tone -87 % (low pass 800 Hz) darkens the tail (highs " + juce::String (juce::roundToInt (100.0f * hiOpen)) + "% -> "
                                                                 + juce::String (juce::roundToInt (100.0f * hiDark)) + "%), Tone +78 % (high pass 1 kHz) thins it (lows " + juce::String (100.0f * loOpen, 1)
                                                                 + "% -> " + juce::String (100.0f * loThin, 1) + "%)");
        check (std::abs (1.0f / NoiseFilter::kFor (1.0f) - 5.507f) < 0.01f && std::abs (1.0f / NoiseFilter::kFor (0.0f) - 0.707f) < 0.01f,
               "Resonance: Q 0.71 .. 5.5 (was up to 12.7)");
    }

    // 7j. Compressor
    std::cout << "\n== Compressor ==\n";
    {
        // a riser held at 25 % and at 100 %: compression narrows the gap; makeup brings the loudness back
        auto levels = [&] (float amount, bool makeup)
        {
            float lv[2];
            for (int k = 0; k < 2; ++k)
            {
                ElukkaNoiseProcessor proc;
                proc.seedRandom (16);
                proc.presets.load (proc.presets.indexOf ("Clean Noise Rise", "Risers / Falls"));
                setParam (proc, "macro", k == 0 ? 25.0f : 100.0f);
                setParam (proc, "comp_amt", amount); setParam (proc, "comp_speed", 0.5f); setParam (proc, "comp_makeup", makeup ? 1.0f : 0.0f);
                RenderOpts o; o.sr = sr; o.seconds = 4.0; o.noteOn = 0.0; o.noteOff = 4.0;
                auto b = render (proc, o);
                lv[k] = juce::Decibels::gainToDecibels (rms (b, (int) (2.5 * sr), (int) (4.0 * sr)));
            }
            return std::make_pair (lv[0], lv[1]);
        };
        const auto dry = levels (0.0f, true), wet = levels (0.7f, false), made = levels (0.7f, true);
        const float gapDry = dry.second - dry.first, gapWet = wet.second - wet.first;
        check (gapWet < gapDry - 3.0f, "Compression narrows a riser's level range: 25 % -> 100 % rises " + juce::String (gapDry, 1) + " dB dry, "
                                       + juce::String (gapWet, 1) + " dB at amount 70 %");
        check (dry.second - wet.second > 4.0f && std::abs (made.second - dry.second) < 1.5f && std::abs (made.first - dry.first) < 1.5f,
               "Makeup: without it 70 % compression is " + juce::String (dry.second - wet.second, 1) + " dB quieter; with it the loudness is within "
               + juce::String (juce::jmax (std::abs (made.second - dry.second), std::abs (made.first - dry.first)), 1) + " dB of the original");

        // makeup keeps the loudness: held notes at several amounts / speeds, and amount sweeps and jumps that add no
        // loudness change of their own (400 ms windows; the same noise with and without the compressor)
        {
            struct Mat { const char* preset; float macro; };
            auto renderWith = [&] (const Mat& m, std::function<float (double)> amountAt, float speed)
            {
                ElukkaNoiseProcessor pr;
                pr.seedRandom (21);
                pr.presets.load (pr.presets.indexOf (m.preset, {}));
                setParam (pr, "macro", m.macro); setParam (pr, "comp_speed", speed); setParam (pr, "comp_makeup", 1.0f);
                RenderOpts o2; o2.sr = sr; o2.seconds = 4.5; o2.noteOn = 0.0; o2.noteOff = 4.5;
                o2.perBlock = [amountAt] (ElukkaNoiseProcessor& p, double t) { setParam (p, "comp_amt", amountAt (t)); };
                return render (pr, o2);
            };
            auto db = [&] (const juce::AudioBuffer<float>& b, double from, double len) { return juce::Decibels::gainToDecibels (rms (b, (int) (from * sr), (int) ((from + len) * sr))); };
            float heldWorst = 0.0f;
            for (const Mat& m : { Mat { "Clean Noise Rise", 100.0f }, Mat { "Rolling Gate Rise", 50.0f }, Mat { "Random Circuit", 60.0f } })
            {
                auto dry = renderWith (m, [] (double) { return 0.0f; }, 0.5f);
                for (float speed : { 0.0f, 1.0f })
                    for (float a : { 0.5f, 1.0f })
                    {
                        auto wet = renderWith (m, [a] (double) { return a; }, speed);
                        heldWorst = juce::jmax (heldWorst, std::abs (db (wet, 2.0, 2.4) - db (dry, 2.0, 2.4)));
                    }
            }
            check (heldWorst < 1.0f, "Makeup matches the loudness without compression: held notes at 50 / 100 %, slow and fast, 3 presets (worst " + juce::String (heldWorst, 2) + " dB)");
            auto drift = [&] (const Mat& m, std::function<float (double)> amountAt)
            {
                auto dry = renderWith (m, [] (double) { return 0.0f; }, 0.5f), wet = renderWith (m, amountAt, 0.5f);
                float worst = 0.0f;
                for (double t = 0.5; t < 4.0; t += 0.1) worst = juce::jmax (worst, std::abs (db (wet, t, 0.4) - db (dry, t, 0.4)));
                return worst;
            };
            auto sweep = [] (double t) { return (float) juce::jlimit (0.0, 1.0, (t - 1.0) / 2.0); };
            auto jump = [] (double t) { return t < 2.0 ? 0.0f : 0.8f; };
            const float steady = juce::jmax (drift ({ "Clean Noise Rise", 50.0f }, sweep), drift ({ "Clean Noise Rise", 50.0f }, jump),
                                             drift ({ "Rolling Gate Rise", 50.0f }, sweep), drift ({ "Rolling Gate Rise", 50.0f }, jump));
            check (steady < 1.0f, "Amount swept 0 -> 100 % or jumped 0 -> 80 % with makeup: loudness stays within " + juce::String (steady, 2) + " dB (400 ms windows)");
            const Mat moving { "Random Circuit", 60.0f };
            const float held = drift (moving, [] (double) { return 0.8f; }), swept = juce::jmax (drift (moving, sweep), drift (moving, jump));
            check (swept < held + 0.5f, "On moving material (S&H steps) an amount sweep adds no loudness change of its own (" + juce::String (swept, 1)
                                        + " dB vs " + juce::String (held, 1) + " dB with the amount held: compression evening out the steps)");
        }

        // speed: a 30 dB step in noise level; how long until the gain reduction is 90 % of the way there
        auto settle = [&] (float speed)
        {
            Compressor c;
            c.prepare (sr);
            Compressor::Params p; p.amount = 0.8f; p.speed = speed; p.makeup = false;
            juce::Random rng (3);
            std::vector<float> l (32), r (32), gr;
            for (int b = 0; b < (int) (0.6 * sr) / 32; ++b)
            {
                const float level = b * 32 < (int) (0.2 * sr) ? 0.01f : 0.3f;
                for (int i = 0; i < 32; ++i) { l[(size_t) i] = level * (rng.nextFloat() * 2.0f - 1.0f); r[(size_t) i] = level * (rng.nextFloat() * 2.0f - 1.0f); }
                c.process (l.data(), r.data(), 32, p);
                gr.push_back (c.uiGr);
            }
            const float final = gr.back();
            const int stepBlock = (int) (0.2 * sr) / 32;
            for (size_t b = (size_t) stepBlock; b < gr.size(); ++b)
                if (gr[b] < 0.9f * final) return (float) ((double) (b - (size_t) stepBlock) * 32.0 / sr * 1000.0);
            return 999.0f;
        };
        const float fast = settle (1.0f), slow = settle (0.0f);
        check (fast < 8.0f && slow > 25.0f, "Speed: gain reduction reaches 90 % in " + juce::String (fast, 1) + " ms (fast) / " + juce::String (slow, 1) + " ms (slow)");

        // heavy, fast compression on gated noise and the amount jumping up mid-note: no clicks, no overs
        ElukkaNoiseProcessor proc;
        proc.seedRandom (17);
        proc.presets.load (proc.presets.indexOf ("Rolling Gate Rise", "Risers / Falls"));
        setParam (proc, "comp_speed", 1.0f);
        RenderOpts o; o.sr = sr; o.seconds = 4.0; o.noteOn = 0.05; o.noteOff = 3.5;
        o.macroAt = [] (double t) { return (float) juce::jlimit (0.0, 1.0, t / 3.0); };
        o.perBlock = [] (ElukkaNoiseProcessor& p, double t) { setParam (p, "comp_amt", t < 1.0 ? 0.0f : 1.0f); };
        auto b = render (proc, o);
        const auto c = detectClicks (b, 8.0f, (int) (0.05 * sr) + 64);
        check (c.clicks == 0 && peak (b) <= 1.0f, "Full, fast compression on Rolling Gate Rise, switched in mid-note: no clicks (worst " + juce::String (c.worstSigmas, 1)
                                                  + " sigma), peak " + juce::String (peak (b), 2));

        // On switch: off with the amount up sounds exactly like amount 0; switching on and off mid-note never clicks
        {
            auto run = [&] (std::function<void (ElukkaNoiseProcessor&, double)> per)
            {
                ElukkaNoiseProcessor pr;
                pr.seedRandom (19);
                pr.presets.load (pr.presets.indexOf ("Rolling Gate Rise", "Risers / Falls"));
                setParam (pr, "macro", 60.0f); setParam (pr, "comp_speed", 1.0f);
                RenderOpts o2; o2.sr = sr; o2.seconds = 3.0; o2.noteOn = 0.05; o2.noteOff = 2.8;
                o2.perBlock = per;
                return render (pr, o2);
            };
            auto offUp = run ([] (ElukkaNoiseProcessor& p, double) { setParam (p, "comp_amt", 0.9f); setParam (p, "comp_on", 0.0f); });
            auto zero = run ([] (ElukkaNoiseProcessor& p, double) { setParam (p, "comp_amt", 0.0f); });
            float maxDiff = 0.0f;
            for (int i = 0; i < offUp.getNumSamples(); ++i) maxDiff = juce::jmax (maxDiff, std::abs (offUp.getSample (0, i) - zero.getSample (0, i)));
            auto toggled = run ([] (ElukkaNoiseProcessor& p, double t) { setParam (p, "comp_amt", 1.0f); setParam (p, "comp_on", std::fmod (t, 0.5) < 0.25 ? 1.0f : 0.0f); });
            const auto cl = detectClicks (toggled, 8.0f, (int) (0.05 * sr) + 64);
            check (maxDiff == 0.0f && cl.clicks == 0, "Compressor On switch: off = untouched (bit-identical to amount 0); switched on and off every 0.25 s at full amount, no clicks (worst "
                                                      + juce::String (cl.worstSigmas, 1) + " sigma)");
        }

        // order: compressor last (after the reverb) sounds different from first
        auto withOrder = [&] (int ord)
        {
            ElukkaNoiseProcessor pr;
            pr.seedRandom (18);
            pr.presets.load (pr.presets.indexOf ("Hypnotic Pulse", "FX"));
            setParam (pr, "macro", 60.0f); setParam (pr, "comp_amt", 0.8f); setParam (pr, "fx_order", (float) ord);
            RenderOpts o2; o2.sr = sr; o2.seconds = 3.0; o2.noteOn = 0.05; o2.noteOff = 2.5;
            return render (pr, o2);
        };
        auto first = withOrder (0), last = withOrder (3);
        double diff = 0.0, ref = 0.0;
        for (int i = (int) (0.5 * sr); i < (int) (2.9 * sr); ++i) { const double x = first.getSample (0, i) - last.getSample (0, i); diff += x * x; ref += (double) first.getSample (0, i) * first.getSample (0, i); }
        check (10.0 * std::log10 (diff / ref) > -30.0, "Effects order: compressor after the reverb differs from compressor first ("
                                                       + juce::String (10.0 * std::log10 (diff / ref), 1) + " dB)");
    }

    // 7j. v0.9.17: the reverb rebuilt for noise. Decay holds in the band it is defined on while highs and lows die
    // faster, width and the mono sum, ducking on gated noise, the macro bloom on Rolling Gate Rise, every reverb knob
    // and the character menu jumping mid-note, the tail through note-off and the gate, extremes, sample rates.
    std::cout << "\n== Reverb ==\n";
    {
        // T60 of a band on the reverb alone, from an impulse: Schroeder backward-integrated energy, -5 .. -25 dB
        auto bandT60 = [] (NoiseReverb::Params p, float lo, float hi, double rate)
        {
            NoiseReverb r;
            r.prepare (rate);
            const int total = (int) ((1.0 + 2.5 * p.decay) * rate), block = 64;
            std::vector<float> L ((size_t) total, 0.0f), R ((size_t) total, 0.0f);
            L[(size_t) (0.5 * rate)] = R[(size_t) (0.5 * rate)] = 1.0f;
            for (int pos = 0; pos < total; pos += block) r.process (L.data() + pos, R.data() + pos, juce::jmin (block, total - pos), p);
            // four-pole band limits (four first-order low passes, then four first-order high passes: 24 dB / octave each
            // side), so a fast-dying band is not swamped by leakage from the mids
            const float aL = 1.0f - std::exp (-juce::MathConstants<float>::twoPi * hi / (float) rate), aH = 1.0f - std::exp (-juce::MathConstants<float>::twoPi * lo / (float) rate);
            float l[4] {}, h[4] {};
            std::vector<double> edc ((size_t) total);
            for (int i = 0; i < total; ++i)
            {
                float x = L[(size_t) i];
                for (auto& v : l) { v += aL * (x - v); x = v; }
                for (auto& v : h) { v += aH * (x - v); x -= v; }
                edc[(size_t) i] = (double) x * x;
            }
            for (int i = total - 2; i >= 0; --i) edc[(size_t) i] += edc[(size_t) i + 1];
            const int from = (int) (0.58 * rate);   // right after the impulse has spread through the lines
            const double top = 10.0 * std::log10 (edc[(size_t) from] + 1.0e-30);
            int t5 = -1, t25 = -1;
            for (int i = from; i < total; ++i)
            {
                const double db = 10.0 * std::log10 (edc[(size_t) i] + 1.0e-30) - top;
                if (t5 < 0 && db <= -5.0) t5 = i;
                if (db <= -25.0) { t25 = i; break; }
            }
            return t5 >= 0 && t25 > t5 ? (float) (3.0 * (t25 - t5) / rate) : 999.0f;
        };
        NoiseReverb::Params base;
        base.mix = 1.0f; base.preDelayMs = 0.0f; base.hpHz = 110.0f; base.lpHz = 20000.0f;
        auto at = [&base] (float decay, float size, float lp) { auto p = base; p.decay = decay; p.size = size; p.lpHz = lp; return p; };
        const float m3 = bandT60 (at (3.0f, 0.65f, 20000.0f), 500.0f, 2000.0f, sr), m12 = bandT60 (at (12.0f, 1.0f, 20000.0f), 500.0f, 2000.0f, sr),
                    m05 = bandT60 (at (0.5f, 0.3f, 20000.0f), 500.0f, 2000.0f, sr);
        check (std::abs (m3 - 3.0f) < 0.45f && std::abs (m12 - 12.0f) < 2.4f && std::abs (m05 - 0.5f) < 0.1f,
               "Decay is the midrange T60 (500 Hz..2 kHz, within 15 %, 20 % at 12 s): 3 s -> " + juce::String (m3, 1) + " s, 12 s (100 % size) -> " + juce::String (m12, 1) + " s, 0.5 s -> " + juce::String (m05, 2) + " s");
        const float mids4k = bandT60 (at (4.0f, 0.65f, 4000.0f), 500.0f, 2000.0f, sr), highs4k = bandT60 (at (4.0f, 0.65f, 4000.0f), 8000.0f, 16000.0f, sr),
                    lows4k = bandT60 (at (4.0f, 0.65f, 4000.0f), 50.0f, 100.0f, sr);
        check (highs4k < 0.5f * mids4k && lows4k < 0.7f * mids4k,
               "Frequency-dependent decay (Decay 4 s, LP 4 kHz, HP 110 Hz): mids " + juce::String (mids4k, 1) + " s, 8-16 kHz " + juce::String (highs4k, 1) + " s, 50-100 Hz " + juce::String (lows4k, 1) + " s");
        const float m96 = bandT60 (at (3.0f, 0.65f, 20000.0f), 500.0f, 2000.0f, 96000.0), m44 = bandT60 (at (3.0f, 0.65f, 20000.0f), 500.0f, 2000.0f, 44100.0);
        check (std::abs (m96 - m3) < 0.3f && std::abs (m44 - m3) < 0.3f, "The same decay at 44.1 / 96 kHz: " + juce::String (m44, 1) + " / " + juce::String (m96, 1) + " s");

        // width, mono sum, smoothness: Init at 100 % wet, a 1 s note
        auto wetRender = [&] (std::function<void (ElukkaNoiseProcessor&)> setup, double seconds = 3.0)
        {
            ElukkaNoiseProcessor proc;
            proc.seedRandom (15);
            proc.presets.load (proc.presets.indexOf ("Init", "Init"));
            setParam (proc, "rev_wet", 1.0f);
            setup (proc);
            RenderOpts o; o.sr = sr; o.seconds = seconds; o.noteOn = 0.0; o.noteOff = 1.0;
            return render (proc, o);
        };
        {
            auto mono = wetRender ([] (ElukkaNoiseProcessor& p) { setParam (p, "rev_width", 0.0f); });
            auto wide = wetRender ([] (ElukkaNoiseProcessor& p) { setParam (p, "rev_width", 1.0f); });
            float maxDiff = 0.0f;
            for (int i = (int) (1.1 * sr); i < (int) (2.1 * sr); ++i) maxDiff = juce::jmax (maxDiff, std::abs (mono.getSample (0, i) - mono.getSample (1, i)));
            const auto pw = probeReverb (wide, sr);
            check (maxDiff < 1.0e-6f && std::abs (pw.corr) < 0.3f && pw.monoDb > -4.0f,
                   "Width 0 %: left = right; 100 %: correlation " + juce::String (pw.corr, 2) + ", mono sum " + juce::String (pw.monoDb, 1) + " dB (no cancellation)");
            auto riser = wetRender ([] (ElukkaNoiseProcessor& p) { setParam (p, "rev_size", 1.0f); setParam (p, "rev_decay", 6.7f); });
            const auto pr = probeReverb (riser, sr), pd = probeReverb (wetRender ([] (ElukkaNoiseProcessor&) {}), sr);
            check (pr.flutter < 0.3f && pd.flutter < 0.35f && pr.ripple < 3.6f && pd.ripple < 4.3f,
                   "Smooth tail: flutter " + juce::String (pr.flutter, 2) + " / " + juce::String (pd.flutter, 2) + ", spectral ripple " + juce::String (pr.ripple, 2) + " / "
                   + juce::String (pd.ripple, 2) + " dB (riser / default; the old reverb: 0.16 / 0.16, 3.55 / 4.28)");
        }

        // ducking on a 1/16 gate (130 bpm: 58 ms on, 58 ms off), wet only: the pulses are pushed down, the gaps recover
        {
            auto gated = [&] (float duck)
            {
                return wetRender ([duck] (ElukkaNoiseProcessor& p)
                {
                    setParam (p, "gate_on", 1.0f); setParam (p, "gate_div", 5.0f); setParam (p, "gate_depth", 1.0f); setParam (p, "gate_duty", 0.5f);
                    setParam (p, "gate_att", 1.0f); setParam (p, "gate_rel", 5.0f);
                    setParam (p, "rev_decay", 2.0f); setParam (p, "rev_pre", 0.0f); setParam (p, "rev_duck", duck);
                }, 2.5);
            };
            auto off = gated (0.0f), on = gated (1.0f);
            float minRatio = 10.0f, maxRatio = 0.0f;
            for (int w = 0; w < 100; ++w)
            {
                const int a = (int) ((1.0 + 0.01 * w) * sr), b = a + (int) (0.01 * sr);
                const float ratio = rms (on, a, b) / juce::jmax (1.0e-6f, rms (off, a, b));
                minRatio = juce::jmin (minRatio, ratio); maxRatio = juce::jmax (maxRatio, ratio);
            }
            const auto cl = detectClicks (on, 8.0f, (int) (0.05 * sr) + 64);
            check (minRatio < 0.35f && maxRatio > 0.6f && cl.clicks == 0 && ! hasNaN (on),
                   "Ducking 100 % on a 1/16 gate: the wet level drops to " + juce::String (juce::Decibels::gainToDecibels (minRatio), 1) + " dB on a pulse and recovers to "
                   + juce::String (juce::Decibels::gainToDecibels (maxRatio), 1) + " dB in the gap; no clicks (worst " + juce::String (cl.worstSigmas, 1) + " sigma)");
        }

        // the bloom: Rolling Gate Rise's reverb grows with the macro while the ducking keeps the chops defined
        {
            auto rolling = [&] (float macro, float duck)
            {
                ElukkaNoiseProcessor proc;
                proc.seedRandom (8);
                proc.presets.load (proc.presets.indexOf ("Rolling Gate Rise", "Risers / Falls"));
                proc.revPresets.loadByName ("Ducked Bloom");   // v0.9.20: the bloom is the reverb preset's (Mix +30 % late, Decay +25 %, Duck 70 %)
                setParam (proc, "rev_wet", 0.1f);
                setParam (proc, "macro", macro); setParam (proc, "dly_on", 0.0f); setParam (proc, "env_release", 30.0f);
                if (duck >= 0.0f) setParam (proc, "rev_duck", duck);
                RenderOpts o; o.sr = sr; o.seconds = 2.6; o.noteOn = 0.05; o.noteOff = 1.5;
                return render (proc, o);
            };
            auto lo = rolling (0.0f, -1.0f), hi = rolling (100.0f, -1.0f);
            const float tailLo = rms (lo, (int) (1.8 * sr), (int) (2.3 * sr)), tailHi = rms (hi, (int) (1.8 * sr), (int) (2.3 * sr));
            auto pulseDepth = [&] (const juce::AudioBuffer<float>& b)
            {
                float lo2 = 1.0e9f, hi2 = 0.0f;
                for (int w = 0; w < 60; ++w) { const float v = rms (b, (int) ((0.8 + 0.01 * w) * sr), (int) ((0.81 + 0.01 * w) * sr)); lo2 = juce::jmin (lo2, v); hi2 = juce::jmax (hi2, v); }
                return juce::Decibels::gainToDecibels (hi2 / juce::jmax (1.0e-6f, lo2));
            };
            auto ducked = rolling (50.0f, -1.0f), flat = rolling (50.0f, 0.0f);
            const float dDucked = pulseDepth (ducked), dFlat = pulseDepth (flat);
            check (tailHi > tailLo * 2.0f && dDucked > 8.0f && dDucked >= dFlat,
                   "Rolling Gate Rise + Ducked Bloom: the reverb tail after note-off grows " + juce::String (juce::Decibels::gainToDecibels (tailHi / juce::jmax (1.0e-6f, tailLo)), 1)
                   + " dB from 0 to 100 % macro; at 50 % the chops still dip " + juce::String (dDucked, 1) + " dB (" + juce::String (dFlat, 1) + " dB without ducking)");
        }

        // every reverb knob and the character menu jumping between extremes every 0.3 s mid-note: no clicks
        {
            struct Jump { const char* id; float a, b; };
            int clean = 0; juce::String worst;
            const Jump jumps[] { { "rev_size", 0.0f, 1.0f }, { "rev_decay", 0.2f, 20.0f }, { "rev_pre", 0.0f, 200.0f }, { "rev_width", 0.0f, 1.0f },
                                 { "rev_mod", 0.0f, 1.0f }, { "rev_duck", 0.0f, 1.0f }, { "rev_tone", -1.0f, 1.0f },
                                 { "preset", 0.0f, 0.0f }, { "rev_wet", 0.0f, 1.0f } };
            for (const Jump& j : jumps)
            {
                ElukkaNoiseProcessor proc;
                proc.seedRandom (9);
                proc.presets.load (proc.presets.indexOf ("Init", "Init"));
                setParam (proc, "rev_wet", 0.5f); setParam (proc, "rev_decay", 4.0f);
                RenderOpts o; o.sr = sr; o.seconds = 4.0; o.noteOn = 0.05; o.noteOff = 3.6;
                o.perBlock = [&] (ElukkaNoiseProcessor& p, double t)
                {
                    const int step = (int) (t / 0.3);
                    if (juce::String (j.id) == "preset") { p.revPresets.load (step % (int) p.revPresets.entries.size()); setParam (p, "rev_wet", 0.5f); }
                    else setParam (p, j.id, step % 2 == 0 ? j.a : j.b);
                };
                auto b = render (proc, o);
                const auto cl = detectClicks (b, 8.0f, (int) (0.05 * sr) + 64);
                if (cl.clicks == 0 && ! hasNaN (b)) ++clean;
                else worst += juce::String (" ") + j.id + " (" + juce::String (cl.worstSigmas, 1) + " sigma)";
            }
            check (clean == (int) std::size (jumps), "Size, Decay, Pre, Width, Mod, Rate, Duck, LP, HP, the preset (with its character) and Mix jumping between extremes every 0.3 s mid-note: no clicks ("
                                                     + juce::String (clean) + " / " + juce::String ((int) std::size (jumps)) + worst + ")");
        }

        // the characters live on inside the reverb presets; the v0.9.17 menu's choices map onto them
        check (FxPresetManager::presetForOldCharacter (FxPresetManager::Reverb, P::RHall) == "Clean Space" && FxPresetManager::presetForOldCharacter (FxPresetManager::Reverb, P::RDarkBloom) == "Dark Bloom"
               && FxPresetManager::presetForOldCharacter (FxPresetManager::Reverb, P::RPsy) == "Psy Atmosphere" && P::revCharacterNames.size() == P::numRevCharacters,
               "Seven reverb characters (internals sets) picked by the reverb presets; the old character menu maps onto them");

        // the tail outlives the note and the gate; extremes stay bounded
        {
            auto tail = [&] (bool gate)
            {
                return wetRender ([gate] (ElukkaNoiseProcessor& p)
                {
                    setParam (p, "rev_wet", 0.5f); setParam (p, "rev_decay", 6.0f); setParam (p, "env_release", 30.0f);
                    if (gate) { setParam (p, "gate_on", 1.0f); setParam (p, "gate_div", 5.0f); setParam (p, "gate_depth", 1.0f); }
                }, 3.0);
            };
            const float plain = rms (tail (false), (int) (1.5 * sr), (int) (1.8 * sr)), gated = rms (tail (true), (int) (1.5 * sr), (int) (1.8 * sr));
            check (plain > 0.003f && gated > 0.002f, "The tail rings on after note-off (" + juce::String (juce::Decibels::gainToDecibels (plain), 1) + " dB 0.5 s later) and under a closed gate ("
                                                      + juce::String (juce::Decibels::gainToDecibels (gated), 1) + " dB)");
            auto extreme = wetRender ([] (ElukkaNoiseProcessor& p)
            {
                p.revPresets.loadByName ("Psy Atmosphere");   // the Psy Space internals
                setParam (p, "rev_decay", 20.0f); setParam (p, "rev_size", 1.0f); setParam (p, "rev_mod", 1.0f);
                setParam (p, "rev_duck", 1.0f); setParam (p, "rev_tone", 0.0f);
            }, 8.0);
            check (! hasNaN (extreme) && peak (extreme) <= 1.0f, "20 s decay, 100 % size, full modulation (3 Hz), Psy Space, full ducking, 100 % wet: finite, peak " + juce::String (peak (extreme), 3));
        }
    }

    // 7k. v0.9.19: the delay rebuilt. Divisions and offset land where they should, a time change is exact at once
    // (crossfade, no glide), the Filter knob evolves the repeats, ping-pong alternates at full level, wide ping-pong
    // swaps sides, feedback 100 % sustains without running away, ducking on a gate, every knob and mode jumping
    // mid-note, a tempo change mid-note, the tail through note-off, extremes, the old parameters migrated, characters.
    std::cout << "\n== Delay ==\n";
    {
        // the delay class alone: an input generator over `seconds`, the echoes 100 % wet, returned as L/R
        struct Out { std::vector<float> L, R; };
        auto run = [&] (StereoDelay::Params p, double seconds, std::function<void (int, float&, float&)> gen, std::function<void (StereoDelay::Params&, double)> perPiece = {})
        {
            StereoDelay d;
            d.prepare (sr);
            p.wet = 1.0f;
            const int total = (int) (seconds * sr), block = 128;
            Out o { std::vector<float> ((size_t) total), std::vector<float> ((size_t) total) };
            for (int i = 0; i < total; ++i) gen (i, o.L[(size_t) i], o.R[(size_t) i]);
            for (int pos = 0; pos < total; pos += block)
            {
                if (perPiece) perPiece (p, pos / sr);
                d.process (o.L.data() + pos, o.R.data() + pos, juce::jmin (block, total - pos), p);
            }
            return o;
        };
        auto level = [&] (const std::vector<float>& x, double from, double to)
        {
            double s = 0.0; int n = 0;
            for (int i = (int) (from * sr); i < juce::jmin ((int) x.size(), (int) (to * sr)); ++i) { s += (double) x[(size_t) i] * x[(size_t) i]; ++n; }
            return n > 0 ? (float) std::sqrt (s / n) : 0.0f;
        };
        auto db = [] (float v) { return juce::Decibels::gainToDecibels (v, -120.0f); };
        // a 20 ms white burst at t = 0 (both channels, or the left only)
        auto burst = [&] (bool leftOnly, juce::uint32 seed = 3)
        {
            juce::Random rng ((juce::int64) seed);
            std::vector<float> noise ((size_t) (0.02 * sr));
            for (auto& v : noise) v = rng.nextFloat() * 0.6f - 0.3f;
            return [leftOnly, noise] (int i, float& l, float& r)
            {
                const float v = i < (int) noise.size() ? noise[(size_t) i] : 0.0f;
                l = v; r = leftOnly ? 0.0f : v;
            };
        };
        // where the first echo lands on a channel (the first 2 ms window above -40 dB after 50 ms)
        auto firstEcho = [&] (const std::vector<float>& x, double after = 0.05)
        {
            for (double t = after; t < x.size() / sr - 0.002; t += 0.001)
                if (level (x, t, t + 0.002) > 0.003f) return t;
            return -1.0;
        };

        // divisions and offset (times in seconds here; the engine test above covers the bpm maths)
        StereoDelay::Params base;
        base.feedback = 0.0f; base.mode = P::DmStereo;
        {
            auto p = base; p.timeSeconds = 1.846f;   // 1/1 at 130 bpm
            const auto o = run (p, 2.5, burst (false));
            auto q = base; q.timeSeconds = 0.2308f; q.offset = 0.3f;
            const auto o2 = run (q, 1.0, burst (false));
            const double t1 = firstEcho (o.L), tl = firstEcho (o2.L), tr = firstEcho (o2.R);
            check (P::dlyDivisionNames.size() == P::numDlyDivisions && std::abs (t1 - 1.846) < 0.003 && std::abs (tl - 0.2308) < 0.003 && std::abs (tr - 0.2308 * 1.3) < 0.003,
                   "12 divisions (1/1 .. 1/32); a 1.846 s echo (1/1 at 130 bpm) lands at " + juce::String (t1 * 1000.0, 0) + " ms; Offset +30 % puts the right echo at "
                   + juce::String (tr * 1000.0, 0) + " ms against " + juce::String (tl * 1000.0, 0) + " ms left (expected 300 / 231)");
        }
        // a time change is exact at once: time 100 -> 200 ms at 0.5 s, a burst at 0.6 s echoes at 0.8 s; no click in the fade
        {
            auto p = base; p.timeSeconds = 0.1f; p.feedback = 0.0f;
            juce::Random rng (9);
            const auto o = run (p, 1.2, [&] (int i, float& l, float& r)
            {
                const double t = i / sr;
                const float v = (t < 0.02 || (t >= 0.6 && t < 0.62)) ? rng.nextFloat() * 0.6f - 0.3f : 0.0f;
                l = r = v;
            }, [] (StereoDelay::Params& q, double t) { q.timeSeconds = t >= 0.5 ? 0.2f : 0.1f; });
            const double e = firstEcho (o.L, 0.7);
            juce::AudioBuffer<float> b (2, (int) o.L.size());
            b.copyFrom (0, 0, o.L.data(), (int) o.L.size()); b.copyFrom (1, 0, o.R.data(), (int) o.R.size());
            // the steady noise case: held noise, time jumping every 0.3 s between 50 ms and 1 s (the engine test below covers the rest)
            auto held = run (base, 3.0, [&] (int, float& l, float& r) { l = rng.nextFloat() * 0.6f - 0.3f; r = rng.nextFloat() * 0.6f - 0.3f; },
                             [] (StereoDelay::Params& q, double t) { q.timeSeconds = ((int) (t / 0.3)) % 2 == 0 ? 0.05f : 1.0f; q.feedback = 0.5f; });
            juce::AudioBuffer<float> hb (2, (int) held.L.size());
            hb.copyFrom (0, 0, held.L.data(), (int) held.L.size()); hb.copyFrom (1, 0, held.R.data(), (int) held.R.size());
            const auto cl = detectClicks (hb, 8.0f, (int) (0.05 * sr) + 64);
            check (std::abs (e - 0.8) < 0.003 && cl.clicks == 0, "A time change is exact at once (100 -> 200 ms: the next burst echoes " + juce::String ((e - 0.6) * 1000.0, 0)
                                                                 + " ms later) and the crossfade never clicks on held noise (time jumping 50 ms <-> 1 s, worst " + juce::String (cl.worstSigmas, 1) + " sigma)");
        }
        // Filter: a 50 ms burst, 200 ms, feedback 80 %: the 4th echo against the 1st
        {
            auto gen = [&] (int i, float& l, float& r) { static juce::Random rng (4); const float v = i < (int) (0.05 * sr) ? rng.nextFloat() * 0.6f - 0.3f : 0.0f; l = r = v; };
            auto echoBright = [&] (float filter, int echo)   // share of the energy above 2 kHz (four-pole high pass)
            {
                auto p = base; p.timeSeconds = 0.2f; p.feedback = 0.8f; p.filter = filter;
                const auto o = run (p, 1.2, gen);
                const float a = 1.0f - std::exp (-juce::MathConstants<float>::twoPi * 2000.0f / (float) sr);
                float h[4] {};
                double hi = 0.0, all = 0.0;
                const double t = 0.2 * echo;
                for (int i = (int) (t * sr); i < (int) ((t + 0.05) * sr); ++i)
                {
                    float x = o.L[(size_t) i];
                    const float v = x;
                    for (auto& z : h) { z += a * (x - z); x -= z; }
                    hi += (double) x * x; all += (double) v * v;
                }
                return (float) (hi / juce::jmax (1.0e-20, all));
            };
            auto echoLows = [&] (float filter, int echo)
            {
                auto p = base; p.timeSeconds = 0.2f; p.feedback = 0.8f; p.filter = filter;
                const auto o = run (p, 1.2, gen);
                const float a = 1.0f - std::exp (-juce::MathConstants<float>::twoPi * 300.0f / (float) sr);
                double lo = 0.0, all = 0.0; float l1 = 0.0f, l2 = 0.0f;
                const double t = 0.2 * echo;
                for (int i = (int) (t * sr); i < (int) ((t + 0.05) * sr); ++i) { const float v = o.L[(size_t) i]; l1 += a * (v - l1); l2 += a * (l1 - l2); lo += (double) l2 * l2; all += (double) v * v; }
                return (float) (lo / juce::jmax (1.0e-20, all));
            };
            const float darkFirst = echoBright (-1.0f, 1), darkFourth = echoBright (-1.0f, 4), neutralFirst = echoBright (0.0f, 1), neutralFourth = echoBright (0.0f, 4);
            const float thinFirst = echoLows (1.0f, 1), thinFourth = echoLows (1.0f, 4);
            check (darkFourth < 0.3f * darkFirst && std::abs (neutralFourth - neutralFirst) < 0.1f * neutralFirst && thinFourth < 0.3f * thinFirst,
                   "Filter: at -100 % the 4th echo keeps " + juce::String (juce::roundToInt (100.0f * darkFourth / juce::jmax (1.0e-6f, darkFirst))) + " % of the 1st's highs (centre: "
                   + juce::String (juce::roundToInt (100.0f * neutralFourth / juce::jmax (1.0e-6f, neutralFirst))) + " %); at +100 % it keeps "
                   + juce::String (juce::roundToInt (100.0f * thinFourth / juce::jmax (1.0e-6f, thinFirst))) + " % of its lows");
        }
        // ping-pong: echoes alternate at full level, feedback once per round trip; width 0 centres them; wide ping-pong swaps sides
        {
            auto p = base; p.timeSeconds = 0.2f; p.feedback = 0.8f; p.mode = P::DmPingPong; p.width = 1.0f;
            const auto o = run (p, 1.0, burst (false));
            const float l1 = level (o.L, 0.2, 0.22), r1 = level (o.R, 0.2, 0.22), l2 = level (o.L, 0.4, 0.42), r2 = level (o.R, 0.4, 0.42), l3 = level (o.L, 0.6, 0.62);
            auto q = p; q.width = 0.0f;
            const auto oc = run (q, 1.0, burst (false));
            float diff = 0.0f; for (int i = (int) (0.2 * sr); i < (int) (0.22 * sr); ++i) diff = juce::jmax (diff, std::abs (oc.L[(size_t) i] - oc.R[(size_t) i]));
            auto w = p; w.mode = P::DmWidePingPong;
            const auto ow = run (w, 1.0, burst (true));
            const float wl1 = level (ow.L, 0.2, 0.22), wr1 = level (ow.R, 0.2, 0.22), wl2 = level (ow.L, 0.4, 0.42), wr2 = level (ow.R, 0.4, 0.42);
            check (db (r1) < db (l1) - 25.0f && db (l2) < db (r2) - 25.0f && std::abs (db (r2) - db (l1)) < 1.5f && std::abs (db (l3) - db (l1) - 2.0f * db (0.8f)) < 1.5f
                   && diff < 1.0e-6f && db (wr1) < db (wl1) - 25.0f && db (wl2) < db (wr2) - 25.0f && std::abs (db (wr2) - db (wl1) - db (0.8f)) < 1.5f,
                   "Ping-pong: echo 1 left (right " + juce::String (db (r1) - db (l1), 0) + " dB), echo 2 right at the same level (" + juce::String (db (r2) - db (l1), 1)
                   + " dB), echo 3 left at feedback squared (" + juce::String (db (l3) - db (l1), 1) + " dB); width 0 centres them; wide ping-pong: a left-only input echoes left, then right at feedback ("
                   + juce::String (db (wr2) - db (wl1), 1) + " dB)");
        }
        // feedback 100 % sustains without running away; 60 % decays
        {
            juce::Random rng (11);
            auto gen = [&] (int i, float& l, float& r) { const float v = i < (int) (1.0 * sr) ? rng.nextFloat() * 0.6f - 0.3f : 0.0f; l = v; r = v; };
            auto p = base; p.timeSeconds = 0.3f; p.feedback = 1.0f;
            const auto o = run (p, 8.0, gen);
            float pk = 0.0f; for (float v : o.L) pk = juce::jmax (pk, std::abs (v));
            const float t2 = level (o.L, 2.0, 2.5), t7 = level (o.L, 7.0, 7.5);
            auto q = base; q.timeSeconds = 0.3f; q.feedback = 0.6f;
            const auto od = run (q, 4.0, gen);
            const float d1 = level (od.L, 1.2, 1.5), d3 = level (od.L, 3.0, 3.3);
            check (std::isfinite (pk) && pk <= 1.0f && t7 > t2 * 0.5f && t7 < t2 * 1.5f && d3 < d1 * 0.1f,
                   "Feedback 100 %: the loop sustains (" + juce::String (db (t2), 1) + " dB at 2 s, " + juce::String (db (t7), 1) + " dB at 7 s), peak " + juce::String (pk, 2)
                   + "; 60 %: " + juce::String (db (d1), 1) + " -> " + juce::String (db (d3), 1) + " dB over 1.8 s");
        }

        // in the engine: ducking on a 1/16 gate (wet only), the Rolling Gate Rise bloom, knob jumps, a tempo change, tails, extremes
        auto wetRender = [&] (std::function<void (ElukkaNoiseProcessor&)> setup, double seconds = 2.5, FakePlayHead* ph = nullptr,
                              std::function<void (ElukkaNoiseProcessor&, double)> perBlock = {})
        {
            ElukkaNoiseProcessor proc;
            proc.seedRandom (15);
            proc.presets.load (proc.presets.indexOf ("Init", "Init"));
            setParam (proc, "dly_wet", 1.0f); setParam (proc, "rev_on", 0.0f);
            setup (proc);
            RenderOpts o; o.sr = sr; o.seconds = seconds; o.noteOn = 0.0; o.noteOff = 1.0; o.playHead = ph; o.perBlock = perBlock;
            return render (proc, o);
        };
        {
            auto gated = [&] (float duck)
            {
                return wetRender ([duck] (ElukkaNoiseProcessor& p)
                {
                    setParam (p, "gate_on", 1.0f); setParam (p, "gate_div", 5.0f); setParam (p, "gate_depth", 1.0f); setParam (p, "gate_duty", 0.5f);
                    setParam (p, "gate_att", 1.0f); setParam (p, "gate_rel", 5.0f);
                    setParam (p, "dly_division", 4.0f); setParam (p, "dly_fb", 0.7f); setParam (p, "dly_mode", (float) P::DmStereo); setParam (p, "dly_duck", duck);
                });
            };
            auto off = gated (0.0f), on = gated (1.0f);
            float minRatio = 10.0f, maxRatio = 0.0f;
            for (int w = 0; w < 100; ++w)
            {
                const int a = (int) ((0.6 + 0.01 * w) * sr), b = a + (int) (0.01 * sr);
                const float ratio = rms (on, a, b) / juce::jmax (1.0e-6f, rms (off, a, b));
                minRatio = juce::jmin (minRatio, ratio); maxRatio = juce::jmax (maxRatio, ratio);
            }
            const auto cl = detectClicks (on, 8.0f, (int) (0.05 * sr) + 64);
            check (minRatio < 0.35f && maxRatio > 0.6f && cl.clicks == 0 && ! hasNaN (on),
                   "Ducking 100 % on a 1/16 gate: the echoes drop to " + juce::String (db (minRatio), 1) + " dB on a pulse and recover to " + juce::String (db (maxRatio), 1)
                   + " dB in the gap; no clicks (worst " + juce::String (cl.worstSigmas, 1) + " sigma)");
        }
        {
            auto rolling = [&] (float macro, float duck)
            {
                ElukkaNoiseProcessor proc;
                proc.seedRandom (8);
                proc.presets.load (proc.presets.indexOf ("Rolling Gate Rise", "Risers / Falls"));
                proc.dlyPresets.loadByName ("Ducked Gate Echo");   // v0.9.20: 1/8 ping-pong at 12 %, Duck 60 %, Mix +20 % late with the macro
                setParam (proc, "macro", macro); setParam (proc, "rev_on", 0.0f); setParam (proc, "env_release", 30.0f);
                if (duck >= 0.0f) setParam (proc, "dly_duck", duck);
                RenderOpts o; o.sr = sr; o.seconds = 2.6; o.noteOn = 0.05; o.noteOff = 1.5;
                return render (proc, o);
            };
            auto lo = rolling (0.0f, -1.0f), hi = rolling (100.0f, -1.0f);
            const float tailLo = rms (lo, (int) (1.7 * sr), (int) (2.2 * sr)), tailHi = rms (hi, (int) (1.7 * sr), (int) (2.2 * sr));
            auto pulseDepth = [&] (const juce::AudioBuffer<float>& b)
            {
                float lo2 = 1.0e9f, hi2 = 0.0f;
                for (int w = 0; w < 60; ++w) { const float v = rms (b, (int) ((0.8 + 0.01 * w) * sr), (int) ((0.81 + 0.01 * w) * sr)); lo2 = juce::jmin (lo2, v); hi2 = juce::jmax (hi2, v); }
                return db (hi2 / juce::jmax (1.0e-6f, lo2));
            };
            const float dDucked = pulseDepth (rolling (50.0f, -1.0f)), dFlat = pulseDepth (rolling (50.0f, 0.0f));
            check (tailHi > tailLo * 1.5f && dDucked > 8.0f && dDucked >= dFlat - 0.5f,
                   "Rolling Gate Rise + Ducked Gate Echo: the echoes after note-off grow " + juce::String (db (tailHi / juce::jmax (1.0e-6f, tailLo)), 1) + " dB from 0 to 100 % macro; at 50 % the chops still dip "
                   + juce::String (dDucked, 1) + " dB (" + juce::String (dFlat, 1) + " dB without ducking)");
        }
        {
            struct Jump { const char* id; float a, b; int cycle; };
            int clean = 0; juce::String worst;
            const Jump jumps[] { { "dly_time", 50.0f, 2000.0f, 0 }, { "dly_division", 0.0f, 11.0f, 0 }, { "dly_fb", 0.0f, 1.0f, 0 }, { "dly_filter", -1.0f, 1.0f, 0 },
                                 { "dly_width", 0.0f, 1.0f, 0 }, { "dly_offset", -0.5f, 0.5f, 0 }, { "dly_mod", 0.0f, 1.0f, 0 },
                                 { "dly_duck", 0.0f, 1.0f, 0 }, { "dly_mode", 0.0f, 0.0f, 4 }, { "preset", 0.0f, 0.0f, 0 }, { "dly_wet", 0.0f, 1.0f, 0 },
                                 { "dly_sync", 0.0f, 1.0f, 0 } };
            for (const Jump& j : jumps)
            {
                ElukkaNoiseProcessor proc;
                proc.seedRandom (9);
                proc.presets.load (proc.presets.indexOf ("Init", "Init"));
                setParam (proc, "dly_wet", 0.5f); setParam (proc, "dly_fb", 0.6f); setParam (proc, "rev_on", 0.0f);
                if (juce::String (j.id) == "dly_time") setParam (proc, "dly_sync", 0.0f);
                RenderOpts o; o.sr = sr; o.seconds = 4.0; o.noteOn = 0.05; o.noteOff = 3.6;
                o.perBlock = [&] (ElukkaNoiseProcessor& p, double t)
                {
                    const int step = (int) (t / 0.3);
                    if (juce::String (j.id) == "preset") { p.dlyPresets.load (step % (int) p.dlyPresets.entries.size()); setParam (p, "dly_wet", 0.5f); setParam (p, "dly_fb", 0.6f); }
                    else setParam (p, j.id, j.cycle > 0 ? (float) (step % j.cycle) : (step % 2 == 0 ? j.a : j.b));
                };
                auto b = render (proc, o);
                const auto cl = detectClicks (b, 8.0f, (int) (0.05 * sr) + 64);
                if (cl.clicks == 0 && ! hasNaN (b)) ++clean;
                else worst += juce::String (" ") + j.id + " (" + juce::String (cl.worstSigmas, 1) + " sigma)";
            }
            check (clean == (int) std::size (jumps), "Time, Division, Feedback, Filter, Width, Offset, Mod, Rate, Duck, Mode, Character, Mix and Sync jumping between extremes every 0.3 s mid-note: no clicks ("
                                                     + juce::String (clean) + " / " + juce::String ((int) std::size (jumps)) + worst + ")");
        }
        {
            // tempo 120 -> 140 bpm at 1 s with a synced 1/8: the 30 ms burst at 1.2 s echoes 214 ms later, no click
            FakePlayHead ph; ph.bpm = 120.0;
            ElukkaNoiseProcessor proc;
            proc.seedRandom (72);
            proc.presets.load (proc.presets.indexOf ("Init", "Init"));
            setParam (proc, "env_attack", 0.5f); setParam (proc, "env_release", 5.0f); setParam (proc, "rev_on", 0.0f);
            setParam (proc, "dly_division", 6.0f); setParam (proc, "dly_fb", 0.3f); setParam (proc, "dly_wet", 0.5f); setParam (proc, "dly_mode", (float) P::DmStereo);
            RenderOpts o; o.sr = sr; o.seconds = 2.0; o.noteOn = 1.2; o.noteOff = 1.23; o.playHead = &ph;
            o.perBlock = [&ph] (ElukkaNoiseProcessor&, double t) { ph.bpm = t >= 1.0 ? 140.0 : 120.0; };
            auto b = render (proc, o);
            double burstAt = -1.0, echoAt = -1.0;
            for (double t = 1.1; t < 2.0 - 0.002; t += 0.002)
            {
                const float v = rms (b, (int) (t * sr), (int) ((t + 0.002) * sr), 0);
                if (v > 0.01f && burstAt < 0.0) burstAt = t;
                else if (v > 0.01f && burstAt >= 0.0 && t > burstAt + 0.1 && echoAt < 0.0) echoAt = t;
            }
            const double gap = echoAt - burstAt;
            // and held noise through the change
            auto held = wetRender ([] (ElukkaNoiseProcessor& p) { setParam (p, "dly_division", 6.0f); setParam (p, "dly_fb", 0.6f); setParam (p, "dly_wet", 0.5f); }, 2.5, &ph,
                                   [&ph] (ElukkaNoiseProcessor&, double t) { ph.bpm = 120.0 + 40.0 * juce::jlimit (0.0, 1.0, (t - 0.8) / 0.6); });
            const auto cl = detectClicks (held, 8.0f, (int) (0.05 * sr) + 64);
            check (std::abs (gap - 60.0 / 140.0 * 0.5) < 0.006 && cl.clicks == 0, "Tempo change mid-note: after 120 -> 140 bpm the 1/8 echo comes " + juce::String (gap * 1000.0, 1)
                                                                                 + " ms later (expected 214.3); a tempo ramp under held noise never clicks (worst " + juce::String (cl.worstSigmas, 1) + " sigma)");
        }
        {
            auto tail = [&] (bool gate)
            {
                return wetRender ([gate] (ElukkaNoiseProcessor& p)
                {
                    setParam (p, "dly_wet", 0.5f); setParam (p, "dly_fb", 0.75f); setParam (p, "dly_division", 6.0f); setParam (p, "env_release", 30.0f);
                    if (gate) { setParam (p, "gate_on", 1.0f); setParam (p, "gate_div", 5.0f); setParam (p, "gate_depth", 1.0f); }
                }, 3.0);
            };
            const float plain = rms (tail (false), (int) (1.5 * sr), (int) (1.8 * sr)), gated = rms (tail (true), (int) (1.5 * sr), (int) (1.8 * sr));
            check (plain > 0.003f && gated > 0.002f, "The echoes ring on after note-off (" + juce::String (db (plain), 1) + " dB 0.5 s later) and under a closed gate (" + juce::String (db (gated), 1) + " dB)");
            auto extreme = wetRender ([] (ElukkaNoiseProcessor& p)
            {
                setParam (p, "dly_fb", 1.0f); setParam (p, "dly_mod", 1.0f); setParam (p, "dly_duck", 1.0f);
                setParam (p, "dly_mode", (float) P::DmWidePingPong); setParam (p, "dly_offset", 0.5f); setParam (p, "dly_width", 1.0f);
            }, 8.0, nullptr, [] (ElukkaNoiseProcessor& p, double t) { setParam (p, "dly_filter", std::fmod (t, 2.0) < 1.0 ? -1.0f : 1.0f); });
            check (! hasNaN (extreme) && peak (extreme) <= 1.0f, "Feedback 100 %, full modulation (3 Hz), wide ping-pong, offset +50 %, Filter flipping, full ducking, 100 % wet for 8 s: finite, peak " + juce::String (peak (extreme), 3));
        }
        // the old parameters are migrated: a state with dly_div 5 (1/8), dly_pp off and dly_lp 6 kHz, and a user preset tree alike
        {
            // a v0.9.19 state: stateVersion 1, no effect-preset children, the old 52-entry destination table in the
            // matrix slots and the character menus; its delay knobs in the old parameters
            ElukkaNoiseProcessor proc;
            auto t = proc.apvts.copyState();
            t.setProperty ("stateVersion", 1, nullptr);
            for (int i = t.getNumChildren() - 1; i >= 0; --i)
            {
                const auto id = t.getChild (i)["id"].toString();
                if (id == "dly_division" || id == "dly_mode" || id == "dly_filter" || ! t.getChild (i).hasType ("PARAM")) t.removeChild (i, nullptr);
            }
            auto set = [&t] (const char* id, float v)
            {
                for (auto c : t) if (c["id"].toString() == id) { c.setProperty ("value", v, nullptr); return; }
                juce::ValueTree c ("PARAM"); c.setProperty ("id", id, nullptr); c.setProperty ("value", v, nullptr); t.appendChild (c, nullptr);
            };
            set ("dly_div", 5.0f); set ("dly_pp", 0.0f); set ("dly_lp", 6000.0f); set ("dly_hp", 120.0f);
            set ("m1_dst", 19.0f); set ("m1_src", (float) P::SMacro);   // old Reverb Wet (18) + 1: the effects left the matrix -> empty
            set ("m2_dst", 21.0f); set ("m2_src", (float) P::SMacro);   // old Level (20) + 1
            set ("m3_dst", 42.0f); set ("m3_src", (float) P::SLfo1);    // old Comp Amount (41) + 1
            set ("rev_char", (float) P::RDarkBloom); set ("dly_char", 7.0f);   // Industrial
            set ("rev_decay", 7.7f);
            juce::MemoryBlock mb;
            if (auto xml = t.createXml()) juce::AudioProcessor::copyXmlToBinary (*xml, mb);
            proc.setStateInformation (mb.getData(), (int) mb.getSize());
            auto v = [&] (const char* id) { return proc.apvts.getRawParameterValue (id)->load(); };
            const bool state = (int) v ("dly_division") == 6 && (int) v ("dly_mode") == P::DmStereo && std::abs (v ("dly_filter") + 0.298f) < 0.01f;
            const bool dests = (int) v ("m1_dst") == 0 && (int) v ("m2_dst") == P::DLevel + 1 && (int) v ("m3_dst") == P::DCompAmt + 1;
            const bool chars = proc.revPresets.currentName == "Dark Bloom" && proc.revMapLive.character == P::RDarkBloom && proc.revPresets.map[P::MrDecay].on
                               && proc.dlyPresets.currentName == "Dark Industrial" && std::abs (v ("rev_decay") - 7.7f) < 0.01f;
            check (state, "Old delay parameters migrate: dly_div 5 / dly_pp off / LP 6 kHz -> 1/8, Stereo, Filter -30 %");
            check (dests, "A v0.9.19 state's matrix slots: Reverb Wet -> empty, Level and Comp Amount -> their new indices");
            check (chars, "A v0.9.19 state's character menus pick the effect presets (Dark Bloom, Industrial -> Dark Industrial) for the mapping and character; its knobs stay (decay 7.7 s)");
            // an instrument preset file with the old delay parameters leaves the delay alone; an effect preset file with them migrates
            juce::ValueTree pre ("ElukkaNoisePreset"), params ("PARAMS");
            for (auto [id, val] : { std::pair<const char*, float> { "dly_div", 7.0f }, { "dly_pp", 1.0f }, { "dly_lp", 20000.0f }, { "dly_hp", 300.0f } })
            { juce::ValueTree c ("PARAM"); c.setProperty ("id", id, nullptr); c.setProperty ("value", val, nullptr); params.appendChild (c, nullptr); }
            pre.appendChild (params, nullptr);
            proc.presets.applyTree (pre);
            const bool untouched = (int) v ("dly_division") == 6 && (int) v ("dly_mode") == P::DmStereo;
            juce::ValueTree fxPre ("ElukkaNoiseFxPreset");
            fxPre.appendChild (params.createCopy(), nullptr);
            proc.dlyPresets.applyTree (fxPre);
            const bool preset = (int) v ("dly_division") == 9 && (int) v ("dly_mode") == P::DmPingPong && std::abs (v ("dly_filter") - 0.281f) < 0.01f;
            check (untouched && preset, "An instrument preset with old delay parameters leaves the delay alone; a delay preset with 1/16, ping-pong, HP 300 Hz -> 1/16, Ping-Pong, Filter +28 %");
        }
        check (FxPresetManager::presetForOldCharacter (FxPresetManager::Delay, 0) == "Clean Rhythmic" && FxPresetManager::presetForOldCharacter (FxPresetManager::Delay, 7) == "Dark Industrial"
               && P::dlyCharacterNames.size() == 8, "The v0.9.19 delay character menu maps onto the delay presets");
    }

    // 8. State: save, change, restore.
    // 9. v0.9.20: effect presets. The delay and the reverb each have their own presets carrying their macro mapping;
    // the instrument preset leaves them alone and the matrix no longer reaches them.
    std::cout << "\n== Effect presets ==\n";
    {
        using FM = FxPresetManager;
        auto snapshot = [] (ElukkaNoiseProcessor& p, const juce::String& skipPrefix)
        {
            std::vector<std::pair<juce::String, float>> v;
            for (auto* prm : p.getParameters())
                if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (prm))
                    if (! rp->paramID.startsWith (skipPrefix)) v.push_back ({ rp->paramID, rp->getValue() });
            return v;
        };
        auto same = [] (const P::FxMapEntry& a, const P::FxMapEntry& b)
        {
            return a.on == b.on && a.mode == b.mode && std::abs (a.a - b.a) < 1.0e-6f && std::abs (a.b - b.b) < 1.0e-6f && std::abs (a.curve - b.curve) < 1.0e-6f;
        };
        auto rawv = [] (ElukkaNoiseProcessor& p, const char* id) { return p.apvts.getRawParameterValue (id)->load(); };

        // factory lists: the brief's presets exist, every value inside its knob's range, every mapping valid, names unique
        {
            ElukkaNoiseProcessor proc;
            bool ok = true; juce::String bad;
            for (auto kind : { FM::Delay, FM::Reverb })
            {
                juce::StringArray names;
                for (auto& fp : FM::factory (kind))
                {
                    if (names.contains (fp.name)) { ok = false; bad += " dup:" + fp.name; }
                    names.add (fp.name);
                    for (auto& [id, val] : fp.values)
                    {
                        auto* prm = proc.apvts.getParameter (id);
                        const auto rg = prm != nullptr ? prm->getNormalisableRange() : juce::NormalisableRange<float>();
                        if (prm == nullptr || val < rg.start - 1.0e-4f || val > rg.end + 1.0e-4f) { ok = false; bad += " " + fp.name + "/" + id; }
                    }
                    const int n = kind == FM::Delay ? P::numDlyMapDests : P::numRevMapDests;
                    for (int d = 0; d < n; ++d)
                    {
                        const auto& e = fp.map[d];
                        if (! e.on) continue;
                        const bool valid = e.mode == P::MapRange ? (fp.mapInUnits[d] || (e.a >= 0.0f && e.a <= 1.0f && e.b >= 0.0f && e.b <= 1.0f)) : std::abs (e.b) <= 1.0f;
                        if (! valid || std::abs (e.curve) > 1.0f) { ok = false; bad += " map:" + fp.name; }
                    }
                }
                const std::vector<const char*> want = kind == FM::Delay ? std::vector<const char*> { "Clean Rhythmic", "Wide Ping-Pong", "Dark Industrial", "Psy Feedback", "Atmospheric" }
                                                                        : std::vector<const char*> { "Clean Space", "Endless Space", "Dark Bloom", "Bright Hall", "Psy Atmosphere", "Frozen Atmosphere" };
                for (auto* w : want) if (! names.contains (w)) { ok = false; bad += juce::String (" missing:") + w; }
            }
            check (ok, juce::String ((int) FM::factory (FM::Delay).size()) + " delay and " + juce::String ((int) FM::factory (FM::Reverb).size())
                       + " reverb factory presets: the brief's names present, every value in range, every mapping valid" + bad);
            check (proc.dlyPresets.currentName == "Clean Rhythmic" && proc.revPresets.currentName == "Clean Space" && proc.revMapLive.character == P::RHall
                   && proc.dlyPresets.map[P::MdMix].on && proc.revPresets.map[P::MrMix].on,
                   "A new instance starts on Clean Rhythmic and Clean Space, both bringing their effect in with the macro");
        }
        // 1 + 2 + 7: an instrument preset, then only the delay preset (or only the reverb preset) changes; an instrument preset keeps both
        {
            ElukkaNoiseProcessor proc;
            proc.presets.load (proc.presets.indexOf ("Rolling Gate Rise", "Risers / Falls"));
            setParam (proc, "macro", 37.0f);
            const auto before = snapshot (proc, "dly_");
            P::FxMapEntry revMap[P::maxFxMapDests];
            std::copy (proc.revPresets.map, proc.revPresets.map + P::maxFxMapDests, revMap);
            proc.dlyPresets.loadByName ("Dark Industrial");
            bool unchanged = before == snapshot (proc, "dly_");
            for (int d = 0; d < P::numRevMapDests; ++d) unchanged = unchanged && same (revMap[d], proc.revPresets.map[d]);
            check (unchanged && std::abs (rawv (proc, "dly_fb") - 0.55f) < 1.0e-4f && proc.dlyPresets.map[P::MdFilter].on,
                   "Changing the delay preset changes only the delay (knobs and mapping): instrument, macro, reverb and its mapping untouched");
            const auto before2 = snapshot (proc, "rev_");
            P::FxMapEntry dlyMap[P::maxFxMapDests];
            std::copy (proc.dlyPresets.map, proc.dlyPresets.map + P::maxFxMapDests, dlyMap);
            proc.revPresets.loadByName ("Dark Bloom");
            bool unchanged2 = before2 == snapshot (proc, "rev_");
            for (int d = 0; d < P::numDlyMapDests; ++d) unchanged2 = unchanged2 && same (dlyMap[d], proc.dlyPresets.map[d]);
            check (unchanged2 && std::abs (rawv (proc, "rev_tone") + 0.9f) < 1.0e-4f && proc.revMapLive.character == P::RDarkBloom,
                   "Changing the reverb preset changes only the reverb (knobs, character and mapping): instrument, macro, delay and its mapping untouched");
            proc.presets.load (proc.presets.indexOf ("Psy Filter", "FX"));
            check (proc.dlyPresets.currentName == "Wide Ping-Pong" && proc.revPresets.currentName == "Psy Atmosphere" && std::abs (rawv (proc, "dly_fb") - 0.5f) < 1.0e-4f
                   && std::abs (rawv (proc, "rev_tone") + 0.45f) < 1.0e-4f && std::abs (rawv (proc, "macro") - 37.0f) < 0.01f && proc.revMapLive.character == P::RPsy,
                   "Loading an instrument preset (v0.9.21) loads the delay and reverb presets it names, with their knobs, mappings and character, and keeps the macro position");
        }
        // 3 + 4 + 9 + 10: both mappings at once (the macro set as a host would), no drift over repeated sweeps, Range mode
        {
            ElukkaNoiseProcessor proc;
            proc.presets.load (proc.presets.indexOf ("Init", "Init"));
            proc.dlyPresets.loadByName ("Wide Ping-Pong");
            proc.revPresets.loadByName ("Endless Space");
            auto all = [&] { return snapshot (proc, "zzz"); };
            const auto stored = all();
            auto run = [&] (std::function<float (double)> macroAt, double seconds)
            {
                RenderOpts o; o.sr = sr; o.seconds = seconds; o.noteOn = 0.05; o.noteOff = seconds; o.macroAt = macroAt;
                return render (proc, o);
            };
            run ([] (double) { return 0.75f; }, 0.5);
            const float fb0 = proc.apvts.getParameter ("dly_fb")->getValue(), dec0 = proc.apvts.getParameter ("rev_decay")->getValue();
            const float fbEff = proc.engine.uiDlyMap[P::MdFb].load(), decEff = proc.engine.uiRevMap[P::MrDecay].load();
            const float widEff = proc.engine.uiDlyMap[P::MdWidth].load(), sizeEff = proc.engine.uiRevMap[P::MrSize].load();
            check (std::abs (fbEff - (fb0 + 0.3f * 0.75f)) < 0.01f && std::abs (decEff - (dec0 + 0.4f * 0.75f)) < 0.01f
                   && widEff > proc.apvts.getParameter ("dly_width")->getValue() + 0.2f && sizeEff > proc.apvts.getParameter ("rev_size")->getValue() + 0.15f,
                   "Macro 75 %: the delay's feedback and width and the reverb's decay and size follow their own presets' mappings at once (feedback +"
                   + juce::String (fbEff - fb0, 3) + ", decay +" + juce::String (decEff - dec0, 3) + " normalised)");
            run ([] (double) { return 0.0f; }, 0.5);
            const float decZero = proc.engine.uiRevMap[P::MrDecay].load(), mixZero = proc.engine.uiDlyMap[P::MdMix].load();
            run ([] (double t) { const double c = std::fmod (t, 2.0); return (float) (c < 1.0 ? c : 2.0 - c); }, 6.0);   // 0 -> 100 -> 0 % three times
            run ([] (double) { return 0.0f; }, 0.5);
            check (stored == all() && std::abs (proc.engine.uiRevMap[P::MrDecay].load() - decZero) < 1.0e-4f && std::abs (proc.engine.uiDlyMap[P::MdMix].load() - mixZero) < 1.0e-4f,
                   "Three macro sweeps 0 -> 100 -> 0 %: no stored parameter changed and the effective values return exactly to where they started (no drift)");
            proc.revPresets.loadByName ("Frozen Atmosphere");
            setParam (proc, "rev_decay", 1.0f);
            auto decayEff = [&] { return proc.apvts.getParameter ("rev_decay")->convertFrom0to1 (proc.engine.uiRevMap[P::MrDecay].load()); };
            run ([] (double) { return 0.0f; }, 0.5);
            const float d0 = decayEff();
            run ([] (double) { return 1.0f; }, 0.5);
            const float d1 = decayEff();
            auto off = proc.revPresets.map[P::MrDecay];
            off.on = false;
            proc.revPresets.setMap (P::MrDecay, off);
            run ([] (double) { return 1.0f; }, 0.5);
            const float dOff = decayEff();
            check (std::abs (d0 - 4.0f) < 0.1f && std::abs (d1 - 20.0f) < 0.3f && std::abs (dOff - 1.0f) < 0.02f,
                   "Frozen Atmosphere's Range mapping: Decay 4 s at 0 %, 20 s at 100 % whatever the knob says (" + juce::String (d0, 2) + " / " + juce::String (d1, 1)
                   + " s); mapping off = the knob (" + juce::String (dOff, 2) + " s)");
        }
        // 5 + 7 + 11: switching effect presets at macro 0 / 50 / 100 % mid-note and while the tails ring: no clicks, the tails survive
        {
            int clean = 0; juce::String worst;
            for (float macro : { 0.0f, 50.0f, 100.0f })
            {
                ElukkaNoiseProcessor proc;
                proc.seedRandom (21);
                proc.presets.load (proc.presets.indexOf ("Clean Noise Rise", "Risers / Falls"));
                setParam (proc, "macro", macro);
                RenderOpts o; o.sr = sr; o.seconds = 4.5; o.noteOn = 0.05; o.noteOff = 2.5;
                int last = -1;
                std::vector<int> dlyIdx, revIdx;   // every preset but Off (which would leave no tail to measure)
                for (int i = 0; i < (int) proc.dlyPresets.entries.size(); ++i) if (proc.dlyPresets.entries[(size_t) i].name != "Off") dlyIdx.push_back (i);
                for (int i = 0; i < (int) proc.revPresets.entries.size(); ++i) if (proc.revPresets.entries[(size_t) i].name != "Off") revIdx.push_back (i);
                o.perBlock = [&] (ElukkaNoiseProcessor& p, double t)
                {
                    const int step = (int) (t / 0.4);
                    if (step == last) return;
                    last = step;
                    p.dlyPresets.load (dlyIdx[(size_t) (step % (int) dlyIdx.size())]);
                    p.revPresets.load (revIdx[(size_t) ((step * 2) % (int) revIdx.size())]);
                    setParam (p, "dly_wet", 0.4f); setParam (p, "rev_wet", 0.4f);
                };
                auto b = render (proc, o);
                const auto cl = detectClicks (b, 8.0f, (int) (0.05 * sr) + 64);
                const float tail = rms (b, (int) (3.0 * sr), (int) (3.6 * sr));
                if (cl.clicks == 0 && ! hasNaN (b) && peak (b) <= 1.0f && tail > 1.0e-3f) ++clean;
                else worst += " at " + juce::String (macro, 0) + " % (" + juce::String (cl.worstSigmas, 1) + " sigma, peak " + juce::String (peak (b), 2) + ", tail " + juce::String (juce::Decibels::gainToDecibels (tail), 1) + " dB)";
            }
            check (clean == 3, "Delay and reverb presets switched in every 0.4 s at macro 0, 50 and 100 %, mid-note and through the tails: no clicks, the tails keep ringing" + worst);
        }
        // 6: instrument presets switched while a note is held bring their effect presets along (clicks: 'Preset switching mid-note')
        {
            ElukkaNoiseProcessor proc;
            proc.seedRandom (22);
            proc.presets.load (0);
            RenderOpts o; o.sr = sr; o.seconds = 1.5; o.noteOn = 0.05; o.noteOff = 1.4;
            int last = -1, followed = 0, loads = 0;
            o.perBlock = [&] (ElukkaNoiseProcessor& p, double t)
            {
                const int idx = juce::jlimit (0, (int) factory.size() - 1, (int) (t / 0.2));
                if (idx == last) return;
                p.presets.load (idx); last = idx; ++loads;
                if (p.dlyPresets.currentName == factory[(size_t) idx].delayPreset && p.revPresets.currentName == factory[(size_t) idx].reverbPreset) ++followed;
            };
            auto b = render (proc, o);
            check (! hasNaN (b) && loads >= 7 && followed == loads,
                   "Instrument presets switched while a note is held: each brings its own delay and reverb presets (" + juce::String (followed) + " / " + juce::String (loads) + ")");
        }
        // 8: user effect presets save and load back with their mapping (and the reverb's character); one file per effect
        {
            auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("ElukkaNoiseTestFxPresets");
            tmp.deleteRecursively();
            PresetManager::overrideDir = tmp;
            ElukkaNoiseProcessor proc;
            auto& dm = proc.dlyPresets;
            auto& rm = proc.revPresets;
            dm.refresh(); rm.refresh();
            const int nd = (int) dm.entries.size(), nr = (int) rm.entries.size();
            dm.loadByName ("Atmospheric");
            setParam (proc, "dly_fb", 0.33f);
            dm.setMap (P::MdFb, { true, P::MapRange, 0.1f, 0.9f, -0.4f });
            rm.loadByName ("Psy Atmosphere");
            check (dm.save ("My Echo") && (int) dm.entries.size() == nd + 1 && dm.currentName == "My Echo" && ! dm.entries[(size_t) dm.currentIndex].isFactory
                   && (int) rm.entries.size() == nr && rm.currentName == "Psy Atmosphere", "Save a user delay preset: listed and current; the reverb presets untouched");
            dm.load (0);
            const bool changed = std::abs (rawv (proc, "dly_fb") - 0.33f) > 0.01f && dm.map[P::MdFb].mode == P::MapAdd;
            check (changed && dm.loadByName ("My Echo") && std::abs (rawv (proc, "dly_fb") - 0.33f) < 1.0e-4f
                   && same (dm.map[P::MdFb], { true, P::MapRange, 0.1f, 0.9f, -0.4f }) && same (dm.live.get (P::MdFb), dm.map[P::MdFb]),
                   "Loading it back restores its knobs and its edited Range mapping, in the engine too");
            auto edited = dm.map[P::MdMix]; edited.b = 0.77f; dm.setMap (P::MdMix, edited);
            dm.restoreMap();
            check (std::abs (dm.map[P::MdMix].b - 0.3f) < 1.0e-6f && same (dm.map[P::MdFb], { true, P::MapRange, 0.1f, 0.9f, -0.4f }), "Restore brings the preset's saved mapping back");
            auto xml = juce::XmlDocument::parse (dm.entries[(size_t) dm.currentIndex].file);
            check (xml != nullptr && xml->getIntAttribute ("version") == 1 && xml->getStringAttribute ("kind") == "Delay" && xml->getChildByName ("Map") != nullptr,
                   "The file carries a format version, its kind and the mapping (" + dm.entries[(size_t) dm.currentIndex].file.getFileName() + ")");
            setParam (proc, "rev_pre", 77.0f);
            check (rm.save ("My Space") && (int) rm.entries.size() == nr + 1, "Save a user reverb preset");
            rm.load (0);
            check (rm.loadByName ("My Space") && std::abs (rawv (proc, "rev_pre") - 77.0f) < 0.01f && proc.revMapLive.character == P::RPsy && rm.map[P::MrMod].on,
                   "Loading it back restores its knobs, its Psy Space character and its mapping");
            check (! dm.deletePreset (0) && ! rm.deletePreset (0), "Factory effect presets cannot be deleted");
            check (dm.deletePreset (dm.indexOf ("My Echo")) && (int) dm.entries.size() == nd && (int) rm.entries.size() == nr + 1, "Deleting a delay preset leaves the reverb presets");
            check (rm.deletePreset (rm.indexOf ("My Space")) && (int) rm.entries.size() == nr, "Delete a user reverb preset");
            PresetManager::overrideDir = juce::File();
            tmp.deleteRecursively();
        }
    }

    std::cout << "\n== Instrument presets: effect references, factory protection, the complete sound ==\n";
    {
        using FMx = FxPresetManager;
        const auto& fac = PresetManager::factory();
        auto hasFx = [] (FMx::Kind k, const juce::String& n) { for (auto& f : FMx::factory (k)) if (f.name == n) return true; return false; };
        int rise = 0, fxc = 0;
        juce::String bad;
        for (auto& p : fac)
        {
            if (p.category == "Risers / Falls") ++rise; else if (p.category == "FX") ++fxc;
            if (! hasFx (FMx::Delay, p.delayPreset) || ! hasFx (FMx::Reverb, p.reverbPreset)) bad += " " + p.name;
        }
        check (rise == 16 && fxc == 16 && bad.isEmpty(), "16 Risers / Falls and 16 FX presets, every one naming a factory delay and a factory reverb preset" + bad);

        auto rv = [] (ElukkaNoiseProcessor& p, const char* id) { return p.apvts.getRawParameterValue (id)->load(); };
        // the parameters of one layer: "" = the instrument (everything but the macro and the effects)
        auto snap = [] (ElukkaNoiseProcessor& p, const juce::String& prefix)
        {
            std::vector<float> v;
            for (auto* prm : p.getParameters())
                if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (prm))
                {
                    const bool fxParam = rp->paramID.startsWith ("dly_") || rp->paramID.startsWith ("rev_");
                    if (prefix.isEmpty() ? (! fxParam && rp->paramID != "macro") : rp->paramID.startsWith (prefix)) v.push_back (rp->getValue());
                }
            return v;
        };
        {
            ElukkaNoiseProcessor proc;
            setParam (proc, "macro", 37.0f);
            proc.presets.load (proc.presets.indexOf ("Spinning Rise", "Risers / Falls"));
            check (proc.dlyPresets.currentName == "Clean Rhythmic" && proc.revPresets.currentName == "Endless Space" && proc.revMapLive.character == P::REndless
                   && std::abs (rv (proc, "rev_decay") - 6.0f) < 0.01f && std::abs (rv (proc, "macro") - 37.0f) < 0.01f,
                   "Spinning Rise loads with Clean Rhythmic and Endless Space (knobs, mapping, character), the macro position kept");
            const auto inst = snap (proc, ""), rev = snap (proc, "rev_");
            proc.dlyPresets.loadByName ("Dark Industrial");
            check (inst == snap (proc, "") && rev == snap (proc, "rev_") && proc.revPresets.currentName == "Endless Space" && proc.dlyPresets.currentName == "Dark Industrial"
                   && std::abs (rv (proc, "dly_fb") - 0.55f) < 1.0e-4f,
                   "Changing only the delay preset afterwards: the instrument and the reverb stay, the delay is now Dark Industrial");
            const auto dly = snap (proc, "dly_");
            proc.revPresets.loadByName ("Tight Space");
            check (inst == snap (proc, "") && dly == snap (proc, "dly_") && proc.dlyPresets.currentName == "Dark Industrial" && proc.revPresets.currentName == "Tight Space",
                   "Changing only the reverb preset afterwards: the instrument and the delay stay");
            proc.presets.load (proc.presets.indexOf ("Psy Chaos", "FX"));
            check (proc.dlyPresets.currentName == "Psy Feedback" && proc.revPresets.currentName == "Psy Atmosphere" && std::abs (rv (proc, "macro") - 37.0f) < 0.01f,
                   "Loading another instrument preset selects its own effect presets (Psy Chaos: Psy Feedback, Psy Atmosphere)");

            // a saved instrument preset stores the effect preset names, not their settings
            auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("ElukkaNoiseRefTest");
            tmp.deleteRecursively();
            PresetManager::overrideDir = tmp;
            proc.presets.refresh();
            proc.dlyPresets.loadByName ("Atmospheric");
            proc.revPresets.loadByName ("Tight Space");
            setParam (proc, "flt_cutoff", 1234.0f);
            const bool saved = proc.presets.save ("My Linked", "FX");
            int fxParams = 0;
            juce::ValueTree saveTree;
            if (saved)
                if (auto xml = juce::XmlDocument::parse (proc.presets.entries[(size_t) proc.presets.currentIndex].file))
                {
                    saveTree = juce::ValueTree::fromXml (*xml);
                    for (const auto& c : saveTree.getChildWithName ("PARAMS"))
                        if (c["id"].toString().startsWith ("dly_") || c["id"].toString().startsWith ("rev_")) ++fxParams;
                }
            ElukkaNoiseProcessor other;
            PresetManager::overrideDir = tmp;
            other.presets.refresh();
            other.presets.load (other.presets.indexOf ("Clean Noise Rise", "Risers / Falls"));
            const bool loaded = other.presets.loadByName ("My Linked", "FX");
            check (saved && (int) saveTree.getProperty ("version", 0) == 4 && saveTree["delayPreset"].toString() == "Atmospheric" && saveTree["reverbPreset"].toString() == "Tight Space"
                   && fxParams == 0 && loaded && other.dlyPresets.currentName == "Atmospheric" && other.revPresets.currentName == "Tight Space"
                   && std::abs (rv (other, "flt_cutoff") - 1234.0f) < 0.5f,
                   "A saved instrument preset carries the effect preset names (format version 4) and no effect parameters; loading it selects them");

            // an old preset (no names) leaves the effects alone; a name that no longer exists falls back to the defaults
            auto old = other.presets.toTree ("Old", "FX");
            old.removeProperty ("delayPreset", nullptr); old.removeProperty ("reverbPreset", nullptr); old.setProperty ("version", 2, nullptr);
            other.dlyPresets.loadByName ("Dark Feedback"); other.revPresets.loadByName ("Bright Hall");
            other.presets.applyTree (old);
            const bool untouched = other.dlyPresets.currentName == "Dark Feedback" && other.revPresets.currentName == "Bright Hall";
            auto gone = other.presets.toTree ("Gone", "FX");
            gone.setProperty ("delayPreset", "No Such Delay", nullptr); gone.setProperty ("reverbPreset", "No Such Reverb", nullptr);
            other.presets.applyTree (gone);
            check (untouched && other.dlyPresets.currentName == "Clean Rhythmic" && other.revPresets.currentName == "Clean Space",
                   "A preset saved before v0.9.21 leaves the effects as they are; a preset naming a missing effect preset gets Clean Rhythmic / Clean Space");

            // factory presets cannot be deleted or overwritten; a user preset cannot take a factory name (v0.9.25: it would be shadowed)
            const int trance = other.presets.indexOf ("Trance Pulse", "FX", true);
            const bool noDelete = ! other.presets.deletePreset (trance) && ! other.dlyPresets.deletePreset (other.dlyPresets.indexOf ("Clean Rhythmic", true));
            const bool refused = ! other.presets.save ("Trance Pulse", "FX");
            int count = 0;
            for (auto& e : other.presets.entries) if (e.name == "Trance Pulse") ++count;
            check (noDelete && refused && count == 1, "Factory instrument and effect presets cannot be deleted; saving under a factory name is refused");

            // isModified: the save dialog's note
            other.dlyPresets.loadByName ("Clean Rhythmic");
            const bool clean = ! other.dlyPresets.isModified();
            setParam (other, "dly_fb", 0.31f);
            const bool dirty = other.dlyPresets.isModified();
            other.dlyPresets.loadByName ("Clean Rhythmic");
            check (clean && dirty && ! other.dlyPresets.isModified(), "An edited delay knob marks the delay preset modified until a preset is loaded again");

            tmp.deleteRecursively();
            PresetManager::overrideDir = juce::File();
        }

        // every preset as a complete sound (effects on): held at 0 / 25 / 50 / 75 / 100 %, no clipping, no level jump
        // over 8 dB between neighbouring positions, nothing below -36 dB; then a 0 -> 100 -> 0 % sweep through the
        // note and the tails: no clicks, and the tails die down (no runaway feedback; a fall's 9 s wash is still decaying 4 s on)
        std::cout << std::setw (26) << std::left << "preset" << std::right << "   rms at 0/25/50/75/100 %            peak   sweep clicks  tail 1 s / 4 s after note-off\n";
        for (auto& p : fac)
        {
            if (p.category == "Init") continue;
            float level[5] {}, pk = 0.0f;
            for (int i = 0; i < 5; ++i)
            {
                ElukkaNoiseProcessor proc;
                proc.seedRandom (5);
                proc.presets.load (proc.presets.indexOf (p.name, p.category));
                setParam (proc, "macro", 25.0f * (float) i);
                RenderOpts o; o.sr = sr; o.seconds = 1.6; o.noteOn = 0.0; o.noteOff = 1.6;
                auto b = render (proc, o);
                level[i] = juce::Decibels::gainToDecibels (rms (b, (int) (0.6 * sr), (int) (1.6 * sr)), -120.0f);
                pk = juce::jmax (pk, peak (b));
            }
            ElukkaNoiseProcessor proc;
            proc.seedRandom (5);
            proc.presets.load (proc.presets.indexOf (p.name, p.category));
            RenderOpts o; o.sr = sr; o.seconds = 7.0; o.noteOn = 0.05; o.noteOff = 3.0;
            o.macroAt = [] (double t) { return (float) (t < 1.5 ? t / 1.5 : t < 2.0 ? 1.0 : juce::jmax (0.0, 1.0 - (t - 2.0) / 1.0)); };
            auto b = render (proc, o);
            const auto cl = detectClicks (b, 8.0f, (int) (0.05 * sr) + 64);
            const float t1 = juce::Decibels::gainToDecibels (rms (b, (int) (3.8 * sr), (int) (4.2 * sr)), -120.0f);
            const float t4 = juce::Decibels::gainToDecibels (rms (b, (int) (6.6 * sr), (int) (7.0 * sr)), -120.0f);
            float maxStep = 0.0f, minLevel = 0.0f;
            for (int i = 0; i < 5; ++i) { minLevel = juce::jmin (minLevel, level[i]); if (i > 0) maxStep = juce::jmax (maxStep, std::abs (level[i] - level[i - 1])); }
            const bool ok = ! hasNaN (b) && pk < 0.95f && peak (b) < 1.0f && maxStep <= 8.0f && minLevel > -36.0f && cl.clicks == 0 && t4 < -40.0f && (t1 < -60.0f || t4 < t1 - 6.0f);
            std::cout << (ok ? "ok    " : "FAIL  ") << std::setw (26) << std::left << p.name << std::right << std::fixed << std::setprecision (1);
            for (int i = 0; i < 5; ++i) std::cout << std::setw (7) << level[i];
            std::cout << "  " << std::setprecision (2) << pk << "   " << cl.clicks << " (worst " << std::setprecision (1) << cl.worstSigmas << ")   " << t1 << " / " << t4 << " dB\n";
            if (! ok) ++failures;
        }
    }

    // Stereo pan (v0.9.24): each channel is panned on its own, so the image moves as a whole and stays wide; the level
    // stays put whatever the Stereo setting, and mono noise pans exactly as the old balance pan did.
    // Release audit (v0.9.25): preset names are stored as typed only without the file-name characters JUCE strips,
    // a user preset cannot take a factory name (it would be shadowed), a corrupt effect preset file leaves the current
    // preset alone, old reverb Low Pass / High Pass macro mappings become Tone mappings, and the editor closes the
    // dialogs and call-outs it opened when it goes.
    std::cout << "\n== Release audit fixes (v0.9.25) ==\n";
    {
        auto rawv = [] (ElukkaNoiseProcessor& p, const char* id) { return p.apvts.getRawParameterValue (id)->load(); };
        auto same = [] (const P::FxMapEntry& a, const P::FxMapEntry& b)
        {
            return a.on == b.on && a.mode == b.mode && std::abs (a.a - b.a) < 1.0e-6f && std::abs (a.b - b.b) < 1.0e-6f && std::abs (a.curve - b.curve) < 1.0e-6f;
        };
        auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("ElukkaNoiseTestAudit");
        tmp.deleteRecursively();
        PresetManager::overrideDir = tmp;
        {
            ElukkaNoiseProcessor proc;
            auto& pm = proc.presets;
            pm.refresh();
            const int n = (int) pm.entries.size();
            check (! pm.save ("Clean Noise Rise", "Risers / Falls") && ! pm.save ("  ", "FX") && (int) pm.entries.size() == n,
                   "Saving an instrument preset under a factory name, or without a name, is refused");
            check (pm.save ("Rise, Fall: A/B", "FX") && pm.currentName == "Rise Fall AB" && pm.currentIndex >= 0 && ! pm.entries[(size_t) pm.currentIndex].isFactory
                   && pm.entries[(size_t) pm.currentIndex].file.getFileNameWithoutExtension() == "Rise Fall AB",
                   "\"Rise, Fall: A/B\" is saved as \"Rise Fall AB\": the name, the menu entry and the file agree (" + pm.currentName + ")");
            check (! pm.rename (pm.currentIndex, "Whiteout") && pm.rename (pm.currentIndex, "..My: Rise") && pm.currentName == "My Rise",
                   "Rename refuses a factory name and stores the legal form (" + pm.currentName + ")");
            auto& rm = proc.revPresets;
            rm.refresh();
            const int nr = (int) rm.entries.size();
            check (! rm.save ("Clean Space") && (int) rm.entries.size() == nr, "Saving a reverb preset under a factory name is refused");
            setParam (proc, "rev_pre", 55.0f);
            check (rm.save ("Big: Space") && rm.currentName == "Big Space" && rm.currentIndex >= 0 && ! rm.isModified(),
                   "\"Big: Space\" is saved as \"Big Space\" and is the current, unmodified preset");
            // a corrupt file: the current preset stays
            auto bad = rm.userDir().getChildFile (juce::String ("Broken") + FxPresetManager::fileExtension);
            bad.replaceWithText ("<not xml");
            rm.refresh();
            const int before = rm.currentIndex;
            rm.load (rm.indexOf ("Broken"));
            check (rm.indexOf ("Broken") >= 0 && rm.currentIndex == before && rm.currentName == "Big Space" && std::abs (rawv (proc, "rev_pre") - 55.0f) < 0.01f,
                   "Loading a corrupt reverb preset file leaves the current preset, its knobs and its name alone");
        }
        PresetManager::overrideDir = juce::File();
        tmp.deleteRecursively();

        // old reverb macro mappings in a project state
        {
            ElukkaNoiseProcessor proc;
            juce::MemoryBlock mb;
            proc.getStateInformation (mb);
            auto t = juce::ValueTree::fromXml (*proc.getXmlFromBinary (mb.getData(), (int) mb.getSize()));
            juce::ValueTree macro;
            for (const auto& c : t)
                for (const auto& m : c)
                    if (m.hasType ("Map") && m["id"].toString() == "rev_tone") macro = c;
            check (macro.isValid(), "The reverb mapping lives in the state");
            auto setMaps = [&] (std::initializer_list<std::tuple<const char*, int, float, float>> maps)
            {
                macro.removeAllChildren (nullptr);
                for (auto& [id, mode, a, b] : maps)
                {
                    juce::ValueTree m ("Map");
                    m.setProperty ("id", id, nullptr); m.setProperty ("on", true, nullptr); m.setProperty ("mode", mode, nullptr);
                    m.setProperty ("a", a, nullptr); m.setProperty ("b", b, nullptr); m.setProperty ("curve", 0.0f, nullptr);
                    macro.appendChild (m, nullptr);
                }
                t.setProperty ("stateVersion", 2, nullptr);
                juce::MemoryBlock out;
                proc.copyXmlToBinary (*t.createXml(), out);
                proc.setStateInformation (out.getData(), (int) out.getSize());
            };
            setMaps ({ { "rev_lp", P::MapAdd, 0.0f, -0.6f }, { "rev_mod_rate", P::MapAdd, 0.0f, 0.5f } });
            auto e = proc.revPresets.map[P::MrTone];
            check (e.on && e.mode == P::MapAdd && std::abs (e.b + 0.3f) < 1.0e-5f && same (proc.revMapLive.get (P::MrTone), e),
                   "A v0.9.22 Low Pass mapping (Add -60 %) becomes Tone Add -30 % (the left half of the knob), in the engine too; the Mod Rate mapping is dropped");
            setMaps ({ { "rev_hp", P::MapRange, 0.2f, 0.8f } });
            e = proc.revPresets.map[P::MrTone];
            check (e.on && e.mode == P::MapRange && std::abs (e.a - 0.6f) < 1.0e-5f && std::abs (e.b - 0.9f) < 1.0e-5f,
                   "A High Pass Range 20..80 % becomes Tone Range 60..90 % (the right half)");
            setMaps ({ { "rev_hp", P::MapAdd, 0.0f, 0.4f }, { "rev_lp", P::MapRange, 0.2f, 0.8f } });
            e = proc.revPresets.map[P::MrTone];
            check (e.on && e.mode == P::MapRange && std::abs (e.a - 0.1f) < 1.0e-5f && std::abs (e.b - 0.4f) < 1.0e-5f,
                   "With both mapped the Low Pass wins: Tone Range 10..40 %");
            setMaps ({ { "rev_tone", P::MapAdd, 0.0f, 0.25f }, { "rev_lp", P::MapAdd, 0.0f, -0.6f } });
            e = proc.revPresets.map[P::MrTone];
            check (e.on && e.mode == P::MapAdd && std::abs (e.b - 0.25f) < 1.0e-5f, "An existing Tone mapping is kept");
        }

        // dialogs and call-outs close with the editor
        {
            ElukkaNoiseProcessor proc;
            std::unique_ptr<juce::AudioProcessorEditor> ed (proc.createEditor());
            auto* editor = dynamic_cast<ElukkaNoiseEditor*> (ed.get());
            auto& mcm = *juce::ModalComponentManager::getInstance();
            juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
            const int modalBefore = mcm.getNumModalComponents();
            auto label = std::make_unique<juce::Label>();
            label->setSize (100, 30);
            editor->launch (std::move (label), { 10, 10, 20, 20 }, editor);
            auto* w = new juce::AlertWindow ("Test", "", juce::MessageBoxIconType::NoIcon);
            w->addButton ("OK", 1);
            juce::Component::SafePointer<juce::AlertWindow> safeW (w);
            editor->track (*w);
            w->enterModalState (true, nullptr, true);
            check (mcm.getNumModalComponents() == modalBefore + 2, "A call-out and a dialog opened from the editor are modal (" + juce::String (mcm.getNumModalComponents()) + ")");
            ed.reset();
            check (safeW == nullptr && mcm.getNumModalComponents() == modalBefore, "Destroying the editor deletes them at once, so nothing stays modal (" + juce::String (mcm.getNumModalComponents()) + ")");
            juce::MessageManager::getInstance()->runDispatchLoopUntil (50);   // the manager's own clean-up finds nothing left
            check (mcm.getNumModalComponents() == modalBefore, "...and the modal manager's clean-up afterwards is a no-op");
        }
    }

    // The minors from the release audit (v0.9.26)
    std::cout << "\n== Audit minors (v0.9.26) ==\n";
    {
        check (PresetManager::legalName ("CON") == "CON_" && PresetManager::legalName ("nul.old") == "nul.old_" && PresetManager::legalName ("Console") == "Console",
               "Windows device names get an underscore as preset names; ordinary names are untouched");
        {
            ElukkaNoiseProcessor proc;   // defaults: reverb decay 3 s on, delay 1/8 D at 120 bpm with 60 % feedback on
            const double t0 = proc.getTailLengthSeconds();
            setParam (proc, "rev_decay", 20.0f);
            const double t1 = proc.getTailLengthSeconds();
            setParam (proc, "rev_on", 0.0f); setParam (proc, "dly_on", 0.0f);
            const double t2 = proc.getTailLengthSeconds();
            check (t0 > 5.0 && t0 < 7.0 && t1 > 26.0 && t1 <= 30.0 && t2 < 0.5,
                   "Tail length follows the effects: " + juce::String (t0, 1) + " s by default, " + juce::String (t1, 1) + " s with a 20 s decay, "
                   + juce::String (t2, 2) + " s with both effects off");
        }
        {
            auto after = [&] (bool soundOff)
            {
                ElukkaNoiseProcessor proc;
                proc.seedRandom (5);
                proc.presets.load (0);
                setParam (proc, "macro", 100.0f); setParam (proc, "hp_cutoff", 20.0f); setParam (proc, "env_release", 2000.0f);
                proc.setPlayConfigDetails (0, 2, sr, 256);
                proc.prepareToPlay (sr, 256);
                juce::AudioBuffer<float> buf (2, 256);
                for (int b = 0; b < 40; ++b)
                {
                    juce::MidiBuffer midi;
                    if (b == 0) midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
                    proc.processBlock (buf, midi);
                }
                juce::MidiBuffer midi;
                midi.addEvent (soundOff ? juce::MidiMessage::allSoundOff (1) : juce::MidiMessage::allNotesOff (1), 0);
                proc.processBlock (buf, midi);
                float level = rms (buf, 0, 256);
                juce::MidiBuffer none;
                proc.processBlock (buf, none);
                return juce::jmax (level, rms (buf, 0, 256));
            };
            const float panic = after (true), release = after (false);
            check (panic == 0.0f && release > 1.0e-3f, "All Sound Off silences the voice and the tails at once (" + juce::String (panic) + "); All Notes Off releases (" + juce::String (release, 4) + ")");
        }
        {
            // synced 1/1 at 40 bpm would be 6 s, longer than the delay buffer: it drops to 1/2 (3 s), on the grid
            ElukkaNoiseProcessor proc;
            proc.seedRandom (6);
            proc.presets.load (0);
            setParam (proc, "macro", 100.0f); setParam (proc, "hp_cutoff", 20.0f);
            setParam (proc, "rev_on", 0.0f); setParam (proc, "comp_on", 0.0f);
            setParam (proc, "dly_on", 1.0f); setParam (proc, "dly_sync", 1.0f); setParam (proc, "dly_division", 0.0f); setParam (proc, "dly_fb", 0.0f); setParam (proc, "dly_wet", 0.8f);
            FakePlayHead ph; ph.bpm = 40.0;
            RenderOpts o; o.sr = sr; o.seconds = 5.0; o.noteOn = 0.0; o.noteOff = 0.05; o.playHead = &ph;
            auto b = render (proc, o);
            auto db = [] (float v) { return 20.0f * std::log10 (juce::jmax (1.0e-9f, v)); };
            const float at3 = db (rms (b, (int) (3.0 * sr), (int) (3.15 * sr))), at2 = db (rms (b, (int) (2.0 * sr), (int) (2.15 * sr))), at15 = db (rms (b, (int) (1.5 * sr), (int) (1.65 * sr)));
            check (at3 > at2 + 20.0f && at3 > at15 + 20.0f, "Synced 1/1 at 40 bpm echoes after 3 s (1/2), not 6 s: " + juce::String (at3, 1) + " dB at 3 s, "
                                                             + juce::String (at2, 1) + " at 2 s, " + juce::String (at15, 1) + " at 1.5 s");
        }
        {
            // idle buffers are cleared in pieces: echoes do not come back when the delay is turned up again, whether
            // the clear had finished (1 s off) or not (0.4 s off: idle after ~0.25 s, the rest is cleared at once)
            auto replay = [&] (double offFor)
            {
                ElukkaNoiseProcessor proc;
                proc.seedRandom (7);
                proc.presets.load (0);
                setParam (proc, "macro", 100.0f); setParam (proc, "hp_cutoff", 20.0f);
                setParam (proc, "rev_on", 0.0f); setParam (proc, "comp_on", 0.0f);
                setParam (proc, "dly_on", 1.0f); setParam (proc, "dly_sync", 0.0f); setParam (proc, "dly_time", 300.0f); setParam (proc, "dly_fb", 0.9f); setParam (proc, "dly_wet", 0.6f);
                RenderOpts o; o.sr = sr; o.seconds = 3.5; o.noteOn = 0.0; o.noteOff = 0.5;
                o.perBlock = [offFor] (ElukkaNoiseProcessor& p, double t) { setParam (p, "dly_wet", t >= 1.0 && t < 1.0 + offFor ? 0.0f : 0.6f); };
                auto b = render (proc, o);
                return std::make_pair (rms (b, (int) (0.6 * sr), (int) (0.9 * sr)), rms (b, (int) (2.6 * sr), (int) (3.5 * sr)));
            };
            const auto longOff = replay (1.0), shortOff = replay (0.4);
            check (longOff.first > 1.0e-3f && longOff.second < 1.0e-6f && shortOff.second < 1.0e-6f,
                   "Echoes (" + juce::String (longOff.first, 4) + ") do not replay after the delay was off for 1 s (" + juce::String (longOff.second)
                   + ") or 0.4 s (" + juce::String (shortOff.second) + ")");
        }
        {
            // pink noise has the same tilt at 48 and 96 kHz (the filter's poles follow the rate)
            auto bright = [&] (double rate, float colour = 0.5f)
            {
                ElukkaNoiseProcessor proc;
                proc.seedRandom (8);
                proc.presets.load (0);
                setParam (proc, "macro", 100.0f); setParam (proc, "hp_cutoff", 20.0f); setParam (proc, "noise_colour", colour);
                setParam (proc, "rev_on", 0.0f); setParam (proc, "dly_on", 0.0f); setParam (proc, "comp_on", 0.0f);
                RenderOpts o; o.sr = rate; o.seconds = 2.0; o.noteOn = 0.0; o.noteOff = 2.0;
                auto b = render (proc, o);
                return brightness (b, (int) (0.2 * rate), (int) (2.0 * rate), rate);
            };
            const float b48 = bright (48000.0), b96 = bright (96000.0), w48 = bright (48000.0, 1.0f), w96 = bright (96000.0, 1.0f), n48 = bright (48000.0, 0.0f), n96 = bright (96000.0, 0.0f);
            // (the measure itself moves a little with the rate: white shows it too; the old 44.1 kHz poles put pink's 3.9 kHz section at 8.6 kHz at 96 kHz)
            check (std::abs (b48 - b96) < 0.04f, "Pink noise brightness at 48 / 96 kHz: " + juce::String (b48, 3) + " / " + juce::String (b96, 3)
                                                 + " (white " + juce::String (w48, 3) + " / " + juce::String (w96, 3) + ", brown " + juce::String (n48, 3) + " / " + juce::String (n96, 3) + ")");
        }
        {
            ElukkaNoiseProcessor proc;
            auto* pan = proc.apvts.getParameter ("pan");
            auto* colour = proc.apvts.getParameter ("noise_colour");
            const bool r1 = ! parsesAsValue (*pan, "abc"), r2 = parsesAsValue (*pan, "C"), r3 = parsesAsValue (*pan, "30L"), r4 = parsesAsValue (*colour, "pink"), r5 = ! parsesAsValue (*colour, "loud");
            check (r1 && r2 && r3 && r4 && r5,
                   "A typed knob value is applied only when it reads as one (\"C\", \"30L\", \"pink\"; not \"abc\") " + juce::String (r1 ? 1 : 0) + juce::String (r2 ? 1 : 0) + juce::String (r3 ? 1 : 0) + juce::String (r4 ? 1 : 0) + juce::String (r5 ? 1 : 0)
                   + " [C->" + pan->getText (pan->getValueForText ("C"), 64) + ", pink->" + colour->getText (colour->getValueForText ("pink"), 64) + ", loud->" + colour->getText (colour->getValueForText ("loud"), 64) + "]");
            FxMapEditor small (proc.apvts, proc.revPresets, 400, 300), full (proc.apvts, proc.revPresets);
            check (small.getWidth() == 420 && small.getHeight() <= 300 && full.getWidth() == 640 && full.getHeight() == 70 + 26 * 8 + 40,
                   "The reverb's Macro window fits a small editor (" + juce::String (small.getWidth()) + "x" + juce::String (small.getHeight()) + ") and is 640x318 with room");
        }
    }

    std::cout << "\n== Stereo pan ==\n";
    {
        auto levels = [&] (float stereo, float pan, float& l, float& r)
        {
            ElukkaNoiseProcessor proc;
            proc.seedRandom (91);
            proc.presets.load (0);
            setParam (proc, "macro", 100.0f); setParam (proc, "hp_cutoff", 20.0f);
            setParam (proc, "dly_on", 0.0f); setParam (proc, "rev_on", 0.0f); setParam (proc, "comp_on", 0.0f);
            setParam (proc, "noise_stereo", stereo); setParam (proc, "pan", pan);
            RenderOpts o; o.sr = sr; o.seconds = 2.0; o.noteOn = 0.0; o.noteOff = 2.0;
            auto b = render (proc, o);
            l = rms (b, (int) (0.5 * sr), (int) (2.0 * sr), 0);
            r = rms (b, (int) (0.5 * sr), (int) (2.0 * sr), 1);
        };
        auto db = [] (float v) { return 20.0f * std::log10 (juce::jmax (1.0e-9f, v)); };
        for (float stereo : { 0.0f, 1.0f, 2.0f })
        {
            float lo = 1000.0f, hi = -1000.0f;
            for (float pan : { -1.0f, -0.5f, 0.0f, 0.5f, 1.0f })
            {
                float l, r; levels (stereo, pan, l, r);
                const float total = db (std::sqrt (l * l + r * r));
                lo = juce::jmin (lo, total); hi = juce::jmax (hi, total);
            }
            check (hi - lo < 0.6f, "Stereo " + juce::String (juce::roundToInt (stereo * 100)) + " %: level across the pan within "
                                   + juce::String (hi - lo, 2) + " dB");
        }
        float l, r;
        levels (0.0f, 0.5f, l, r);
        check (std::abs (db (r) - db (l) - 7.66f) < 0.5f, "Mono noise at pan 50R: right " + juce::String (db (r) - db (l), 2) + " dB over left (balance law 7.66)");
        levels (1.0f, 1.0f, l, r);
        check (db (l) < db (r) - 80.0f, "Stereo 100 %, hard right: left silent (" + juce::String (db (l) - db (r), 1) + " dB)");
        float lc, rc; levels (1.0f, 0.0f, lc, rc);
        levels (1.0f, 0.5f, l, r);
        check (db (l) - db (lc) > -3.6f, "Stereo 100 %, pan 50R: left channel " + juce::String (db (l) - db (lc), 2)
                                         + " dB from centre (the left noise moves to the middle; a balance pan fades it to -5.3)");
    }

    std::cout << "\n== Merged knobs (v0.9.23): Noise Stereo, reverb Tone, Mod ==\n";
    {
        // Noise Stereo 0..2: 0 = identical channels, 1 = independent noise, 2 = the side signal widened further
        auto side = [&] (float stereo)
        {
            ElukkaNoiseProcessor proc;
            proc.seedRandom (9);
            proc.presets.load (proc.presets.indexOf ("Init", "Init"));
            setParam (proc, "dly_on", 0.0f); setParam (proc, "rev_on", 0.0f); setParam (proc, "noise_stereo", stereo);
            RenderOpts o; o.sr = sr; o.seconds = 1.0; o.noteOff = 1.0;
            auto b = render (proc, o);
            juce::AudioBuffer<float> s (1, b.getNumSamples()), m (1, b.getNumSamples());
            for (int i = 0; i < b.getNumSamples(); ++i) { s.setSample (0, i, 0.5f * (b.getSample (0, i) - b.getSample (1, i))); m.setSample (0, i, 0.5f * (b.getSample (0, i) + b.getSample (1, i))); }
            return rms (s, (int) (0.2 * sr), s.getNumSamples()) / juce::jmax (1.0e-9f, rms (m, (int) (0.2 * sr), m.getNumSamples()));
        };
        const float s0 = side (0.0f), s1 = side (1.0f), s2 = side (2.0f);
        check (s0 < 0.001f && s1 > 0.7f && s1 < 1.4f && s2 > 1.6f * s1, "Noise Stereo: 0 % = mono (side / mid " + juce::String (s0, 4) + "), 100 % = independent noise (" + juce::String (s1, 2) + "), 200 % = wider still (" + juce::String (s2, 2) + ")");
        check (std::abs (P::modRateFor (0.0f) - 0.3f) < 1.0e-4f && std::abs (P::modRateFor (1.0f) - 3.0f) < 1.0e-3f && std::abs (P::modRateFor (0.25f) - 0.533f) < 0.01f && std::abs (P::modRateFor (0.5f) - 0.949f) < 0.01f,
               "Mod rate follows the depth: 0.3 Hz at 0 %, 0.53 Hz at 25 % (the reverb's default, as before), 0.95 Hz at 50 %, 3 Hz at 100 % (delay and reverb)");
        check (std::abs (P::reverbToneLp (-0.75f) - 1257.0f) < 5.0f && P::reverbToneHp (-0.75f) == 80.0f && P::reverbToneLp (0.5f) == 20000.0f && std::abs (P::reverbToneHp (0.5f) - 400.0f) < 1.0f
               && std::abs (P::reverbToneFor (700.0f, 90.0f) + 0.909f) < 0.01f && std::abs (P::reverbToneFor (20000.0f, 1000.0f) - 0.785f) < 0.01f,
               "Reverb Tone: -75 % (the default) = low pass 1.26 kHz, +50 % = high pass 400 Hz; the old 700 Hz / 90 Hz pair maps to -91 %, a 1 kHz high pass to +78 %");

        // a v0.9.22 project: Width + Stereo -> Noise Stereo, Low Pass / High Pass -> Tone, the mod rates dropped, matrix slots re-indexed
        {
            ElukkaNoiseProcessor proc;
            juce::MemoryBlock mb;
            proc.getStateInformation (mb);
            auto t = juce::ValueTree::fromXml (*proc.getXmlFromBinary (mb.getData(), (int) mb.getSize()));
            t.setProperty ("stateVersion", 2, nullptr);
            auto setP = [&t] (const juce::String& id, float v)
            {
                for (auto c : t) if (c.hasType ("PARAM") && c["id"].toString() == id) { c.setProperty ("value", v, nullptr); return; }
                juce::ValueTree c ("PARAM"); c.setProperty ("id", id, nullptr); c.setProperty ("value", v, nullptr); t.appendChild (c, nullptr);
            };
            for (int i = t.getNumChildren(); --i >= 0;)
                if (t.getChild (i).hasType ("PARAM") && (t.getChild (i)["id"].toString() == "noise_stereo" || t.getChild (i)["id"].toString() == "rev_tone")) t.removeChild (i, nullptr);
            setP ("width", 0.4f); setP ("noise_stereo", 1.0f);
            setP ("rev_lp", 700.0f); setP ("rev_hp", 90.0f); setP ("dly_mod_rate", 3.0f); setP ("rev_mod_rate", 2.0f);
            setP ("m1_dst", 15.0f);   // Width in the v0.9.20 .. v0.9.22 table (index 14 + 1)
            setP ("m2_dst", 16.0f);   // Level (15 + 1)
            setP ("m3_dst", 35.0f);   // Filter 2 Drive (34 + 1)
            auto xml = t.createXml();
            juce::MemoryBlock mb2;
            proc.copyXmlToBinary (*xml, mb2);
            proc.setStateInformation (mb2.getData(), (int) mb2.getSize());
            auto v = [&proc] (const char* id) { return proc.apvts.getRawParameterValue (id)->load(); };
            check (std::abs (v ("noise_stereo") - 0.4f) < 1.0e-4f && std::abs (v ("rev_tone") + 0.909f) < 0.01f
                   && (int) v ("m1_dst") == P::DNoiseStereo + 1 && (int) v ("m2_dst") == P::DLevel + 1 && (int) v ("m3_dst") == P::DDrive2 + 1,
                   "A v0.9.22 project migrates: Width 40 % -> Noise Stereo 40 %, Low Pass 700 Hz / High Pass 90 Hz -> Tone -91 %, matrix slots Width -> Noise Stereo, Level and Filter 2 Drive keep their meaning");
            // an old user reverb preset file (Low Pass / High Pass) through the effect preset loader
            auto rt = proc.revPresets.toTree ("Old Dark");
            auto params = rt.getChildWithName ("PARAMS");
            for (int i = params.getNumChildren(); --i >= 0;) if (params.getChild (i)["id"].toString() == "rev_tone") params.removeChild (i, nullptr);
            juce::ValueTree lp ("PARAM"); lp.setProperty ("id", "rev_lp", nullptr); lp.setProperty ("value", 8000.0f, nullptr); params.appendChild (lp, nullptr);
            setParam (proc, "rev_tone", 0.3f);
            proc.revPresets.applyTree (rt);
            check (std::abs (v ("rev_tone") + 0.248f) < 0.01f, "An old reverb preset file with Low Pass 8 kHz loads as Tone -25 %");
            // a v0.9.22 instrument preset file: width and a matrix slot on it
            auto it = proc.presets.toTree ("Old Wide", "FX");
            it.setProperty ("version", 3, nullptr);
            auto ip = it.getChildWithName ("PARAMS");
            for (int i = ip.getNumChildren(); --i >= 0;) if (ip.getChild (i)["id"].toString() == "noise_stereo") ip.removeChild (i, nullptr);
            juce::ValueTree w ("PARAM"); w.setProperty ("id", "width", nullptr); w.setProperty ("value", 1.5f, nullptr); ip.appendChild (w, nullptr);
            for (auto c : ip) if (c["id"].toString() == "m1_dst") c.setProperty ("value", 15.0f, nullptr);
            proc.presets.applyTree (it);
            check (std::abs (v ("noise_stereo") - 1.5f) < 1.0e-4f && (int) v ("m1_dst") == P::DNoiseStereo + 1, "A v0.9.22 instrument preset file: Width 150 % -> Noise Stereo 150 %, its matrix slot on Width now on Noise Stereo");
        }
    }

    std::cout << "\n== State ==\n";
    {
        ElukkaNoiseProcessor proc;
        proc.presets.load (proc.presets.indexOf ("Psy Filter", "FX"));
        setParam (proc, "macro", 42.0f);
        setParam (proc, "flt_cutoff", 777.0f);
        proc.dlyPresets.loadByName ("Psy Feedback");
        proc.revPresets.loadByName ("Dark Bloom");
        proc.revPresets.setMap (P::MrWidth, { true, P::MapRange, 0.2f, 0.9f, 0.1f });
        setParam (proc, "rev_pre", 55.0f);
        proc.uiExpanded = true;
        juce::MemoryBlock state;
        proc.getStateInformation (state);
        proc.presets.load (0);
        proc.dlyPresets.load (0); proc.revPresets.load (0);
        setParam (proc, "macro", 5.0f);
        proc.uiExpanded = false;
        proc.setStateInformation (state.getData(), (int) state.getSize());
        {
            const auto& w = proc.revPresets.map[P::MrWidth];
            check (proc.dlyPresets.currentName == "Psy Feedback" && proc.revPresets.currentName == "Dark Bloom" && proc.revMapLive.character == P::RDarkBloom
                   && w.on && w.mode == P::MapRange && std::abs (w.a - 0.2f) < 1e-6f && std::abs (w.b - 0.9f) < 1e-6f && std::abs (w.curve - 0.1f) < 1e-6f
                   && std::abs (proc.revMapLive.b[P::MrWidth].load() - 0.9f) < 1e-6f && std::abs (proc.apvts.getRawParameterValue ("rev_pre")->load() - 55.0f) < 0.01f,
                   "Effect presets, the reverb's character, an edited Range mapping and an edited knob restored from project state");
        }
        check (std::abs (proc.apvts.getRawParameterValue ("macro")->load() - 42.0f) < 0.01f, "Macro restored from project state");
        check (std::abs (proc.apvts.getRawParameterValue ("flt_cutoff")->load() - 777.0f) < 0.5f, "Edited cutoff restored from project state");
        check (proc.presets.currentName == "Psy Filter" && proc.presets.currentCategory == "FX", "Preset name and category restored");
        check (proc.uiExpanded, "Editor expanded state restored");
        // all parameters round-trip through a preset file
        auto t = proc.presets.toTree ("RoundTrip", "FX");
        proc.presets.load (0);
        proc.presets.applyTree (t);
        check (std::abs (proc.apvts.getRawParameterValue ("flt_cutoff")->load() - 777.0f) < 0.5f, "Preset tree round-trip restores parameters");
        check (! PresetManager::userPresetDir().getFullPathName().contains ("Vibecode"), "User presets live in the application data folder: " + PresetManager::userPresetDir().getFullPathName());

        // user presets: save, load back, rename, delete, factory protection (in a temporary folder)
        auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("ElukkaNoiseTestPresets");
        tmp.deleteRecursively();
        PresetManager::overrideDir = tmp;
        auto& pm = proc.presets;
        pm.refresh();
        const int factoryCount = (int) pm.entries.size();
        setParam (proc, "flt_cutoff", 1234.0f);
        check (pm.save ("My Riser", "Risers / Falls"), "Save a user preset");
        check ((int) pm.entries.size() == factoryCount + 1 && pm.currentName == "My Riser" && pm.currentIndex >= 0 && ! pm.entries[(size_t) pm.currentIndex].isFactory, "Saved preset is listed as a user preset and is current");
        pm.load (0);
        check (std::abs (proc.apvts.getRawParameterValue ("flt_cutoff")->load() - 1234.0f) > 1.0f, "Loading a factory preset changes the cutoff back");
        check (pm.loadByName ("My Riser", "Risers / Falls") && std::abs (proc.apvts.getRawParameterValue ("flt_cutoff")->load() - 1234.0f) < 0.5f, "Loading the user preset restores its cutoff");
        check (! pm.deletePreset (0) && ! pm.rename (0, "Nope"), "Factory presets cannot be deleted or renamed");
        check (pm.rename (pm.currentIndex, "My Better Riser") && pm.currentName == "My Better Riser" && pm.indexOf ("My Riser", "Risers / Falls") < 0, "Rename a user preset");
        check (pm.save ("My Better Riser", "Risers / Falls") && (int) pm.entries.size() == factoryCount + 1, "Saving under an existing user name overwrites it");
        check (pm.deletePreset (pm.currentIndex) && (int) pm.entries.size() == factoryCount, "Delete a user preset");
        PresetManager::overrideDir = juce::File();
        tmp.deleteRecursively();
    }

    // 9. Mono output bus.
    std::cout << "\n== Mono output ==\n";
    {
        ElukkaNoiseProcessor proc;
        proc.seedRandom (9);
        proc.presets.load (0);
        setParam (proc, "macro", 70.0f);
        proc.setPlayConfigDetails (0, 1, sr, 512);
        proc.prepareToPlay (sr, 512);
        juce::AudioBuffer<float> buf (1, 512);
        float pk = 0.0f;
        for (int i = 0; i < 100; ++i)
        {
            juce::MidiBuffer midi;
            if (i == 0) midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
            proc.processBlock (buf, midi);
            pk = juce::jmax (pk, buf.getMagnitude (0, 512));
        }
        check (pk > 0.01f && std::isfinite (pk), "Mono output renders (peak " + juce::String (pk, 3) + ")");
    }

    // 10. Bypass keeps note state.
    std::cout << "\n== Bypass ==\n";
    {
        ElukkaNoiseProcessor proc;
        proc.presets.load (0);
        setParam (proc, "macro", 70.0f);
        proc.setPlayConfigDetails (0, 2, sr, 512);
        proc.prepareToPlay (sr, 512);
        juce::AudioBuffer<float> buf (2, 512);
        juce::MidiBuffer on; on.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
        proc.processBlockBypassed (buf, on);
        const float bypassed = buf.getMagnitude (0, 512);
        juce::MidiBuffer none;
        for (int i = 0; i < 10; ++i) proc.processBlock (buf, none);
        check (bypassed == 0.0f && buf.getMagnitude (0, 512) > 0.01f, "Bypassed block is silent; the note started during bypass sounds afterwards");
    }

    std::cout << "\n" << (failures == 0 ? "ALL TESTS PASSED" : juce::String (failures) + " FAILURE(S)") << "\n";
    return failures == 0 ? 0 : 1;
}
