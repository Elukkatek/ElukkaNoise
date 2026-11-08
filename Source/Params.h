#pragma once
#include <JuceHeader.h>

// Parameter IDs, choice lists and the modulation destination table.
//
// Every sound setting is a host parameter (AudioProcessorValueTreeState). A preset is a set of parameter values,
// including the macro map (how far and with what curve the MACRO knob pushes each destination) and the LFO / S&H
// routing slots, so presets, host automation and project state all go through the same mechanism.
namespace P
{
inline const juce::StringArray filterTypes  { "Low Pass", "Band Pass", "High Pass" };
enum FilterType { FLowPass, FBandPass, FHighPass };
inline const juce::StringArray filterSlopes { "24 dB", "12 dB" };

inline const juce::StringArray lfoWaves { "Sine", "Triangle", "Saw Up", "Saw Down", "Square", "S&H", "Smooth Random" };
enum LfoWave { WSine, WTriangle, WSawUp, WSawDown, WSquare, WSampleHold, WSmoothRandom };

// Synced rates for the LFOs and S&H, slowest to fastest (dotted and triplet values sit between the straight ones).
inline const juce::StringArray syncRateNames { "8 Bars", "4 Bars", "2 Bars D", "2 Bars", "1 Bar D", "1 Bar", "1/2 D", "1/2", "1/4 D", "1/2 T",
                                               "1/4", "1/8 D", "1/4 T", "1/8", "1/16 D", "1/8 T", "1/16", "1/32 D", "1/16 T", "1/32" };
inline constexpr float syncRateBeats[] { 32.0f, 16.0f, 12.0f, 8.0f, 6.0f, 4.0f, 3.0f, 2.0f, 1.5f, 4.0f / 3.0f,
                                         1.0f, 0.75f, 2.0f / 3.0f, 0.5f, 0.375f, 1.0f / 3.0f, 0.25f, 0.1875f, 1.0f / 6.0f, 0.125f };
constexpr int numSyncRates = 20;

// Trance gate step lengths
inline const juce::StringArray gateDivNames { "1/2", "1/4", "1/4 T", "1/8", "1/8 T", "1/16", "1/16 T", "1/32", "1/64" };
inline constexpr float gateDivBeats[] { 2.0f, 1.0f, 2.0f / 3.0f, 0.5f, 1.0f / 3.0f, 0.25f, 1.0f / 6.0f, 0.125f, 0.0625f };
// Effects order: the six orders of compressor (0), delay (1) and reverb (2)
constexpr int numFx = 3;
inline constexpr int fxOrders[6][3] { { 0, 1, 2 }, { 0, 2, 1 }, { 1, 2, 0 }, { 2, 1, 0 }, { 1, 0, 2 }, { 2, 0, 1 } };
inline juce::StringArray fxOrderNames()
{
    const char* n[3] { "Comp", "Delay", "Reverb" };
    juce::StringArray a;
    for (auto& o : fxOrders) a.add (juce::String (n[o[0]]) + " > " + n[o[1]] + " > " + n[o[2]]);
    return a;
}
inline int fxOrderIndex (const int* order)
{
    for (int i = 0; i < 6; ++i)
        if (fxOrders[i][0] == order[0] && fxOrders[i][1] == order[1] && fxOrders[i][2] == order[2]) return i;
    return 0;
}

// Trance gate sequencer: 16 steps, each Off, On (opens the gate with a fresh envelope) or Tie (continues the step
// before it without a new attack).
constexpr int numGateSteps = 16;
inline const juce::StringArray gateStepStates { "Off", "On", "Tie" };
enum GateStep { GOff, GOn, GTie };
inline juce::String gateStep (int i) { return "gate_s" + juce::String (i + 1); }
// Pattern library for the sequencer's Patterns menu (bit 0 = first step)
struct GatePattern { const char* name; juce::uint16 bits; };
inline constexpr GatePattern gatePatterns[] { { "Straight", 0xffff }, { "Offbeat", 0xaaaa }, { "Long Short", 0xdddd }, { "Short Long", 0xbbbb },
                                              { "Broken", 0xb5ad }, { "Stutter", 0xf3f1 }, { "Three-Three-Two", 0x4949 }, { "Eighths", 0x5555 },
                                              { "Quarter Pulse", 0x1111 }, { "Psy Roll", 0x7777 } };

// Synced delay times, longest to shortest
// Delay divisions (v0.9.19: in time order, with 1/1 and 1/16 D; the old dly_div list is mapped on load)
inline const juce::StringArray dlyDivisionNames { "1/1", "1/2", "1/4 D", "1/4", "1/8 D", "1/4 T", "1/8", "1/16 D", "1/8 T", "1/16", "1/16 T", "1/32" };
inline constexpr float dlyDivisionBeats[] { 4.0f, 2.0f, 1.5f, 1.0f, 0.75f, 2.0f / 3.0f, 0.5f, 0.375f, 1.0f / 3.0f, 0.25f, 1.0f / 6.0f, 0.125f };
constexpr int numDlyDivisions = 12;
inline constexpr int oldDlyDivToDivision[10] { 1, 2, 3, 4, 5, 6, 8, 9, 10, 11 };
inline const juce::StringArray dlyModeNames { "Mono", "Stereo", "Ping-Pong", "Wide Ping-Pong" };
enum DlyMode { DmMono, DmStereo, DmPingPong, DmWidePingPong };
// Delay characters (the menu in the delay panel): a set of knob values. Saved as an index: only ever append.
// The delay characters of v0.9.19 (the dly_char parameter), kept to turn an old state's choice into a delay preset
inline const juce::StringArray dlyCharacterNames { "Clean Echo", "Rhythmic", "Ping-Pong", "Wide Atmosphere", "Dark Feedback", "Modulated", "Psy Feedback", "Industrial" };

// Modulation destinations: every continuous knob of the instrument. Modulation is added in the parameter's normalised
// 0..1 space (so a log-scaled cutoff moves in octaves), clipped, then converted back to the real value. Amp has no
// parameter: it is a gain of 1 at rest. Saved presets store the destination as an index into this table: only ever
// append to it. (v0.9.20: the delay and reverb left the matrix; each effect preset carries its own macro mapping, see
// FxMapEntry below. Old indices are converted by migrateMatrixDests.)
struct DestInfo { const char* name; const char* paramID; };
enum Dst
{
    DCutoff, DHp, DRes, DDrive, DColour,
    DLfo1Rate, DLfo1Depth, DLfo2Rate, DLfo2Depth, DShRate, DShDepth, DGateDepth, DGateRate,
    DPan, DLevel, DAmp,   // (Width merged into Noise Stereo in v0.9.23)
    DNoiseStereo, DLfo1Smooth, DLfo2Smooth, DShSmooth,
    DGateLen, DGateAtt, DGateHold, DGateDecay, DGateSus, DGateRel, DGateSwing,
    DEnvAttack, DEnvRelease, DCompAmt, DCompSpeed,
    DCutoff2, DRes2,   // v0.9.21: the second filter
    DDrive2,           // v0.9.22
    NumDst
};
inline constexpr DestInfo dests[NumDst] =
{
    { "Filter Cutoff", "flt_cutoff" }, { "High Pass", "hp_cutoff" }, { "Resonance", "flt_res" }, { "Drive", "flt_drive" },
    { "Noise Colour", "noise_colour" },
    { "LFO 1 Rate", "l1_rate" }, { "LFO 1 Depth", "l1_depth" }, { "LFO 2 Rate", "l2_rate" }, { "LFO 2 Depth", "l2_depth" },
    { "S&H Rate", "sh_rate" }, { "S&H Depth", "sh_depth" }, { "Gate Depth", "gate_depth" }, { "Gate Rate", "gate_rate" },
    { "Pan", "pan" }, { "Level", "out_level" }, { "Amp", "" },
    { "Noise Stereo", "noise_stereo" }, { "LFO 1 Smooth", "l1_smooth" }, { "LFO 2 Smooth", "l2_smooth" }, { "S&H Smooth", "sh_smooth" },
    { "Gate Step Len", "gate_duty" }, { "Gate Attack", "gate_att" }, { "Gate Hold", "gate_hold" }, { "Gate Decay", "gate_decay" },
    { "Gate Sustain", "gate_sus" }, { "Gate Release", "gate_rel" }, { "Gate Swing", "gate_swing" },
    { "Attack", "env_attack" }, { "Release", "env_release" }, { "Comp Amount", "comp_amt" }, { "Comp Speed", "comp_speed" },
    { "Filter 2 Cutoff", "flt2_cutoff" }, { "Filter 2 Resonance", "flt2_res" }, { "Filter 2 Drive", "flt2_drive" }
};
// The table as it was up to v0.9.19 (52 entries, by parameter ID; "" = Amp): a saved slot's index into it
inline constexpr const char* oldDestParamIDs[52] =
{
    "flt_cutoff", "hp_cutoff", "flt_res", "flt_drive", "noise_colour",
    "l1_rate", "l1_depth", "l2_rate", "l2_depth", "sh_rate", "sh_depth", "gate_depth", "gate_rate",
    "pan", "width", "dly_wet", "dly_fb", "dly_time", "rev_wet", "rev_size", "out_level", "",
    "noise_stereo", "l1_smooth", "l2_smooth", "sh_smooth",
    "gate_duty", "gate_att", "gate_hold", "gate_decay", "gate_sus", "gate_rel", "gate_swing",
    "dly_filter", "dly_width", "rev_decay", "rev_lp", "rev_pre", "env_attack", "env_release",
    "rev_hp", "comp_amt", "comp_speed",
    "rev_width", "rev_mod", "rev_mod_rate", "rev_duck",
    "dly_offset", "dly_mod", "dly_mod_rate", "dly_duck", "dly_division"
};
// The table from v0.9.20 to v0.9.22 (35 entries; "" = Amp): Width was merged into Noise Stereo in v0.9.23
inline constexpr const char* oldDestParamIDs2[35] =
{
    "flt_cutoff", "hp_cutoff", "flt_res", "flt_drive", "noise_colour",
    "l1_rate", "l1_depth", "l2_rate", "l2_depth", "sh_rate", "sh_depth", "gate_depth", "gate_rate",
    "pan", "width", "out_level", "",
    "noise_stereo", "l1_smooth", "l2_smooth", "sh_smooth",
    "gate_duty", "gate_att", "gate_hold", "gate_decay", "gate_sus", "gate_rel", "gate_swing",
    "env_attack", "env_release", "comp_amt", "comp_speed",
    "flt2_cutoff", "flt2_res", "flt2_drive"
};
inline const char* destGroup (int d)
{
    switch (d)
    {
        case DCutoff: case DHp: case DRes: case DDrive: case DCutoff2: case DRes2: case DDrive2: return "Filter";
        case DColour: case DNoiseStereo: return "Noise";
        case DLfo1Rate: case DLfo1Depth: case DLfo2Rate: case DLfo2Depth: case DLfo1Smooth: case DLfo2Smooth: return "LFOs";
        case DShRate: case DShDepth: case DShSmooth: return "Random";
        case DGateDepth: case DGateRate: case DGateLen: case DGateAtt: case DGateHold: case DGateDecay: case DGateSus: case DGateRel: case DGateSwing: return "Gate";
        case DCompAmt: case DCompSpeed: return "Compressor";
        default: return "Output";
    }
}
inline int destForParam (const juce::String& id)
{
    for (int d = 0; d < NumDst; ++d)
        if (id == dests[d].paramID) return d;
    return -1;
}

// Modulation sources. All are unipolar (0..1, as in ElukkaSynth): they push a knob up from its value by up to the
// amount (down for a negative amount); LFOs and S&H after their Depth. Saved presets store the source as an index:
// only ever append.
inline const juce::StringArray modSources { "None", "Macro", "LFO 1", "LFO 2", "S&H", "Velocity" };
enum Src { SNone, SMacro, SLfo1, SLfo2, SSH, SVel, NumSrc };

// Reverb characters: a set of internals (NoiseReverb::internals). Since v0.9.20 a reverb preset picks one (there is no
// menu for it); the names are those of the v0.9.17 rev_char parameter, kept to turn an old state's choice into a preset.
inline const juce::StringArray revCharacterNames { "Tight Space", "Clean Hall", "Endless", "Dark Bloom", "Bright Space", "Modulated", "Psy Space" };
enum RevCharacter { RTight, RHall, REndless, RDarkBloom, RBright, RModulated, RPsy, numRevCharacters };

// Source response: curve < 0 bends towards early movement, > 0 towards late; 0 is linear.
inline float shapeMacro (float m, float curve)
{
    m = juce::jlimit (0.0f, 1.0f, m);
    if (curve > 0.001f)  return std::pow (m, 1.0f + 3.0f * curve);
    if (curve < -0.001f) return 1.0f - std::pow (1.0f - m, 1.0f - 3.0f * curve);
    return m;
}

// Effect macro mapping (v0.9.20). The delay and the reverb each carry, as part of their own preset, how the shared
// MACRO moves each of their continuous knobs. Stored in the plugin state (not as host parameters) and evaluated in the
// engine at control rate in the knob's normalised 0..1 space, like the matrix:
//   Additive: the knob is the reference, the macro adds depth b (-1..1) shaped by the curve; Range: the knob is
//   ignored, the macro travels from a to b (0..1). Either way the result is clipped to the knob's range, so repeated
//   sweeps never drift and the stored knob is never overwritten.
struct FxDest { const char* name; const char* paramID; };
inline constexpr FxDest dlyMapDests[] = { { "Mix", "dly_wet" }, { "Feedback", "dly_fb" }, { "Filter", "dly_filter" }, { "Width", "dly_width" },
                                          { "Offset", "dly_offset" }, { "Mod", "dly_mod" }, { "Duck", "dly_duck" } };
inline constexpr FxDest revMapDests[] = { { "Mix", "rev_wet" }, { "Decay", "rev_decay" }, { "Size", "rev_size" }, { "Tone", "rev_tone" },
                                          { "Width", "rev_width" }, { "Mod", "rev_mod" }, { "Pre-Delay", "rev_pre" }, { "Duck", "rev_duck" } };
constexpr int numDlyMapDests = 7, numRevMapDests = 8, maxFxMapDests = 8;   // (v0.9.23: Mod Rate gone, Low Pass / High Pass -> Tone)
enum DlyMapDest { MdMix, MdFb, MdFilter, MdWidth, MdOffset, MdMod, MdDuck };
enum RevMapDest { MrMix, MrDecay, MrSize, MrTone, MrWidth, MrMod, MrPre, MrDuck };
enum FxMapMode { MapAdd, MapRange };
struct FxMapEntry
{
    bool on = false;
    int mode = MapAdd;
    float a = 0.0f, b = 0.0f;   // Range: start, end (normalised). Additive: b = depth (-1..1), a unused
    float curve = 0.0f;         // response (shapeMacro)
};
inline float applyFxMap (const FxMapEntry& e, float baseNorm, float macro)
{
    if (! e.on) return baseNorm;
    const float m = shapeMacro (macro, e.curve);
    return juce::jlimit (0.0f, 1.0f, e.mode == MapRange ? e.a + (e.b - e.a) * m : baseNorm + e.b * m);
}
// the normalised range a mapping can move its knob over (for the knob's ring)
inline juce::Range<float> fxMapReach (const FxMapEntry& e, float baseNorm)
{
    if (! e.on) return { baseNorm, baseNorm };
    if (e.mode == MapRange) return { juce::jmin (e.a, e.b), juce::jmax (e.a, e.b) };
    const float far = juce::jlimit (0.0f, 1.0f, baseNorm + e.b);
    return { juce::jmin (baseNorm, far), juce::jmax (baseNorm, far) };
}
// The mapping as the audio thread reads it (the preset manager writes it on the message thread). A field at a time;
// a preset change is rare and every effect parameter glides, so a block with a half-new mapping cannot click.
struct FxMapLive
{
    std::atomic<float> on[maxFxMapDests] {}, mode[maxFxMapDests] {}, a[maxFxMapDests] {}, b[maxFxMapDests] {}, curve[maxFxMapDests] {};
    std::atomic<int> character { RHall };   // reverb only: the internals set
    void set (int d, const FxMapEntry& e)
    {
        on[d] = e.on ? 1.0f : 0.0f; mode[d] = (float) e.mode; a[d] = e.a; b[d] = e.b; curve[d] = e.curve;
    }
    FxMapEntry get (int d) const { return { on[d].load() > 0.5f, mode[d].load() > 0.5f ? MapRange : MapAdd, a[d].load(), b[d].load(), curve[d].load() }; }
    float apply (int d, float baseNorm, float macro) const { return applyFxMap (get (d), baseNorm, macro); }
};

// The modulation matrix: each slot routes a source through a curve and an amount to a destination; an optional aux
// source scales the amount (0..1), e.g. the macro deciding how far an LFO moves the filter.
constexpr int numMatrixSlots = 16;
inline juce::String mx (int slot, const char* what) { return "m" + juce::String (slot + 1) + "_" + what; }
inline juce::StringArray destChoices()
{
    juce::StringArray a { "None" };
    for (auto& d : dests) a.add (d.name);
    return a;   // choice index = destination + 1
}

inline juce::NormalisableRange<float> logRange (float lo, float hi)
{
    juce::NormalisableRange<float> r (lo, hi);
    r.setSkewForCentre (std::sqrt (lo * hi));
    return r;
}

// v0.9.19: the delay's dly_div (old list), dly_pp and dly_lp / dly_hp became dly_division, dly_mode and dly_filter.
// Applied to a saved state or preset tree (PARAM children with id / value) before it is loaded.
inline void migrateOldDelayParams (juce::ValueTree& params)
{
    auto find = [&params] (const juce::String& id) -> juce::ValueTree
    {
        for (const auto& c : params) if (c.hasType ("PARAM") && c["id"].toString() == id) return c;
        return {};
    };
    auto add = [&params] (const juce::String& id, float v)
    {
        juce::ValueTree c ("PARAM");
        c.setProperty ("id", id, nullptr);
        c.setProperty ("value", v, nullptr);
        params.appendChild (c, nullptr);
    };
    if (auto d = find ("dly_div"); d.isValid() && ! find ("dly_division").isValid())
        add ("dly_division", (float) oldDlyDivToDivision[juce::jlimit (0, 9, (int) (float) d["value"])]);
    if (auto pp = find ("dly_pp"); pp.isValid() && ! find ("dly_mode").isValid())
        add ("dly_mode", (float) pp["value"] > 0.5f ? (float) DmPingPong : (float) DmStereo);
    auto lp = find ("dly_lp"), hp = find ("dly_hp");
    if ((lp.isValid() || hp.isValid()) && ! find ("dly_filter").isValid())
    {
        const float lpHz = lp.isValid() ? (float) lp["value"] : 20000.0f, hpHz = hp.isValid() ? (float) hp["value"] : 100.0f;
        float f = 0.0f;
        if (lpHz < 15000.0f) f = -juce::jlimit (0.0f, 1.0f, std::log (20000.0f / lpHz) / std::log (20000.0f / 350.0f));
        else if (hpHz > 120.0f) f = juce::jlimit (0.0f, 1.0f, std::log (hpHz / 100.0f) / std::log (50.0f));
        add ("dly_filter", f);
    }
}

// v0.9.20: the delay and reverb destinations left the matrix and the table shrank. Converts every slot's saved
// destination (an index into the old table) to the new index; a slot that pointed at an effect becomes empty.
// Re-indexes every saved m<k>_dst from an old destination table (ids by index) into a new one; an id the new table
// does not have empties the slot. "width" (gone in v0.9.23) continues as "noise_stereo", which now covers it.
inline void migrateMatrixDests (juce::ValueTree& params, const char* const* oldTable, int oldCount, const char* const* newTable, int newCount)
{
    auto indexIn = [newTable, newCount] (juce::String id)
    {
        if (id == "width") id = "noise_stereo";
        for (int d = 0; d < newCount; ++d) if (id == newTable[d]) return d;
        return -1;
    };
    for (auto c : params)
    {
        if (! c.hasType ("PARAM")) continue;
        const auto id = c["id"].toString();
        if (! id.startsWith ("m") || ! id.endsWith ("_dst")) continue;
        const int old = juce::roundToInt ((float) c["value"]) - 1;
        if (old < 0 || old >= oldCount) { c.setProperty ("value", 0.0f, nullptr); continue; }
        const int d = indexIn (oldTable[old]);
        c.setProperty ("value", d >= 0 ? (float) (d + 1) : 0.0f, nullptr);
    }
}
// the current table's ids, for the migrations
inline const char* const* currentDestParamIDs()
{
    static const char* ids[NumDst];
    for (int d = 0; d < NumDst; ++d) ids[d] = dests[d].paramID;
    return ids;
}
// before v0.9.20 (52 entries) -> the v0.9.20 .. v0.9.22 table
inline void migrateMatrixDestsV2 (juce::ValueTree& params) { migrateMatrixDests (params, oldDestParamIDs, 52, oldDestParamIDs2, 35); }
// v0.9.20 .. v0.9.22 (35 entries) -> now
inline void migrateMatrixDestsV3 (juce::ValueTree& params) { migrateMatrixDests (params, oldDestParamIDs2, 35, currentDestParamIDs(), NumDst); }

// v0.9.23: one Tone knob for the reverb, like the delay's Filter. Left: the low pass closes (20 kHz -> 500 Hz, the
// tail darker); right: the high pass rises (80 Hz -> 2 kHz, the tail thinner). 0 is neutral.
inline float reverbToneLp (float t) { return t < 0.0f ? 20000.0f * std::pow (500.0f / 20000.0f, juce::jmin (1.0f, -t)) : 20000.0f; }
inline float reverbToneHp (float t) { return t > 0.0f ? 80.0f * std::pow (2000.0f / 80.0f, juce::jmin (1.0f, t)) : 80.0f; }
// the Tone that comes closest to an old Low Pass / High Pass pair (the low pass wins when both are set)
inline float reverbToneFor (float lpHz, float hpHz)
{
    if (lpHz < 19000.0f) return juce::jlimit (-1.0f, 0.0f, -std::log (20000.0f / juce::jmax (500.0f, lpHz)) / std::log (40.0f));
    if (hpHz > 100.0f) return juce::jlimit (0.0f, 1.0f, std::log (hpHz / 80.0f) / std::log (25.0f));
    return 0.0f;
}
// v0.9.23: the delay's and reverb's modulation rate follows the depth (0.3 Hz at 0, 0.53 Hz at the reverb's default 25 %
// as before, 3 Hz at full): a slow wobble at low depth, a swirl at high depth
inline float modRateFor (float depth) { return 0.3f * std::pow (10.0f, juce::jlimit (0.0f, 1.0f, depth)); }

// v0.9.23: Width merged into Noise Stereo (0..2: 0 = mono, 1 = independent left / right noise, 2 = extra wide), and
// the reverb's Low Pass / High Pass into Tone. Mod Rate is gone (derived from Mod). Applied to any PARAMS tree
// (project state, instrument preset, effect preset) saved before.
inline void migrateStereoAndTone (juce::ValueTree& params)
{
    auto find = [&params] (const juce::String& id) -> juce::ValueTree
    {
        for (const auto& c : params) if (c.hasType ("PARAM") && c["id"].toString() == id) return c;
        return {};
    };
    auto add = [&params] (const juce::String& id, float v)
    {
        juce::ValueTree c ("PARAM");
        c.setProperty ("id", id, nullptr);
        c.setProperty ("value", v, nullptr);
        params.appendChild (c, nullptr);
    };
    if (auto w = find ("width"); w.isValid())
    {
        const float width = (float) w["value"];
        auto s = find ("noise_stereo");
        const float stereo = s.isValid() ? (float) s["value"] : 1.0f;
        const float combined = width <= 1.0f ? stereo * width : (stereo >= 0.999f ? width : stereo);
        if (s.isValid()) s.setProperty ("value", combined, nullptr); else add ("noise_stereo", combined);
        params.removeChild (w, nullptr);
    }
    auto lp = find ("rev_lp"), hp = find ("rev_hp");
    if ((lp.isValid() || hp.isValid()) && ! find ("rev_tone").isValid())
    {
        add ("rev_tone", reverbToneFor (lp.isValid() ? (float) lp["value"] : 1200.0f, hp.isValid() ? (float) hp["value"] : 110.0f));
        if (lp.isValid()) params.removeChild (lp, nullptr);
        if (hp.isValid()) params.removeChild (hp, nullptr);
    }
    for (auto* id : { "dly_mod_rate", "rev_mod_rate" })
        if (auto c = find (id); c.isValid()) params.removeChild (c, nullptr);
}

inline juce::AudioProcessorValueTreeState::ParameterLayout createLayout()
{
    using namespace juce;
    std::vector<std::unique_ptr<RangedAudioParameter>> ps;
    int version = 1;
    // How a value is shown and how typed text is read back: typing reads what the knob shows, so a percentage knob
    // takes "50" or "50%" (not 0.5), a frequency takes "2k" or "2 kHz", a time "1.5 s" or "1500 ms", pan "30L" / "C".
    struct Fmt { std::function<String (float, int)> toText; std::function<float (const String&)> fromText; };
    auto f = [&] (const String& id, const String& name, NormalisableRange<float> r, float def, const String& label = {}, const Fmt& fmt = {})
    {
        auto attr = AudioParameterFloatAttributes().withLabel (label);
        if (fmt.toText) attr = attr.withStringFromValueFunction (fmt.toText);
        if (fmt.fromText) attr = attr.withValueFromStringFunction (fmt.fromText);
        ps.push_back (std::make_unique<AudioParameterFloat> (ParameterID { id, version }, name, r, def, attr));
    };
    auto c = [&] (const String& id, const String& name, const StringArray& choices, int def)
    {
        ps.push_back (std::make_unique<AudioParameterChoice> (ParameterID { id, version }, name, choices, def));
    };
    auto b = [&] (const String& id, const String& name, bool def)
    {
        ps.push_back (std::make_unique<AudioParameterBool> (ParameterID { id, version }, name, def));
    };
    auto num = [] (const String& t) { return t.replaceCharacter (',', '.').retainCharacters ("-+0123456789.").getFloatValue(); };
    const Fmt pct  { [] (float v, int) { return String (roundToInt (v * 100.0f)) + "%"; }, [num] (const String& t) { return num (t) * 0.01f; } };
    const Fmt pctS { [] (float v, int) { return (v > 0.0f ? "+" : "") + String (roundToInt (v * 100.0f)) + "%"; }, [num] (const String& t) { return num (t) * 0.01f; } };
    const Fmt hz   { [] (float v, int) { return v < 100.0f ? String (v, 2) + " Hz" : v < 1000.0f ? String (v, 1) + " Hz" : String (v / 1000.0f, 2) + " kHz"; },
                     [num] (const String& t) { return t.containsIgnoreCase ("k") ? num (t) * 1000.0f : num (t); } };
    const Fmt ms   { [] (float v, int) { return v < 10.0f ? String (v, 2) + " ms" : v < 1000.0f ? String (v, 0) + " ms" : String (v / 1000.0f, 2) + " s"; },
                     [num] (const String& t) { return t.containsIgnoreCase ("ms") || ! t.containsIgnoreCase ("s") ? num (t) : num (t) * 1000.0f; } };
    const Fmt sec  { [] (float v, int) { return String (v, 1) + " s"; },
                     [num] (const String& t) { return t.containsIgnoreCase ("ms") ? num (t) * 0.001f : num (t); } };
    const Fmt db   { [] (float v, int) { return (v > 0.0f ? "+" : "") + String (v, 1) + " dB"; }, [num] (const String& t) { return num (t); } };
    const Fmt whole { [] (float v, int) { return String (roundToInt (v)) + "%"; }, [num] (const String& t) { return num (t); } };   // 0..100 values

    // The one that matters
    f ("macro", "MACRO", { 0.0f, 100.0f, 0.0f }, 0.0f, "%", whole);

    // Noise
    f ("noise_colour", "Noise Colour", { 0.0f, 1.0f }, 1.0f, "",
       Fmt { [] (float v, int) { return String (v < 0.25f ? "Brown " : v < 0.75f ? "Pink " : "White ") + String (roundToInt (v * 100.0f)) + "%"; },
             [num] (const String& t)
             {
                 if (t.containsAnyOf ("0123456789")) return num (t) * 0.01f;
                 return t.containsIgnoreCase ("brown") ? 0.0f : t.containsIgnoreCase ("pink") ? 0.5f : 1.0f;
             } });
    f ("noise_stereo", "Noise Stereo", { 0.0f, 2.0f }, 1.0f, "", pct);   // v0.9.23: 0 = mono, 100 % = independent L / R, 200 % = extra wide (was Stereo + Width)
    b ("noise_vary", "Noise Vary", false);

    // Filter
    c ("flt_type", "Filter Type", filterTypes, FLowPass);
    c ("flt_slope", "Filter Slope", filterSlopes, 0);
    f ("flt_cutoff", "Filter Cutoff", logRange (20.0f, 20000.0f), 150.0f, "Hz", hz);
    f ("flt_res", "Filter Resonance", { 0.0f, 1.0f }, 0.2f, "", pct);
    f ("flt_drive", "Filter Drive", { 0.0f, 1.0f }, 0.0f, "", pct);
    f ("hp_cutoff", "HP Cutoff", logRange (20.0f, 20000.0f), 20.0f, "Hz", hz);
    // Filter 2 (v0.9.21): a second SVF after the main filter, the same as the first (drive since v0.9.22). Open by default.
    c ("flt2_type", "Filter 2 Type", filterTypes, FLowPass);
    c ("flt2_slope", "Filter 2 Slope", filterSlopes, 1);
    f ("flt2_cutoff", "Filter 2 Cutoff", logRange (20.0f, 20000.0f), 20000.0f, "Hz", hz);
    f ("flt2_res", "Filter 2 Resonance", { 0.0f, 1.0f }, 0.0f, "", pct);
    f ("flt2_drive", "Filter 2 Drive", { 0.0f, 1.0f }, 0.0f, "", pct);   // v0.9.22

    // LFO 1 and 2
    for (int i = 1; i <= 2; ++i)
    {
        const String p = "l" + String (i) + "_", n = "LFO " + String (i) + " ";
        c (p + "wave", n + "Wave", lfoWaves, WSine);
        f (p + "rate", n + "Rate", logRange (0.02f, 50.0f), i == 1 ? 0.5f : 2.0f, "Hz", hz);
        b (p + "sync", n + "Sync", true);
        c (p + "div", n + "Sync Rate", syncRateNames, i == 1 ? 0 : 10);   // LFO 1: 8 bars, LFO 2: 1/4
        f (p + "phase", n + "Phase", { 0.0f, 1.0f }, 0.0f, "", pct);
        b (p + "retrig", n + "Retrigger", true);
        f (p + "depth", n + "Depth", { 0.0f, 1.0f }, 1.0f, "", pct);
        f (p + "smooth", n + "Smooth", { 0.0f, 200.0f }, 2.0f, "ms", ms);
    }

    // Sample & hold random
    f ("sh_rate", "S&H Rate", logRange (0.1f, 200.0f), 8.0f, "Hz", hz);
    b ("sh_sync", "S&H Sync", true);
    c ("sh_div", "S&H Sync Rate", syncRateNames, 16);
    f ("sh_depth", "S&H Depth", { 0.0f, 1.0f }, 1.0f, "", pct);
    f ("sh_smooth", "S&H Smooth", { 0.0f, 500.0f }, 50.0f, "ms", ms);
    b ("sh_retrig", "S&H Retrigger", true);

    // Trance gate: step sequencer with an attack / hold / decay / sustain / release envelope per step
    b ("gate_on", "Gate On", false);
    f ("gate_depth", "Gate Depth", { 0.0f, 1.0f }, 1.0f, "", pct);
    b ("gate_retrig", "Gate Note Retrigger", true);   // on: the pattern restarts at step 1 on every note
    c ("gate_div", "Gate Division", gateDivNames, 5);
    // Not a knob: the reference for "Gate Rate" modulation, which moves the synced division in whole octaves
    f ("gate_rate", "Gate Rate", logRange (0.5f, 60.0f), 8.0f, "Hz", hz);
    f ("gate_duty", "Gate Length", { 0.05f, 0.95f }, 0.5f, "", pct);
    f ("gate_att", "Gate Attack", logRange (0.5f, 100.0f), 3.0f, "ms", ms);
    f ("gate_hold", "Gate Hold", { 0.0f, 500.0f, 0.0f, 0.5f }, 0.0f, "ms", ms);
    f ("gate_decay", "Gate Decay", logRange (1.0f, 2000.0f), 80.0f, "ms", ms);
    f ("gate_sus", "Gate Sustain", { 0.0f, 1.0f }, 1.0f, "", pct);
    f ("gate_rel", "Gate Release", logRange (1.0f, 500.0f), 20.0f, "ms", ms);
    f ("gate_swing", "Gate Swing", { 50.0f, 75.0f }, 50.0f, "%", whole);
    ps.push_back (std::make_unique<AudioParameterInt> (ParameterID { "gate_len", version }, "Gate Length Steps", 1, numGateSteps, numGateSteps));
    b ("gate_random", "Gate Random Steps", false);
    for (int i = 0; i < numGateSteps; ++i) c (gateStep (i), "Gate Step " + String (i + 1), gateStepStates, GOn);

    // Delay (on by default at wet 0, so the main-screen DELAY knob always works)
    b ("dly_on", "Delay On", true);
    b ("dly_sync", "Delay Sync", true);
    c ("dly_division", "Delay Division", dlyDivisionNames, 4);   // 1/8 D
    f ("dly_time", "Delay Time", logRange (1.0f, 4000.0f), 375.0f, "ms", ms);
    f ("dly_fb", "Delay Feedback", { 0.0f, 1.0f }, 0.6f, "", pct);
    f ("dly_wet", "Delay Wet", { 0.0f, 1.0f }, 0.0f, "", pct);
    f ("dly_filter", "Delay Filter", { -1.0f, 1.0f }, 0.0f, "", pctS);
    f ("dly_width", "Delay Width", { 0.0f, 1.0f }, 1.0f, "", pct);
    f ("dly_offset", "Delay Offset", { -0.5f, 0.5f }, 0.0f, "", pctS);
    c ("dly_mode", "Delay Mode", dlyModeNames, DmPingPong);
    f ("dly_mod", "Delay Mod", { 0.0f, 1.0f }, 0.0f, "", pct);   // the rate follows the depth (v0.9.23, modRateFor)
    f ("dly_duck", "Delay Duck", { 0.0f, 1.0f }, 0.0f, "", pct);

    // Reverb (on by default at wet 0, so the main-screen REVERB knob always works)
    b ("rev_on", "Reverb On", true);
    f ("rev_size", "Reverb Size", { 0.0f, 1.0f }, 0.65f, "", pct);
    f ("rev_decay", "Reverb Decay", logRange (0.2f, 20.0f), 3.0f, "s", sec);
    f ("rev_tone", "Reverb Tone", { -1.0f, 1.0f }, -0.75f, "", pctS);   // v0.9.23: one knob (was Low Pass 1.2 kHz / High Pass 110 Hz); -75 % = the old default
    f ("rev_wet", "Reverb Wet", { 0.0f, 1.0f }, 0.0f, "", pct);
    f ("rev_pre", "Reverb Pre-Delay", { 0.0f, 200.0f }, 10.0f, "ms", ms);
    f ("rev_width", "Reverb Width", { 0.0f, 1.0f }, 1.0f, "", pct);
    f ("rev_mod", "Reverb Mod", { 0.0f, 1.0f }, 0.25f, "", pct);   // the rate follows the depth (v0.9.23)
    f ("rev_duck", "Reverb Duck", { 0.0f, 1.0f }, 0.0f, "", pct);

    // Compressor
    b ("comp_on", "Compressor On", true);   // off: glides to unity, then the compressor is skipped
    f ("comp_amt", "Compressor Amount", { 0.0f, 1.0f }, 0.0f, "", pct);
    f ("comp_speed", "Compressor Speed", { 0.0f, 1.0f }, 0.5f, "", pct);
    b ("comp_makeup", "Compressor Makeup", true);

    // Effects order (the panel on top in EFFECTS goes first; drag the panels to reorder). The first two keep the
    // meaning they had before the compressor existed.
    c ("fx_order", "Effects Order", fxOrderNames(), 0);

    // Macro range: the macro's 0..100 % plays this part of its travel (floor .. ceiling), so a DAW automation from
    // 0 to 100 % can skip a dead start or end
    f ("macro_lo", "Macro Floor", { 0.0f, 100.0f }, 0.0f, "%", whole);
    f ("macro_hi", "Macro Ceiling", { 0.0f, 100.0f }, 100.0f, "%", whole);

    // Output, envelope, notes
    f ("out_level", "Output Level", { -24.0f, 24.0f, 0.0f }, 0.0f, "dB", db);
    f ("pan", "Pan", { -1.0f, 1.0f }, 0.0f, "",
       Fmt { [] (float v, int) { return std::abs (v) < 0.005f ? String ("C") : v < 0.0f ? String (roundToInt (-v * 100.0f)) + "L" : String (roundToInt (v * 100.0f)) + "R"; },
             [num] (const String& t)
             {
                 const auto u = t.trim().toLowerCase();
                 if (u == "c" || u.startsWith ("cen")) return 0.0f;
                 const float x = std::abs (num (u)) * 0.01f;
                 if (u.containsChar ('l')) return -x;
                 if (u.containsChar ('r')) return x;
                 return num (u) * 0.01f;
             } });
    f ("env_attack", "Attack", logRange (0.5f, 2000.0f), 5.0f, "ms", ms);
    f ("env_release", "Release", logRange (5.0f, 8000.0f), 60.0f, "ms", ms);
    f ("vel_sens", "Velocity", { 0.0f, 1.0f }, 0.0f, "", pct);
    b ("retrig", "Retrigger Modulation", true);

    // Modulation matrix
    for (int i = 0; i < numMatrixSlots; ++i)
    {
        const String n = "Mod " + String (i + 1) + " ";
        c (mx (i, "src"), n + "Source", modSources, SNone);
        f (mx (i, "crv"), n + "Curve", { -1.0f, 1.0f }, 0.0f, "", pctS);
        f (mx (i, "amt"), n + "Amount", { -1.0f, 1.0f }, 0.0f, "", pctS);
        c (mx (i, "dst"), n + "Destination", destChoices(), 0);
        c (mx (i, "aux"), n + "Aux Source", modSources, SNone);
    }
    return { ps.begin(), ps.end() };
}
}
