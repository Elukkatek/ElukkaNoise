#include "FxPresets.h"
#include "PluginProcessor.h"

FxPresetManager::FxPresetManager (ElukkaNoiseProcessor& p, Kind k, P::FxMapLive& l) : kind (k), live (l), proc (p) {}

// Factory presets. Delay: Clean Rhythmic is the default (its knobs are the parameter defaults). Mappings are additive
// unless a Range is wanted: the knobs stay the reference, so turning one moves the whole macro travel with it.
// Depths are in the knob's normalised range (+0.3 on Mix = +30 %; on a log knob like Decay, a share of its travel).
const std::vector<FxPreset>& FxPresetManager::factory (Kind k)
{
    using namespace P;
    static const std::vector<FxPreset> delay = []
    {
        std::vector<FxPreset> v;
        auto add = [&] (const char* name) -> FxPreset& { v.push_back ({ name }); return v.back(); };
        {
            // 1/8 D ping-pong: the macro brings the echoes in late, feeding back a little more and spreading a little wider
            auto& p = add ("Clean Rhythmic");
            p.set ("dly_width", 0.8f);
            p.add (MdMix, 0.35f, 0.3f).add (MdFb, 0.15f).add (MdWidth, 0.2f);
        }
        {
            // the stereo input kept, every echo swapping sides; the macro opens it up: wetter, longer, wider
            auto& p = add ("Wide Ping-Pong");
            p.set ("dly_division", 6.0f).set ("dly_mode", (float) DmWidePingPong).set ("dly_fb", 0.5f).set ("dly_filter", -0.1f).set ("dly_width", 0.6f);
            p.add (MdMix, 0.4f, 0.2f).add (MdFb, 0.3f).add (MdWidth, 0.4f);
        }
        {
            // 1/8 T, offset right line, ducked: the macro feeds it back into a dense wall while the Filter closes
            auto& p = add ("Dark Industrial");
            p.set ("dly_division", 8.0f).set ("dly_mode", (float) DmWidePingPong).set ("dly_fb", 0.55f).set ("dly_filter", -0.3f).set ("dly_width", 0.5f)
             .set ("dly_offset", 0.15f).set ("dly_mod", 0.1f).set ("dly_duck", 0.5f);
            p.add (MdMix, 0.3f).add (MdFb, 0.4f).add (MdFilter, -0.5f);
        }
        {
            // 1/16 ping-pong, thin repeats: the macro adds feedback (the loop limiter keeps it stable), wobble and speed
            auto& p = add ("Psy Feedback");
            p.set ("dly_division", 9.0f).set ("dly_fb", 0.6f).set ("dly_filter", 0.25f).set ("dly_mod", 0.3f).set ("dly_duck", 0.4f);
            p.add (MdMix, 0.3f).add (MdFb, 0.35f).add (MdMod, 0.5f);
        }
        {
            // 1/2 wide echoes, dark and slow-moving: the macro brings them in softly, a little wider and longer
            auto& p = add ("Atmospheric");
            p.set ("dly_division", 1.0f).set ("dly_mode", (float) DmWidePingPong).set ("dly_fb", 0.5f).set ("dly_filter", -0.25f).set ("dly_width", 0.8f)
             .set ("dly_mod", 0.2f);
            p.add (MdMix, 0.3f, 0.3f).add (MdWidth, 0.2f).add (MdFb, 0.2f);
        }
        {
            // 1/8 ping-pong ducked 60 %: the echoes fill the gaps between gated pulses and grow with the macro
            auto& p = add ("Ducked Gate Echo");
            p.set ("dly_division", 6.0f).set ("dly_wet", 0.12f).set ("dly_duck", 0.6f);
            p.add (MdMix, 0.2f, 0.3f);
        }
        {
            auto& p = add ("Dark Feedback");
            p.set ("dly_division", 6.0f).set ("dly_mode", (float) DmStereo).set ("dly_fb", 0.8f).set ("dly_filter", -0.7f).set ("dly_width", 0.6f).set ("dly_duck", 0.2f);
            p.add (MdMix, 0.3f).add (MdFb, 0.15f);
        }
        {
            auto& p = add ("Modulated Echo");
            p.set ("dly_division", 3.0f).set ("dly_mode", (float) DmStereo).set ("dly_fb", 0.5f).set ("dly_filter", -0.15f).set ("dly_width", 0.9f)
             .set ("dly_offset", -0.1f).set ("dly_mod", 0.5f);
            p.add (MdMix, 0.3f).add (MdMod, 0.4f);
        }
        {
            // for a fall (100 % -> 0 %): 1/8 ping-pong echoes that grow as the macro falls (the macro pulls the mix and
            // feedback down, so the knobs stay live), so the space takes over as the dry noise fades
            auto& p = add ("Fall Echo");
            p.set ("dly_division", 6.0f).set ("dly_wet", 0.35f).set ("dly_fb", 0.65f).set ("dly_filter", -0.2f).set ("dly_width", 0.8f);
            p.add (MdMix, -0.3f).add (MdFb, -0.3f);
        }
        {
            // the delay out of the way: mix 0, no mapping (the instrument presets that want no echo)
            add ("Off");
        }
        return v;
    }();
    static const std::vector<FxPreset> reverb = []
    {
        std::vector<FxPreset> v;
        auto add = [&] (const char* name, int character) -> FxPreset& { v.push_back ({ name }); v.back().character = character; return v.back(); };
        {
            // Clean Hall internals, the parameter defaults: the macro brings the reverb in late and lengthens it a little
            auto& p = add ("Clean Space", RHall);
            p.add (MrMix, 0.3f, 0.3f).add (MrDecay, 0.15f);
        }
        {
            // the macro grows a huge, evolving tail: longer, bigger, wetter, wider
            auto& p = add ("Endless Space", REndless);
            p.set ("rev_size", 0.7f).set ("rev_decay", 6.0f).set ("rev_tone", -0.55f).set ("rev_pre", 20.0f).set ("rev_width", 0.7f)
             .set ("rev_mod", 0.3f).set ("rev_duck", 0.3f);
            p.add (MrDecay, 0.4f).add (MrSize, 0.3f).add (MrMix, 0.35f, 0.2f).add (MrWidth, 0.3f);
        }
        {
            // dark (Low Pass 700 Hz, highs dying 8x faster), ducked: the macro lengthens and lifts it, never brightens it
            auto& p = add ("Dark Bloom", RDarkBloom);
            p.set ("rev_size", 0.85f).set ("rev_decay", 4.0f).set ("rev_tone", -0.9f).set ("rev_pre", 30.0f).set ("rev_width", 0.9f)
             .set ("rev_mod", 0.2f).set ("rev_duck", 0.6f);
            p.add (MrDecay, 0.3f).add (MrMix, 0.35f, 0.3f);
        }
        {
            // bright but not harsh (Low Pass 8 kHz): the macro lengthens, widens and lifts it
            auto& p = add ("Bright Hall", RBright);
            p.set ("rev_size", 0.6f).set ("rev_decay", 2.5f).set ("rev_tone", -0.25f).set ("rev_width", 0.7f).set ("rev_duck", 0.2f);
            p.add (MrDecay, 0.3f).add (MrWidth, 0.3f).add (MrMix, 0.3f);
        }
        {
            // moving, evolving texture: the macro lengthens and widens it and swirls the tail
            auto& p = add ("Psy Atmosphere", RPsy);
            p.set ("rev_size", 0.9f).set ("rev_decay", 5.0f).set ("rev_tone", -0.45f).set ("rev_pre", 40.0f).set ("rev_width", 0.6f)
             .set ("rev_mod", 0.4f).set ("rev_duck", 0.5f);
            p.add (MrDecay, 0.25f).add (MrWidth, 0.4f).add (MrMod, 0.5f);
        }
        {
            // towards a frozen texture: Decay travels 4 s -> 20 s (the longest there is), the dry ducks the tail more
            // and more, the mix rises. No topology change: smooth all the way, and the tail never runs away.
            auto& p = add ("Frozen Atmosphere", REndless);
            p.set ("rev_size", 1.0f).set ("rev_decay", 4.0f).set ("rev_tone", -0.5f).set ("rev_pre", 20.0f).set ("rev_mod", 0.15f)
             .set ("rev_duck", 0.3f);
            p.range (MrDecay, 4.0f, 20.0f, 0.3f).add (MrMix, 0.4f, 0.3f).add (MrDuck, 0.4f);
        }
        {
            auto& p = add ("Tight Space", RTight);
            p.set ("rev_size", 0.35f).set ("rev_decay", 0.8f).set ("rev_tone", -0.3f).set ("rev_pre", 5.0f).set ("rev_width", 0.8f)
             .set ("rev_mod", 0.25f).set ("rev_duck", 0.4f);
            p.add (MrMix, 0.25f).add (MrDecay, 0.15f);
        }
        {
            // ducking with a macro-controlled bloom: the dry pushes the tail down 70 %, the macro lifts and lengthens it
            auto& p = add ("Ducked Bloom", RHall);
            p.set ("rev_duck", 0.7f);
            p.add (MrMix, 0.3f, 0.3f).add (MrDecay, 0.25f);
        }
        {
            auto& p = add ("Modulated Space", RModulated);
            p.set ("rev_size", 0.75f).set ("rev_decay", 5.0f).set ("rev_tone", -0.5f).set ("rev_pre", 15.0f).set ("rev_mod", 0.6f);
            p.add (MrMix, 0.3f).add (MrMod, 0.4f);
        }
        {
            // for a fall: a big wash whose mix and decay grow as the macro falls (the macro pulls them down)
            auto& p = add ("Fall Wash", REndless);
            p.set ("rev_size", 0.8f).set ("rev_decay", 9.0f).set ("rev_tone", -0.5f).set ("rev_pre", 20.0f).set ("rev_width", 0.8f)
             .set ("rev_wet", 0.45f).set ("rev_mod", 0.2f).set ("rev_duck", 0.3f);
            p.add (MrMix, -0.37f).add (MrDecay, -0.25f);
        }
        {
            add ("Off", RHall);   // mix 0, no mapping
        }
        return v;
    }();
    return k == Delay ? delay : reverb;
}

juce::String FxPresetManager::presetForOldCharacter (Kind k, int c)
{
    static const char* rev[P::numRevCharacters] { "Tight Space", "Clean Space", "Endless Space", "Dark Bloom", "Bright Hall", "Modulated Space", "Psy Atmosphere" };
    static const char* dly[8] { "Clean Rhythmic", "Clean Rhythmic", "Clean Rhythmic", "Atmospheric", "Dark Feedback", "Modulated Echo", "Psy Feedback", "Dark Industrial" };
    return k == Reverb ? rev[juce::jlimit (0, P::numRevCharacters - 1, c)] : dly[juce::jlimit (0, 7, c)];
}

juce::File FxPresetManager::userDir() const
{
    return PresetManager::userPresetDir().getChildFile (title());
}

void FxPresetManager::refresh()
{
    entries.clear();
    const auto& f = factory (kind);
    for (int i = 0; i < (int) f.size(); ++i) entries.push_back ({ f[(size_t) i].name, true, i, {} });
    juce::Array<juce::File> files = userDir().findChildFiles (juce::File::findFiles, false, juce::String ("*") + fileExtension);
    files.sort();
    for (auto& file : files) entries.push_back ({ file.getFileNameWithoutExtension(), false, -1, file });
    currentIndex = indexOf (currentName);
}

int FxPresetManager::indexOf (const juce::String& name, bool factoryOnly) const
{
    for (int i = 0; i < (int) entries.size(); ++i)
        if (entries[(size_t) i].name == name && (! factoryOnly || entries[(size_t) i].isFactory)) return i;
    return -1;
}

static void setParam (ElukkaNoiseProcessor& proc, const juce::String& id, float value)
{
    if (auto* p = proc.apvts.getParameter (id))
        p->setValueNotifyingHost (p->convertTo0to1 (value));
}

void FxPresetManager::setMapping (const P::FxMapEntry* m, int ch)
{
    for (int d = 0; d < P::maxFxMapDests; ++d) map[d] = d < numDests() ? m[d] : P::FxMapEntry();
    character = ch;
    push();
}

void FxPresetManager::push()
{
    for (int d = 0; d < P::maxFxMapDests; ++d) live.set (d, map[d]);
    live.character = character;
    writeToState();
}

void FxPresetManager::applyFactory (const FxPreset& p)
{
    // this effect's parameters: defaults first, then what the preset changes
    for (auto* prm : proc.getParameters())
        if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (prm))
            if (rp->paramID.startsWith (prefix())) rp->setValueNotifyingHost (rp->getDefaultValue());
    for (auto& [id, value] : p.values)
    {
        jassert (proc.apvts.getParameter (id) != nullptr && id.startsWith (prefix()));
        setParam (proc, id, value);
    }
    P::FxMapEntry m[P::maxFxMapDests];
    for (int d = 0; d < numDests(); ++d)
    {
        m[d] = p.map[d];
        if (p.mapInUnits[d])   // a Range written in the knob's units
            if (auto* prm = proc.apvts.getParameter (dests()[d].paramID))
            {
                m[d].a = prm->convertTo0to1 (m[d].a);
                m[d].b = prm->convertTo0to1 (m[d].b);
            }
    }
    setMapping (m, p.character);
}

juce::ValueTree FxPresetManager::toTree (const juce::String& name) const
{
    juce::ValueTree t ("ElukkaNoiseFxPreset");
    t.setProperty ("version", 1, nullptr);
    t.setProperty ("kind", title(), nullptr);
    t.setProperty ("name", name, nullptr);
    if (kind == Reverb) t.setProperty ("character", character, nullptr);
    juce::ValueTree params ("PARAMS");
    for (auto* p : proc.getParameters())
        if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p))
            if (rp->paramID.startsWith (prefix()))
            {
                juce::ValueTree v ("PARAM");
                v.setProperty ("id", rp->paramID, nullptr);
                v.setProperty ("value", rp->convertFrom0to1 (rp->getValue()), nullptr);
                params.appendChild (v, nullptr);
            }
    t.appendChild (params, nullptr);
    for (int d = 0; d < numDests(); ++d)
    {
        juce::ValueTree m ("Map");
        m.setProperty ("id", dests()[d].paramID, nullptr);
        m.setProperty ("on", map[d].on, nullptr);
        m.setProperty ("mode", map[d].mode, nullptr);
        m.setProperty ("a", map[d].a, nullptr);
        m.setProperty ("b", map[d].b, nullptr);
        m.setProperty ("curve", map[d].curve, nullptr);
        t.appendChild (m, nullptr);
    }
    return t;
}

// the Map children of a preset or state tree -> entries (a destination without one is off)
static void readMaps (const juce::ValueTree& t, const P::FxDest* dests, int n, P::FxMapEntry* out)
{
    for (int d = 0; d < P::maxFxMapDests; ++d) out[d] = {};
    auto entry = [] (const juce::ValueTree& m)
    {
        return P::FxMapEntry { (bool) m.getProperty ("on", false), (int) m.getProperty ("mode", 0) == P::MapRange ? P::MapRange : P::MapAdd,
                               juce::jlimit (0.0f, 1.0f, (float) m.getProperty ("a", 0.0f)), juce::jlimit (-1.0f, 1.0f, (float) m.getProperty ("b", 0.0f)),
                               juce::jlimit (-1.0f, 1.0f, (float) m.getProperty ("curve", 0.0f)) };
    };
    for (const auto& m : t)
    {
        if (! m.hasType ("Map")) continue;
        for (int d = 0; d < n; ++d)
            if (m["id"].toString() == dests[d].paramID) out[d] = entry (m);
    }
    // v0.9.20-0.9.22 reverb mappings on the Low Pass / High Pass knobs (now one Tone knob: its left half is the low
    // pass 20 kHz -> 500 Hz, its right half the high pass 80 Hz -> 2 kHz) become a Tone mapping; the low pass wins when
    // both were mapped. Mod Rate mappings are dropped (the rate follows the depth since v0.9.23).
    if (dests == P::revMapDests && ! out[P::MrTone].on)
        for (auto* id : { "rev_lp", "rev_hp" })
            for (const auto& m : t)
                if (m.hasType ("Map") && ! out[P::MrTone].on && m["id"].toString() == id && (bool) m.getProperty ("on", false))
                {
                    auto e = entry (m);
                    const bool hp = juce::String (id) == "rev_hp";
                    if (e.mode == P::MapRange) { e.a = hp ? 0.5f + 0.5f * e.a : 0.5f * e.a; e.b = hp ? 0.5f + 0.5f * e.b : 0.5f * e.b; }
                    else e.b *= 0.5f;
                    out[P::MrTone] = e;
                    break;
                }
}

void FxPresetManager::applyTree (const juce::ValueTree& t)
{
    for (auto* prm : proc.getParameters())
        if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (prm))
            if (rp->paramID.startsWith (prefix())) rp->setValueNotifyingHost (rp->getDefaultValue());
    auto params = t.getChildWithName ("PARAMS");
    P::migrateOldDelayParams (params);
    P::migrateStereoAndTone (params);   // v0.9.23: rev_lp / rev_hp -> rev_tone, mod rates gone
    for (const auto& v : params)
        if (v.hasType ("PARAM") && v["id"].toString().startsWith (prefix())) setParam (proc, v["id"].toString(), (float) v["value"]);
    P::FxMapEntry m[P::maxFxMapDests];
    readMaps (t, dests(), numDests(), m);
    setMapping (m, juce::jlimit (0, P::numRevCharacters - 1, (int) t.getProperty ("character", P::RHall)));
}

void FxPresetManager::load (int entryIndex)
{
    if (entryIndex < 0 || entryIndex >= (int) entries.size()) return;
    auto& e = entries[(size_t) entryIndex];
    juce::ValueTree t;
    if (! e.isFactory)   // read the file first: a corrupt one leaves the current preset as it is
    {
        auto xml = juce::XmlDocument::parse (e.file);
        if (xml == nullptr) return;
        t = juce::ValueTree::fromXml (*xml);
        if (! t.hasType ("ElukkaNoiseFxPreset")) return;
    }
    currentIndex = entryIndex;   // set before applying: push() writes the name into the state
    currentName = e.name;
    if (e.isFactory) applyFactory (factory (kind)[(size_t) e.index]);
    else applyTree (t);
    loadedTree = toTree (currentName);
    proc.bumpUiVersion();
}

bool FxPresetManager::isModified() const
{
    return currentIndex >= 0 && ! toTree (currentName).isEquivalentTo (loadedTree);
}

bool FxPresetManager::loadByName (const juce::String& name)
{
    const int i = indexOf (name);
    if (i < 0) return false;
    load (i);
    return true;
}

bool FxPresetManager::save (const juce::String& nameIn)
{
    const auto name = PresetManager::legalName (nameIn);
    if (name.isEmpty() || indexOf (name, true) >= 0) return false;   // a factory name would shadow the user's file
    auto dir = userDir();
    if (! dir.createDirectory()) return false;
    auto file = dir.getChildFile (juce::File::createLegalFileName (name) + fileExtension);
    if (auto xml = toTree (name).createXml())
        if (! xml->writeTo (file)) return false;
    currentName = name;
    refresh();
    writeToState();
    loadedTree = toTree (currentName);
    proc.bumpUiVersion();
    return true;
}

bool FxPresetManager::deletePreset (int entryIndex)
{
    if (entryIndex < 0 || entryIndex >= (int) entries.size()) return false;
    auto e = entries[(size_t) entryIndex];
    if (e.isFactory || ! e.file.existsAsFile()) return false;
    if (! e.file.moveToTrash() && ! e.file.deleteFile()) return false;   // no Trash (a bare Linux session): delete outright
    refresh();
    if (currentName == e.name) currentIndex = -1;
    proc.bumpUiVersion();
    return true;
}

void FxPresetManager::setMap (int d, const P::FxMapEntry& e)
{
    if (d < 0 || d >= numDests()) return;
    map[d] = e;
    map[d].a = juce::jlimit (0.0f, 1.0f, map[d].a);
    map[d].b = juce::jlimit (-1.0f, 1.0f, map[d].b);
    map[d].curve = juce::jlimit (-1.0f, 1.0f, map[d].curve);
    push();
}

void FxPresetManager::restoreMap()
{
    if (currentIndex < 0 || currentIndex >= (int) entries.size()) return;
    auto& e = entries[(size_t) currentIndex];
    P::FxMapEntry m[P::maxFxMapDests];
    if (e.isFactory)
    {
        const auto& p = factory (kind)[(size_t) e.index];
        for (int d = 0; d < numDests(); ++d)
        {
            m[d] = p.map[d];
            if (p.mapInUnits[d])
                if (auto* prm = proc.apvts.getParameter (dests()[d].paramID))
                {
                    m[d].a = prm->convertTo0to1 (m[d].a);
                    m[d].b = prm->convertTo0to1 (m[d].b);
                }
        }
        setMapping (m, p.character);
    }
    else if (auto xml = juce::XmlDocument::parse (e.file))
    {
        auto t = juce::ValueTree::fromXml (*xml);
        readMaps (t, dests(), numDests(), m);
        setMapping (m, juce::jlimit (0, P::numRevCharacters - 1, (int) t.getProperty ("character", character)));
    }
}

void FxPresetManager::selectMappingOf (const juce::String& factoryName)
{
    const int i = indexOf (factoryName, true);
    if (i < 0) return;
    currentIndex = i;
    currentName = factoryName;
    const auto& p = factory (kind)[(size_t) entries[(size_t) i].index];
    P::FxMapEntry m[P::maxFxMapDests];
    for (int d = 0; d < numDests(); ++d)
    {
        m[d] = p.map[d];
        if (p.mapInUnits[d])
            if (auto* prm = proc.apvts.getParameter (dests()[d].paramID))
            {
                m[d].a = prm->convertTo0to1 (m[d].a);
                m[d].b = prm->convertTo0to1 (m[d].b);
            }
    }
    setMapping (m, p.character);
}

static const juce::Identifier dlyMacroType ("DelayMacro"), revMacroType ("ReverbMacro");

void FxPresetManager::writeToState()
{
    auto st = proc.apvts.state;
    auto c = st.getOrCreateChildWithName (kind == Delay ? dlyMacroType : revMacroType, nullptr);
    c.removeAllChildren (nullptr);
    c.setProperty ("preset", currentName, nullptr);
    if (kind == Reverb) c.setProperty ("character", character, nullptr);
    for (int d = 0; d < numDests(); ++d)
    {
        juce::ValueTree m ("Map");
        m.setProperty ("id", dests()[d].paramID, nullptr);
        m.setProperty ("on", map[d].on, nullptr);
        m.setProperty ("mode", map[d].mode, nullptr);
        m.setProperty ("a", map[d].a, nullptr);
        m.setProperty ("b", map[d].b, nullptr);
        m.setProperty ("curve", map[d].curve, nullptr);
        c.appendChild (m, nullptr);
    }
}

void FxPresetManager::readFromState()
{
    auto c = proc.apvts.state.getChildWithName (kind == Delay ? dlyMacroType : revMacroType);
    if (! c.isValid()) { selectMappingOf (factory (kind)[0].name); return; }
    currentName = c.getProperty ("preset", factory (kind)[0].name).toString();
    currentIndex = indexOf (currentName);
    P::FxMapEntry m[P::maxFxMapDests];
    readMaps (c, dests(), numDests(), m);
    setMapping (m, juce::jlimit (0, P::numRevCharacters - 1, (int) c.getProperty ("character", P::RHall)));
}
