#include "Presets.h"
#include "PluginProcessor.h"

const juce::StringArray& PresetManager::categories()
{
    static const juce::StringArray cats { "Risers / Falls", "FX" };
    return cats;
}

juce::File PresetManager::overrideDir;

juce::File PresetManager::userPresetDir()
{
    if (overrideDir != juce::File()) return overrideDir;
   #if JUCE_MAC
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("Application Support/ElukkaNoise/Presets");
   #else
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("ElukkaNoise/Presets");
   #endif
}

// Modulation matrix helpers: each call takes the preset's next free slot. Sources are unipolar (0..1): a routing pushes
// the knob up from its value by up to the amount (down for a negative amount), so a swing is written as a push from a
// knob set to the bottom of the swing. Amounts are in the destination's normalised range (log knobs move in octaves).
static FactoryPreset& slot (FactoryPreset& p, int src, int dest, float amount, float curve = 0.0f, int aux = P::SNone)
{
    jassert (p.slots < P::numMatrixSlots);
    const int k = p.slots++;
    p.set (P::mx (k, "src"), (float) src).set (P::mx (k, "dst"), (float) (dest + 1)).set (P::mx (k, "amt"), amount);
    if (curve != 0.0f) p.set (P::mx (k, "crv"), curve);
    if (aux != P::SNone) p.set (P::mx (k, "aux"), (float) aux);
    return p;
}
static FactoryPreset& mac  (FactoryPreset& p, int dest, float amount, float curve = 0.0f) { return slot (p, P::SMacro, dest, amount, curve); }
static FactoryPreset& lfo1 (FactoryPreset& p, int dest, float amount, int aux = P::SNone) { return slot (p, P::SLfo1, dest, amount, 0.0f, aux); }
static FactoryPreset& lfo2 (FactoryPreset& p, int dest, float amount, int aux = P::SNone) { return slot (p, P::SLfo2, dest, amount, 0.0f, aux); }
static FactoryPreset& sh   (FactoryPreset& p, int dest, float amount, int aux = P::SNone) { return slot (p, P::SSH, dest, amount, 0.0f, aux); }
// the delay and reverb presets the instrument preset loads with (FxPresets.cpp)
static FactoryPreset& fx (FactoryPreset& p, const char* delay, const char* reverb) { p.delayPreset = delay; p.reverbPreset = reverb; return p; }

// Gate steps from a bit mask (bit 0 = step 1): set bits On, clear bits Off
static FactoryPreset& steps (FactoryPreset& p, juce::uint16 bits)
{
    for (int i = 0; i < P::numGateSteps; ++i) p.set (P::gateStep (i), ((bits >> i) & 1) != 0 ? (float) P::GOn : (float) P::GOff);
    return p;
}
// LFO / S&H setup in one call: wave, synced division (or a free rate in Hz), retrigger, smoothing
static FactoryPreset& lfo (FactoryPreset& p, int n, int wave, int div, float rateHz, bool retrig, float smoothMs, float depth = 1.0f)
{
    const juce::String pre = "l" + juce::String (n) + "_";
    p.set (pre + "wave", (float) wave).set (pre + "retrig", retrig ? 1.0f : 0.0f).set (pre + "smooth", smoothMs).set (pre + "depth", depth);
    if (div >= 0) p.set (pre + "sync", 1.0f).set (pre + "div", (float) div);
    else p.set (pre + "sync", 0.0f).set (pre + "rate", rateHz);
    return p;
}
static FactoryPreset& shSetup (FactoryPreset& p, int div, float smoothMs, bool retrig = false)
{
    return p.set ("sh_sync", 1.0f).set ("sh_div", (float) div).set ("sh_smooth", smoothMs).set ("sh_retrig", retrig ? 1.0f : 0.0f);
}
static FactoryPreset& gate (FactoryPreset& p, int div, float depth, float duty, float attMs, float relMs)
{
    return p.set ("gate_on", 1.0f).set ("gate_div", (float) div).set ("gate_depth", depth).set ("gate_duty", duty).set ("gate_att", attMs).set ("gate_rel", relMs);
}

// Sync divisions (P::syncRateNames): 1 = 4 bars, 3 = 2 bars, 5 = 1 bar, 7 = 1/2, 10 = 1/4, 13 = 1/8, 16 = 1/16, 19 = 1/32.
// Gate divisions (P::gateDivNames): 1 = 1/4, 3 = 1/8, 5 = 1/16, 7 = 1/32.
// Cutoff amounts: +0.333 per decade (150 Hz -> 15 kHz is +0.667); LFO rate: +0.294 per decade; level: +0.0208 per dB.
const std::vector<FactoryPreset>& PresetManager::factory()
{
    static const std::vector<FactoryPreset> list = []
    {
        using namespace P;
        std::vector<FactoryPreset> v;
        auto add = [&] (const char* name, const char* cat) -> FactoryPreset& { v.push_back ({ name, cat, {} }); return v.back(); };
        const char* RISE = "Risers / Falls";
        const char* FX = "FX";
        enum { S_4BARS = 1, S_2BARS = 3, S_1BAR = 5, S_HALF = 7, S_4TH = 10, S_8TH = 13, S_16TH = 16, S_32ND = 19 };
        enum { G_4TH = 1, G_8TH = 3, G_16TH = 5, G_32ND = 7 };

        {
            // Init: white noise with the lows taken out (high pass 500 Hz, v0.9.22). Filter wide open, no resonance or
            // drive, no gate, no modulation, delay and reverb off. A clean starting point (the macro does nothing until it
            // is mapped).
            auto& p = add ("Init", "Init");
            p.set ("noise_colour", 1.0f).set ("flt_type", (float) FLowPass).set ("flt_cutoff", 20000.0f).set ("flt_res", 0.0f)
             .set ("flt_drive", 0.0f).set ("hp_cutoff", 500.0f);
            fx (p, "Off", "Off");
        }

        // ---- Risers / Falls: the macro is the progression, 0 % -> 100 % a riser, 100 % -> 0 % a fall ------------
        {
            // The reference riser: a 24 dB low-pass sweep from a dark rumble to bright, controlled full-band noise.
            // Level comes down 6 dB on the way up so the top is not painfully louder than the bottom. Steady 1/8 D
            // echoes and a clean hall arrive late in the rise.
            auto& p = add ("Clean Noise Rise", RISE);
            p.set ("flt_cutoff", 110.0f).set ("flt_res", 0.18f).set ("out_level", 6.9f).set ("env_release", 90.0f);
            mac (p, DCutoff, 0.597f, -0.04f);   // tops out at ~14.5 kHz exactly at 100 %
            mac (p, DLevel, -0.128f);
            fx (p, "Clean Rhythmic", "Clean Space");
        }
        {
            // Restrained movement for long transitions: pink-white noise, a gentle sweep that never gets harsh (Filter 2
            // caps the top at 6 kHz, opening to 12 kHz), width growing late. Tight room, steady echoes.
            auto& p = add ("Subtle Tension", RISE);
            p.set ("noise_colour", 0.7f).set ("flt_cutoff", 300.0f).set ("flt_res", 0.1f).set ("flt2_cutoff", 6000.0f).set ("noise_stereo", 0.6f)
             .set ("out_level", 4.0f).set ("env_release", 300.0f);
            mac (p, DCutoff, 0.35f, 0.2f);
            mac (p, DCutoff2, 0.1f, 0.5f);
            mac (p, DNoiseStereo, 0.2f, 0.3f);
            mac (p, DLevel, -0.04f);
            fx (p, "Clean Rhythmic", "Tight Space");
        }
        {
            // Brown noise that turns pink and white as it opens into a wide space: the colour, the cutoff and the width
            // all rise, a 2-bar pan drift keeps it alive. Half-note wide echoes, an endless tail.
            auto& p = add ("Deep Atmosphere", RISE);
            p.set ("noise_colour", 0.15f).set ("flt_cutoff", 200.0f).set ("flt_res", 0.15f).set ("noise_stereo", 0.5f).set ("pan", -0.3f)
             .set ("out_level", -2.0f).set ("env_release", 600.0f);
            lfo (p, 1, WSine, S_2BARS, 0.0f, false, 20.0f);
            lfo1 (p, DPan, 0.3f);
            mac (p, DColour, 0.5f, 0.2f);
            mac (p, DCutoff, 0.5f, 0.1f);
            mac (p, DNoiseStereo, 0.5f, 0.2f);
            fx (p, "Atmospheric", "Endless Space");
        }
        {
            // Bright, dense white noise rising to an intense peak: the low pass opens fully, the lows thin out late, drive
            // and compression pack it, the level rises a little instead of falling. No delay, a bright hall.
            auto& p = add ("Whiteout", RISE);
            p.set ("flt_cutoff", 1000.0f).set ("flt_res", 0.1f).set ("hp_cutoff", 60.0f).set ("flt_drive", 0.1f).set ("out_level", 0.0f)
             .set ("comp_amt", 0.35f).set ("comp_speed", 0.5f).set ("env_release", 120.0f);
            mac (p, DCutoff, 0.43f, -0.1f);
            mac (p, DHp, 0.3f, 0.6f);
            mac (p, DDrive, 0.4f);
            fx (p, "Off", "Bright Hall");
        }
        {
            // Slow stereo movement that turns into a fast spin as the filter opens. The delay stays a plain 1/8 D echo on
            // purpose: the spin is the instrument's, the echoes just follow it.
            auto& p = add ("Spinning Rise", RISE);
            p.set ("flt_cutoff", 150.0f).set ("flt_res", 0.25f).set ("noise_stereo", 0.3f).set ("pan", -0.45f).set ("out_level", 8.1f);
            lfo (p, 1, WSine, -1, 0.3f, true, 5.0f);
            lfo1 (p, DPan, 0.45f);
            mac (p, DCutoff, 0.6f, -0.05f);
            mac (p, DLfo1Rate, 0.42f, 0.2f);
            mac (p, DLevel, -0.12f);
            fx (p, "Clean Rhythmic", "Endless Space");
        }
        {
            // A filter LFO that speeds up and deepens as the macro rises (0.5 Hz at 25 % depth -> 11 Hz at full depth)
            // over a slow 1-bar pan. Wide ping-pong echoes, a clean hall.
            auto& p = add ("Spiral", RISE);
            p.set ("flt_cutoff", 250.0f).set ("flt_res", 0.4f).set ("pan", -0.3f).set ("out_level", 4.0f).set ("env_release", 200.0f);
            lfo (p, 1, WSine, -1, 0.5f, true, 3.0f, 0.25f);
            lfo (p, 2, WSine, S_1BAR, 0.0f, false, 10.0f);
            lfo1 (p, DCutoff, 0.3f);
            lfo2 (p, DPan, 0.3f);
            mac (p, DLfo1Depth, 0.75f);
            mac (p, DLfo1Rate, 0.4f, 0.3f);
            mac (p, DCutoff, 0.4f);
            mac (p, DLevel, -0.1f);
            fx (p, "Wide Ping-Pong", "Clean Space");
        }
        {
            // Strong stereo movement: a pan sweep that accelerates (0.5 -> 7 Hz) while a 1-bar triangle breathes the
            // width, the filter opening underneath. No delay (the stereo picture is the instrument's), a bright hall.
            auto& p = add ("Wide Rotation", RISE);
            p.set ("flt_cutoff", 220.0f).set ("flt_res", 0.2f).set ("noise_stereo", 0.4f).set ("pan", -0.5f).set ("out_level", 5.5f).set ("env_release", 200.0f);
            lfo (p, 1, WSine, -1, 0.5f, true, 5.0f);
            lfo (p, 2, WTriangle, S_1BAR, 0.0f, false, 10.0f);
            lfo1 (p, DPan, 0.5f);
            lfo2 (p, DNoiseStereo, 0.25f);
            mac (p, DCutoff, 0.58f, -0.05f);
            mac (p, DNoiseStereo, 0.3f, 0.2f);
            mac (p, DLfo1Rate, 0.35f, 0.3f);
            mac (p, DLevel, -0.16f);
            fx (p, "Off", "Bright Hall");
        }
        {
            // Two LFOs winding around each other: a triangle on the resonant cutoff and a slow sine on the pan, both
            // speeding up with the macro, into a modulated stereo echo. The reverb stays small so the helix stays clear.
            auto& p = add ("Helix", RISE);
            p.set ("flt_cutoff", 300.0f).set ("flt_res", 0.45f).set ("pan", -0.4f).set ("out_level", 4.0f).set ("env_release", 250.0f);
            lfo (p, 1, WTriangle, -1, 0.8f, true, 4.0f);
            lfo (p, 2, WSine, -1, 0.23f, true, 8.0f);
            lfo1 (p, DCutoff, 0.2f);
            lfo2 (p, DPan, 0.4f);
            mac (p, DCutoff, 0.45f, 0.1f);
            mac (p, DLfo1Rate, 0.3f, 0.3f);
            mac (p, DLfo2Rate, 0.3f);
            mac (p, DLevel, -0.1f);
            fx (p, "Modulated Echo", "Tight Space");
        }
        {
            // Dense, resonant, driven: a 24 dB low pass with the resonance and drive climbing late, the lows thinning out,
            // compression holding it together. Dark industrial echoes, a dark ducked bloom.
            auto& p = add ("Industrial Pressure", RISE);
            p.set ("noise_colour", 0.8f).set ("flt_cutoff", 120.0f).set ("flt_res", 0.55f).set ("flt_drive", 0.35f).set ("hp_cutoff", 40.0f)
             .set ("out_level", 4.0f).set ("comp_amt", 0.4f).set ("comp_speed", 0.6f).set ("env_release", 150.0f);
            mac (p, DCutoff, 0.6f, 0.05f);
            mac (p, DRes, 0.2f, 0.4f);
            mac (p, DDrive, 0.4f);
            mac (p, DHp, 0.25f, 0.7f);
            mac (p, DLevel, -0.1f);
            fx (p, "Dark Industrial", "Dark Bloom");
        }
        {
            // A square-wave filter chop that gets faster, deeper, more resonant and more saturated as the macro rises,
            // until the machine runs away. Dark feedback echoes, a tight room.
            auto& p = add ("Machine Overload", RISE);
            p.set ("flt_cutoff", 400.0f).set ("flt_res", 0.5f).set ("flt_drive", 0.2f).set ("out_level", 2.0f)
             .set ("comp_amt", 0.5f).set ("comp_speed", 0.7f).set ("env_release", 150.0f);
            lfo (p, 1, WSquare, -1, 3.0f, true, 3.0f, 0.3f);
            lfo1 (p, DCutoff, 0.15f);
            mac (p, DLfo1Depth, 0.7f);
            mac (p, DLfo1Rate, 0.2f, 0.4f);
            mac (p, DDrive, 0.6f, 0.2f);
            mac (p, DRes, 0.3f, 0.5f);
            mac (p, DCutoff, 0.35f);
            mac (p, DLevel, -0.08f);
            fx (p, "Dark Feedback", "Tight Space");
        }
        {
            // Acid: a screaming 12 dB band pass plucked by a 1/16 saw, the band lifting through the rise while the
            // resonance eases off near the top and the drive comes in. 1/16 psy echoes, a clean hall.
            auto& p = add ("Acidic Rise", RISE);
            p.set ("flt_type", (float) FBandPass).set ("flt_slope", 1.0f).set ("flt_cutoff", 250.0f).set ("flt_res", 0.85f).set ("flt_drive", 0.2f)
             .set ("out_level", 9.5f).set ("env_release", 150.0f);
            lfo (p, 1, WSawDown, S_16TH, 0.0f, true, 1.0f);
            lfo1 (p, DCutoff, 0.3f);
            mac (p, DCutoff, 0.45f);
            mac (p, DRes, -0.15f, 0.5f);
            mac (p, DDrive, 0.3f);
            mac (p, DLevel, -0.02f);
            fx (p, "Psy Feedback", "Clean Space");
        }
        {
            // Techno build-up: a synced gate that doubles its speed as the macro rises (1/8 from 0 %, 1/16 from 33 %,
            // 1/32 from 69 %) and opens from a soft pulse into hard chops, while the filter opens and the lows thin out.
            // Ducked 1/8 echoes fill the gaps, a ducked bloom grows between the chops.
            auto& p = add ("Rolling Gate Rise", RISE);
            p.set ("flt_cutoff", 160.0f).set ("flt_res", 0.25f).set ("out_level", 8.3f).set ("env_release", 150.0f).set ("gate_rate", 4.0f);
            gate (p, G_8TH, 0.35f, 0.55f, 1.0f, 12.0f);
            mac (p, DCutoff, 0.6f, -0.04f);
            mac (p, DGateRate, 0.298f, 0.18f);
            mac (p, DGateDepth, 0.6f, -0.2f);
            mac (p, DHp, 0.4f, 0.6f);   // lows thin out late in the rise (up to ~220 Hz)
            mac (p, DLevel, -0.1f);
            fx (p, "Ducked Gate Echo", "Ducked Bloom");
        }
        {
            // Pink noise breathing with a 4-bar sine, opening slowly into a wide wash. The effects carry this one: half-note
            // wide echoes and an endless space, both growing with the macro.
            auto& p = add ("Endless Horizon", RISE);
            p.set ("noise_colour", 0.4f).set ("flt_cutoff", 250.0f).set ("flt_res", 0.1f).set ("noise_stereo", 0.8f).set ("out_level", -1.0f)
             .set ("env_release", 800.0f);
            lfo (p, 1, WSine, S_4BARS, 0.0f, false, 30.0f);
            lfo1 (p, DCutoff, 0.08f);
            mac (p, DCutoff, 0.5f, 0.15f);
            mac (p, DNoiseStereo, 0.2f);
            mac (p, DLevel, 0.02f);
            fx (p, "Atmospheric", "Endless Space");
        }
        {
            // A 1/16 sample & hold stepping the cutoff more and more widely as the macro rises (it is the aux source),
            // over a stereo sine that speeds up. Psy echoes; the reverb stays a clean hall so the steps stay sharp.
            auto& p = add ("Psychedelic Ascent", RISE);
            p.set ("flt_cutoff", 350.0f).set ("flt_res", 0.4f).set ("noise_stereo", 0.6f).set ("pan", -0.35f).set ("out_level", 4.0f).set ("env_release", 300.0f);
            shSetup (p, S_16TH, 60.0f);
            lfo (p, 2, WSine, -1, 0.4f, true, 6.0f);
            sh (p, DCutoff, 0.3f, SMacro);
            lfo2 (p, DPan, 0.35f);
            mac (p, DLfo2Rate, 0.3f, 0.3f);
            mac (p, DCutoff, 0.45f);
            mac (p, DLevel, -0.08f);
            fx (p, "Psy Feedback", "Clean Space");
        }
        {
            // For automation from 100 % down to 0 %: bright and thin at the top, then the lows return and a resonant
            // peak forms as it falls. The Fall Echo and Fall Wash presets grow their tails as the macro falls, so the
            // space takes over as the dry noise darkens.
            auto& p = add ("Blackout Fall", RISE);
            p.set ("flt_slope", 1.0f).set ("flt_cutoff", 140.0f).set ("flt_res", 0.5f).set ("out_level", 7.6f).set ("env_release", 400.0f);
            mac (p, DCutoff, 0.58f, -0.05f);
            mac (p, DRes, -0.25f, 0.3f);
            mac (p, DHp, 0.2f, 0.5f);
            mac (p, DLevel, -0.2f);
            fx (p, "Fall Echo", "Fall Wash");
        }
        {
            // A stepped spin (saw-down pan) that slows down and darkens as the macro falls, landing in a wash of reverb
            // that grows on the way down. No delay: the spin stays legible.
            auto& p = add ("Reverse Spin Fall", RISE);
            p.set ("flt_slope", 1.0f).set ("flt_cutoff", 160.0f).set ("flt_res", 0.3f).set ("noise_stereo", 0.25f).set ("pan", -0.45f).set ("out_level", 6.3f).set ("env_release", 400.0f);
            lfo (p, 1, WSawDown, -1, 0.5f, true, 12.0f);
            lfo1 (p, DPan, 0.45f);
            mac (p, DCutoff, 0.64f, -0.05f);
            mac (p, DLfo1Rate, 0.4f, 0.3f);
            mac (p, DLevel, -0.2f);
            fx (p, "Off", "Fall Wash");
        }

        // ---- FX: rhythmic, percussive, experimental; the macro is the intensity ------------------------------------
        {
            // Classic 1/16 trance gate on low-passed noise: the macro deepens the gate and brightens the noise. Steady
            // 1/8 D echoes and a dark ducked bloom between the pulses.
            auto& p = add ("Trance Pulse", FX);
            p.set ("flt_cutoff", 400.0f).set ("flt_res", 0.3f).set ("out_level", 8.0f);
            gate (p, G_16TH, 0.5f, 0.5f, 1.0f, 15.0f);
            mac (p, DGateDepth, 0.5f);
            mac (p, DCutoff, 0.45f);
            mac (p, DLevel, -0.06f);
            fx (p, "Clean Rhythmic", "Dark Bloom");
        }
        {
            // Rapid 1/32 chops with two rests per half beat; the macro hardens the gate, adds resonance and drive.
            // Ducked 1/8 echoes in the rests, a tight room.
            auto& p = add ("Industrial Stutter", FX);
            p.set ("noise_colour", 0.9f).set ("flt_cutoff", 500.0f).set ("flt_res", 0.4f).set ("flt_drive", 0.3f).set ("out_level", 3.0f)
             .set ("comp_amt", 0.4f).set ("comp_speed", 0.7f);
            gate (p, G_32ND, 0.6f, 0.5f, 0.5f, 6.0f);
            steps (p, 0xf7f7);
            mac (p, DGateDepth, 0.4f);
            mac (p, DRes, 0.35f, 0.3f);
            mac (p, DDrive, 0.5f);
            mac (p, DCutoff, 0.35f);
            mac (p, DLevel, -0.06f);
            fx (p, "Ducked Gate Echo", "Tight Space");
        }
        {
            // An irregular 1/16 pattern with swing under a 1/8 stepped-random filter: a machine with a loose bearing.
            // The macro opens the filter and deepens the gate. Dark feedback echoes, a tight room.
            auto& p = add ("Broken Machine", FX);
            p.set ("flt_cutoff", 350.0f).set ("flt_res", 0.4f).set ("out_level", 3.5f).set ("gate_swing", 58.0f);
            gate (p, G_16TH, 0.3f, 0.5f, 1.5f, 25.0f);
            steps (p, 0xb5ad);
            lfo (p, 1, WSampleHold, S_8TH, 0.0f, true, 6.0f);
            lfo1 (p, DCutoff, 0.3f);
            mac (p, DCutoff, 0.5f);
            mac (p, DGateDepth, 0.7f, 0.2f);
            mac (p, DLevel, -0.02f);
            fx (p, "Dark Feedback", "Tight Space");
        }
        {
            // A 1/8 gate whose pulses alternate sides (a 1/4 square on the pan), wide; the macro brightens, deepens
            // and widens it. No delay (it would blur the sides), a clean hall.
            auto& p = add ("Wide Gate", FX);
            p.set ("flt_cutoff", 500.0f).set ("flt_res", 0.25f).set ("noise_stereo", 1.2f).set ("pan", -0.6f).set ("out_level", 6.0f);
            gate (p, G_8TH, 0.7f, 0.5f, 1.0f, 20.0f);
            lfo (p, 1, WSquare, S_4TH, 0.0f, true, 8.0f);
            lfo1 (p, DPan, 0.6f);
            mac (p, DCutoff, 0.45f);
            mac (p, DGateDepth, 0.3f);
            mac (p, DNoiseStereo, 0.2f);
            mac (p, DLevel, -0.16f);
            fx (p, "Off", "Clean Space");
        }
        {
            // Repetitive 1/8 pulses under a 1-bar filter sine and a 2-bar pan drift: the same thing, slowly changing.
            // The macro lifts the filter. Steady echoes, a ducked bloom.
            auto& p = add ("Hypnotic Pulse", FX);
            p.set ("noise_colour", 0.6f).set ("flt_cutoff", 300.0f).set ("flt_res", 0.3f).set ("noise_stereo", 0.6f).set ("pan", -0.35f).set ("out_level", 0.0f)
             .set ("env_release", 800.0f);
            gate (p, G_8TH, 0.6f, 0.5f, 2.0f, 30.0f);
            lfo (p, 1, WSine, S_1BAR, 0.0f, false, 10.0f);
            lfo (p, 2, WTriangle, S_2BARS, 0.0f, false, 20.0f);
            lfo1 (p, DCutoff, 0.2f);
            lfo2 (p, DPan, 0.35f);
            mac (p, DCutoff, 0.4f);
            mac (p, DLevel, 0.04f);
            fx (p, "Clean Rhythmic", "Ducked Bloom");
        }
        {
            // Sparse hits every three 1/16 steps (a dotted-eighth pulse) with the echoes filling the space between them:
            // the macro brings the echoes in and feeds them back, and blooms the ducked reverb.
            auto& p = add ("Echo Gate", FX);
            p.set ("flt_cutoff", 600.0f).set ("flt_res", 0.3f).set ("out_level", 10.0f);
            gate (p, G_16TH, 0.9f, 0.35f, 1.0f, 15.0f);
            steps (p, 0x1249);
            mac (p, DCutoff, 0.4f);
            mac (p, DLevel, -0.06f);
            fx (p, "Clean Rhythmic", "Ducked Bloom");
        }
        {
            // Slow, spacious gating: half-note swells (a 1/4 gate on every other step, 40 ms attack, 120 ms release)
            // of pink noise into an endless space. The macro opens and widens it.
            auto& p = add ("Spacious Gate", FX);
            p.set ("noise_colour", 0.5f).set ("flt_cutoff", 250.0f).set ("flt_res", 0.2f).set ("noise_stereo", 0.9f).set ("out_level", 3.0f)
             .set ("env_release", 600.0f);
            gate (p, G_4TH, 0.8f, 0.6f, 40.0f, 120.0f);
            steps (p, 0x5555);
            mac (p, DCutoff, 0.45f, 0.1f);
            mac (p, DNoiseStereo, 0.2f);
            mac (p, DLevel, -0.06f);
            fx (p, "Off", "Endless Space");
        }
        {
            // Acid: a screaming 12 dB band pass under a 1/16 saw and a 1/16 gate, the band lifting with the macro while
            // the resonance eases off. 1/16 psy echoes, a clean hall.
            auto& p = add ("Acid Noise", FX);
            p.set ("flt_type", (float) FBandPass).set ("flt_slope", 1.0f).set ("flt_cutoff", 250.0f).set ("flt_res", 0.85f).set ("out_level", 10.9f);
            lfo (p, 1, WSawDown, S_16TH, 0.0f, true, 1.0f);
            gate (p, G_16TH, 0.8f, 0.5f, 1.0f, 30.0f);
            lfo1 (p, DCutoff, 0.35f);
            mac (p, DCutoff, 0.4f);
            mac (p, DRes, -0.12f);
            fx (p, "Psy Feedback", "Clean Space");
        }
        {
            // Fast, aggressive sweeps: a 1/8 saw-up throws the resonant cutoff two decades every beat half, a 1/2 square
            // swaps sides; the macro adds resonance and drive. Dark industrial echoes, a tight room.
            auto& p = add ("Machine Sweep", FX);
            p.set ("flt_cutoff", 300.0f).set ("flt_res", 0.55f).set ("flt_drive", 0.3f).set ("pan", -0.3f).set ("out_level", -4.0f);
            lfo (p, 1, WSawUp, S_8TH, 0.0f, true, 2.0f);
            lfo (p, 2, WSquare, S_HALF, 0.0f, true, 8.0f);
            lfo1 (p, DCutoff, 0.45f);
            lfo2 (p, DPan, 0.3f);
            mac (p, DCutoff, 0.25f);
            mac (p, DRes, 0.25f, 0.4f);
            mac (p, DDrive, 0.4f);
            fx (p, "Dark Industrial", "Tight Space");
        }
        {
            // Psychedelic filter movement: a 1-bar triangle on the resonant cutoff that deepens with the macro, a stereo
            // sine that speeds up. Wide ping-pong echoes, a swirling psy space.
            auto& p = add ("Psy Filter", FX);
            p.set ("flt_cutoff", 400.0f).set ("flt_res", 0.5f).set ("noise_stereo", 0.5f).set ("pan", -0.45f).set ("out_level", 6.4f).set ("env_release", 300.0f);
            lfo (p, 1, WSine, -1, 1.0f, true, 4.0f);
            lfo (p, 2, WTriangle, S_1BAR, 0.0f, false, 10.0f, 0.4f);
            lfo1 (p, DPan, 0.45f);
            lfo2 (p, DCutoff, 0.25f);
            mac (p, DLfo2Depth, 0.6f);
            mac (p, DLfo1Rate, 0.5f, 0.3f);
            mac (p, DCutoff, 0.35f);
            mac (p, DLevel, -0.16f);
            fx (p, "Wide Ping-Pong", "Psy Atmosphere");
        }
        {
            // Rhythmic low-frequency wobble: a 1/4 sine swings a resonant low pass over a decade and a half; the macro
            // deepens the wobble and adds resonance. No delay, a clean hall.
            auto& p = add ("Wobble Noise", FX);
            p.set ("flt_cutoff", 120.0f).set ("flt_res", 0.6f).set ("out_level", 6.0f);
            lfo (p, 1, WSine, S_4TH, 0.0f, true, 3.0f, 0.5f);
            lfo1 (p, DCutoff, 0.5f);
            mac (p, DLfo1Depth, 0.5f);
            mac (p, DCutoff, 0.25f);
            mac (p, DRes, 0.15f, 0.5f);
            mac (p, DLevel, -0.14f);
            fx (p, "Off", "Clean Space");
        }
        {
            // A ringing, driven band pass shimmering with a 6 Hz sine (faster with the macro) over a slow pan. Steady
            // echoes and a bright hall keep the metal ringing.
            auto& p = add ("Metallic Motion", FX);
            p.set ("noise_colour", 0.6f).set ("flt_type", (float) FBandPass).set ("flt_cutoff", 1200.0f).set ("flt_res", 0.9f).set ("flt_drive", 0.7f)
             .set ("pan", -0.3f).set ("out_level", 6.7f);
            lfo (p, 1, WSine, -1, 6.0f, true, 2.0f);
            lfo (p, 2, WSine, -1, 0.7f, true, 5.0f);
            lfo1 (p, DCutoff, 0.12f);
            lfo2 (p, DPan, 0.3f);
            mac (p, DCutoff, 0.3f);
            mac (p, DLfo1Rate, 0.4f, 0.3f);
            mac (p, DLevel, 0.08f);
            fx (p, "Clean Rhythmic", "Bright Hall");
        }
        {
            // Stepped random filter: a 1/16 sample & hold on a resonant band pass under a 1/16 gate. The macro is the
            // S&H's aux source: at 0 % the band sits still, turning it up spreads the random steps wider.
            auto& p = add ("Random Circuit", FX);
            p.set ("flt_type", (float) FBandPass).set ("flt_cutoff", 900.0f).set ("flt_res", 0.6f).set ("flt_drive", 0.2f).set ("out_level", 10.0f);
            shSetup (p, S_16TH, 50.0f);
            gate (p, G_16TH, 0.85f, 0.5f, 1.0f, 18.0f);
            sh (p, DCutoff, 0.35f, SMacro);
            mac (p, DCutoff, 0.15f);
            mac (p, DLevel, -0.04f);
            fx (p, "Clean Rhythmic", "Tight Space");
        }
        {
            // Acid plucks: a 1/16 S&H on a screaming 12 dB band pass, gate steps On-Tie-On-Off with a short decay so
            // every step plucks. The macro lifts the band, widens the random steps (aux) and adds drive. Psy echoes and
            // a psy space, both swirling more with the macro.
            auto& p = add ("Psy Chaos", FX);
            p.set ("flt_type", (float) FBandPass).set ("flt_slope", 1.0f).set ("flt_cutoff", 500.0f).set ("flt_res", 0.85f).set ("flt_drive", 0.5f)
             .set ("out_level", 7.0f).set ("gate_decay", 60.0f).set ("gate_sus", 0.5f);
            shSetup (p, S_16TH, 50.0f);
            gate (p, G_16TH, 0.9f, 0.6f, 1.5f, 25.0f);
            for (int i = 0; i < P::numGateSteps; ++i)
                p.set (P::gateStep (i), (float) (i % 4 == 0 ? GOn : i % 4 == 1 ? GTie : i % 4 == 2 ? GOn : GOff));
            sh (p, DCutoff, 0.22f);
            sh (p, DCutoff, 0.2f, SMacro);
            mac (p, DCutoff, 0.25f);
            mac (p, DDrive, 0.3f);
            mac (p, DLevel, 0.04f);
            fx (p, "Psy Feedback", "Psy Atmosphere");
        }
        {
            // A 1/32 sample & hold throwing a band pass around and, scaled by the macro, each step to a random place in
            // the stereo field, under a swung 1/32 stutter gate. Dark feedback echoes, no reverb: the glitches stay dry.
            auto& p = add ("Glitch Storm", FX);
            p.set ("flt_type", (float) FBandPass).set ("flt_cutoff", 1200.0f).set ("flt_res", 0.5f).set ("noise_stereo", 1.3f).set ("out_level", 5.0f)
             .set ("gate_swing", 58.0f);
            shSetup (p, S_32ND, 10.0f);
            gate (p, G_32ND, 0.4f, 0.45f, 0.5f, 10.0f);
            steps (p, 0xf3f1);
            sh (p, DCutoff, 0.25f);
            sh (p, DPan, 0.5f, SMacro);
            mac (p, DGateDepth, 0.5f);
            mac (p, DCutoff, 0.28f);
            mac (p, DLevel, 0.05f);
            fx (p, "Dark Feedback", "Off");
        }
        {
            // Controlled randomness with a mechanical character: a 1/8 S&H with no smoothing steps the cutoff in hard
            // jumps under a Long-Short 1/16 gate; the macro widens the steps and deepens the gate. Ducked echoes, tight room.
            auto& p = add ("Digital Machinery", FX);
            p.set ("flt_cutoff", 800.0f).set ("flt_res", 0.45f).set ("flt_drive", 0.15f).set ("out_level", 1.0f);
            shSetup (p, S_8TH, 0.0f, true);
            gate (p, G_16TH, 0.7f, 0.5f, 0.5f, 8.0f);
            steps (p, 0xdddd);
            sh (p, DCutoff, 0.3f);
            sh (p, DCutoff, 0.2f, SMacro);
            mac (p, DCutoff, 0.3f);
            mac (p, DGateDepth, 0.3f);
            mac (p, DLevel, -0.06f);
            fx (p, "Ducked Gate Echo", "Tight Space");
        }
        return v;
    }();
    return list;
}

int PresetManager::indexOf (const juce::String& name, const juce::String& category, bool factoryOnly) const
{
    for (int i = 0; i < (int) entries.size(); ++i)
    {
        auto& e = entries[(size_t) i];
        if (e.name == name && (category.isEmpty() || e.category == category) && (! factoryOnly || e.isFactory)) return i;
    }
    for (int i = 0; i < (int) entries.size(); ++i)   // category unknown (older project): first name match
        if (entries[(size_t) i].name == name && (! factoryOnly || entries[(size_t) i].isFactory)) return i;
    return -1;
}

void PresetManager::refresh()
{
    entries.clear();
    const auto& f = factory();
    for (int i = 0; i < (int) f.size(); ++i)
        entries.push_back ({ f[(size_t) i].name, f[(size_t) i].category, true, i, {} });
    for (auto& cat : categories())
    {
        auto dir = userPresetDir().getChildFile (juce::File::createLegalFileName (cat));
        juce::Array<juce::File> files = dir.findChildFiles (juce::File::findFiles, false, juce::String ("*") + fileExtension);
        files.sort();
        for (auto& file : files)
            entries.push_back ({ file.getFileNameWithoutExtension(), cat, false, -1, file });
    }
    currentIndex = indexOf (currentName, currentCategory);
}

// what an instrument preset holds: everything but MACRO and the two effects
static bool isInstrumentParam (const juce::String& id)
{
    return id != "macro" && ! id.startsWith ("dly_") && ! id.startsWith ("rev_");
}

juce::ValueTree PresetManager::toTree (const juce::String& name, const juce::String& category) const
{
    juce::ValueTree t ("ElukkaNoisePreset");
    t.setProperty ("version", 4, nullptr);   // 2 = v0.9.20 (the matrix without the effects); 3 = v0.9.21 (effect preset names); 4 = v0.9.23 (Stereo + Width merged)
    t.setProperty ("name", name, nullptr);
    t.setProperty ("category", category, nullptr);
    t.setProperty ("delayPreset", proc.dlyPresets.currentName, nullptr);
    t.setProperty ("reverbPreset", proc.revPresets.currentName, nullptr);
    juce::ValueTree params ("PARAMS");
    for (auto* p : proc.getParameters())
        if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p))
            if (isInstrumentParam (rp->paramID))
            {
                juce::ValueTree v ("PARAM");
                v.setProperty ("id", rp->paramID, nullptr);
                v.setProperty ("value", rp->convertFrom0to1 (rp->getValue()), nullptr);
                params.appendChild (v, nullptr);
            }
    t.appendChild (params, nullptr);
    return t;
}

static void setParam (ElukkaNoiseProcessor& proc, const juce::String& id, float value)
{
    if (auto* p = proc.apvts.getParameter (id))
        p->setValueNotifyingHost (p->convertTo0to1 (value));
}

void PresetManager::applyFactory (const FactoryPreset& p)
{
    // defaults first, so a preset only has to list what it changes
    for (auto* prm : proc.getParameters())
        if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (prm))
            if (isInstrumentParam (rp->paramID)) rp->setValueNotifyingHost (rp->getDefaultValue());
    for (auto& [id, value] : p.values)
    {
        jassert (proc.apvts.getParameter (id) != nullptr && isInstrumentParam (id));
        setParam (proc, id, value);
    }
    applyFxRefs (p.delayPreset, p.reverbPreset);
}

void PresetManager::applyFxRefs (const juce::String& delayName, const juce::String& reverbName)
{
    if (delayName.isNotEmpty() && ! proc.dlyPresets.loadByName (delayName)) proc.dlyPresets.loadByName ("Clean Rhythmic");
    if (reverbName.isNotEmpty() && ! proc.revPresets.loadByName (reverbName)) proc.revPresets.loadByName ("Clean Space");
}

void PresetManager::applyTree (const juce::ValueTree& t)
{
    for (auto* prm : proc.getParameters())
        if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (prm))
            if (isInstrumentParam (rp->paramID)) rp->setValueNotifyingHost (rp->getDefaultValue());
    auto params = t.getChildWithName ("PARAMS");
    const int version = (int) t.getProperty ("version", 1);
    if (version < 2) P::migrateMatrixDestsV2 (params);   // presets saved before v0.9.20
    if (version < 4) { P::migrateMatrixDestsV3 (params); P::migrateStereoAndTone (params); }   // before v0.9.23
    for (const auto& v : params)
        if (v.hasType ("PARAM") && isInstrumentParam (v["id"].toString())) setParam (proc, v["id"].toString(), (float) v["value"]);
    applyFxRefs (t.getProperty ("delayPreset", "").toString(), t.getProperty ("reverbPreset", "").toString());
}

void PresetManager::load (int entryIndex)
{
    if (entryIndex < 0 || entryIndex >= (int) entries.size()) return;
    auto& e = entries[(size_t) entryIndex];
    if (e.isFactory)
        applyFactory (factory()[(size_t) e.index]);
    else if (auto xml = juce::XmlDocument::parse (e.file))
    {
        auto t = juce::ValueTree::fromXml (*xml);
        if (! t.hasType ("ElukkaNoisePreset")) return;
        applyTree (t);
    }
    else return;
    currentIndex = entryIndex;
    currentName = e.name;
    currentCategory = e.category;
    proc.bumpUiVersion();
}

bool PresetManager::loadByName (const juce::String& name, const juce::String& category)
{
    const int i = indexOf (name, category);
    if (i < 0) return false;
    load (i);
    return true;
}

bool PresetManager::save (const juce::String& nameIn, const juce::String& categoryIn)
{
    const auto name = legalName (nameIn);
    const auto category = categories().contains (categoryIn) ? categoryIn : categories()[0];
    if (name.isEmpty() || indexOf (name, category, true) >= 0) return false;   // a factory name would shadow the user's file
    auto dir = userPresetDir().getChildFile (juce::File::createLegalFileName (category));
    if (! dir.createDirectory()) return false;
    auto file = dir.getChildFile (juce::File::createLegalFileName (name) + fileExtension);
    auto t = toTree (name, category);
    if (auto xml = t.createXml())
        if (! xml->writeTo (file)) return false;
    refresh();
    currentName = name;
    currentCategory = category;
    currentIndex = indexOf (name, category);
    proc.bumpUiVersion();
    return true;
}

bool PresetManager::deletePreset (int entryIndex)
{
    if (entryIndex < 0 || entryIndex >= (int) entries.size()) return false;
    auto e = entries[(size_t) entryIndex];
    if (e.isFactory || ! e.file.existsAsFile()) return false;
    if (! e.file.moveToTrash()) return false;
    refresh();
    if (currentName == e.name && currentCategory == e.category) currentIndex = -1;
    proc.bumpUiVersion();
    return true;
}

bool PresetManager::rename (int entryIndex, const juce::String& newNameIn)
{
    const auto newName = legalName (newNameIn);
    if (entryIndex < 0 || entryIndex >= (int) entries.size() || newName.isEmpty()) return false;
    auto e = entries[(size_t) entryIndex];
    if (e.isFactory || ! e.file.existsAsFile() || indexOf (newName, e.category, true) >= 0) return false;
    auto target = e.file.getSiblingFile (juce::File::createLegalFileName (newName) + fileExtension);
    if (target.existsAsFile() && target != e.file) return false;
    // keep the name inside the file in step
    auto xml = juce::XmlDocument::parse (e.file);
    if (xml == nullptr) return false;
    xml->setAttribute ("name", newName);
    if (! xml->writeTo (target)) return false;
    if (target != e.file) e.file.deleteFile();
    const bool wasCurrent = currentName == e.name && currentCategory == e.category;
    refresh();
    if (wasCurrent) { currentName = newName; currentIndex = indexOf (newName, e.category); }
    proc.bumpUiVersion();
    return true;
}
