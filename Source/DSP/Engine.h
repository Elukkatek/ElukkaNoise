#pragma once
#include <JuceHeader.h>
#include "../Params.h"
#include "Noise.h"
#include "Filter.h"
#include "LFO.h"
#include "Gate.h"
#include "Delay.h"
#include "Reverb.h"
#include "Compressor.h"

struct Transport
{
    double bpm = 120.0, ppq = 0.0;
    bool playing = false;
};

// The whole instrument: one noise voice (noise -> HP -> filter -> filter 2 -> envelope, gate, width, pan) into delay and reverb.
//
// Modulation runs at control rate (blocks of up to 16 samples): the 16 matrix slots each take a source (smoothed
// MACRO, LFO 1 / 2, S&H, velocity), bend it by their curve, scale it by their amount and an optional aux source, and add
// it to their destination in that parameter's normalised space. The resulting cutoff, resonance, pan, gain, ... are ramped per sample
// inside the control block, so nothing in the signal path ever steps. The delay and reverb are outside the matrix:
// each effect preset's own macro mapping (P::FxMapLive) moves their knobs, with the same normalised-space model.
class NoiseEngine
{
public:
    void attach (juce::AudioProcessorValueTreeState& apvts);
    void attachFxMaps (const P::FxMapLive& dly, const P::FxMapLive& rev) { dlyMap = &dly; revMap = &rev; }
    void prepare (double sampleRate);
    void reset();

    void noteOn (int note, float velocity);
    void noteOff (int note);
    void allNotesOff();
    void allSoundOff();   // MIDI panic: the voice and every tail stop at once
    bool anyNoteHeld() const { return numHeld > 0; }

    // Renders n samples into L/R (replacing). t.ppq is the song position at the first sample.
    void render (float* L, float* R, int n, const Transport& t);

    // True when the voice has faded out and the effects tails are silent.
    bool isSilent() const { return voiceIdle && ! delay.isActive() && ! reverb.isActive(); }

    // Live values for the UI (written by the audio thread)
    std::atomic<float> uiMacro { 0.0f }, uiCutoff { 0.0f }, uiPan { 0.0f }, uiLevel { 0.0f }, uiGate { 1.0f },
                       uiLfo1 { 0.0f }, uiLfo2 { 0.0f }, uiEnv { 0.0f }, uiWidth { 1.0f };
    std::atomic<bool> uiNoteOn { false };
    std::atomic<int> uiGateStep { 0 }, uiGateRandomBits { 0xffff };
    std::atomic<float> uiDest[P::NumDst] {};   // every destination where the modulation has it now (normalised)
    std::atomic<float> uiDlyMap[P::numDlyMapDests] {}, uiRevMap[P::numRevMapDests] {};   // the effects' knobs after their macro mapping (normalised)
    std::atomic<float> uiLfoPhase[2] {}, uiSh { 0.0f };   // LFO phases (0..1) and the S&H output, for the displays
    std::atomic<float> uiCompIn { -100.0f }, uiCompOut { -100.0f }, uiCompGr { 0.0f }, uiCompLevel { -100.0f };   // compressor meter (dB, last block)

    // Seeds for repeatable offline renders (tests)
    void seed (juce::uint32 s) { seedBase = s; reseedAll(); }

    // Current smoothed macro (0..1)
    float currentMacro() const { return macroSm; }

private:
    void control (int n, const Transport& t);
    void renderVoice (float* L, float* R, int n);
    void reseedAll();
    float norm (int d);

    struct Raw
    {
        std::atomic<float> *macro {}, *noiseStereo {}, *noiseVary {}, *fltType {}, *fltSlope {}, *flt2Type {}, *flt2Slope {};
        std::atomic<float> *lWave[2] {}, *lSync[2] {}, *lDiv[2] {}, *lPhase[2] {}, *lRetrig[2] {}, *lSmooth[2] {};
        std::atomic<float> *shSync {}, *shDiv {}, *shSmooth {}, *shRetrig {};
        std::atomic<float> *gateOn {}, *gateRetrig {}, *gateDiv {}, *gateDuty {}, *gateAtt {}, *gateRel {}, *gateHold {}, *gateDecay {},
                           *gateSus {}, *gateSwing {}, *gateLen {}, *gateRandom {}, *gateRateBase {};
        std::atomic<float>* gateSteps[P::numGateSteps] {};
        std::atomic<float> *dlyOn {}, *dlySync {}, *dlyMode {}, *dlyTime {}, *dlyDiv {};
        std::atomic<float> *revOn {};
        std::atomic<float> *dlyBase[P::numDlyMapDests] {}, *revBase[P::numRevMapDests] {};   // the effects' mapped knobs
        std::atomic<float> *envAttack {}, *envRelease {}, *velSens {}, *retrig {};
        std::atomic<float> *fxOrder {}, *macroLo {}, *macroHi {}, *compMakeup {}, *compOn {};
        std::atomic<float>* base[P::NumDst] {};
        std::atomic<float> *mSrc[P::numMatrixSlots] {}, *mCrv[P::numMatrixSlots] {}, *mAmt[P::numMatrixSlots] {},
                           *mDst[P::numMatrixSlots] {}, *mAux[P::numMatrixSlots] {};
    } raw;
    juce::NormalisableRange<float> ranges[P::NumDst];
    juce::NormalisableRange<float> dlyRanges[P::numDlyMapDests], revRanges[P::numRevMapDests];
    const P::FxMapLive* dlyMap = nullptr;   // each effect preset's macro mapping (owned by the processor)
    const P::FxMapLive* revMap = nullptr;

    double sr = 44100.0;
    NoiseGen noise;
    NoiseFilter filter, filter2;   // filter 2 (v0.9.21): a second SVF after the main one
    HighPass12 hp;
    LFO lfo[2], sh;
    TranceGate gate;
    StereoDelay delay;
    NoiseReverb reverb;
    Compressor comp;
    Compressor::Params compP;
    juce::uint32 seedBase = 1;
    double gateOrigin = 0.0;            // song position of the last note (gate note retrigger)
    bool gateOriginPending = false;

    // notes
    int held[128] {};
    int numHeld = 0;
    float velocity = 1.0f;

    // control-rate state
    float macroSm = 0.0f;
    float baseSm[P::NumDst] {};
    float off[P::NumDst] {}, resolved[P::NumDst] {};
    float lastRaw[P::NumDst] {}, lastNorm[P::NumDst] {};
    float fxLastRaw[P::maxFxMapDests * 2] {}, fxLastNorm[P::maxFxMapDests * 2] {}, fxSm[P::maxFxMapDests * 2] {};   // delay 0..9, reverb 10..19
    float lfoVal[2] {}, shVal = 0.0f;
    bool primed = false;
    float typeMix[3] { 1.0f, 0.0f, 0.0f }, slopeMix = 1.0f;
    float typeMix2[3] { 1.0f, 0.0f, 0.0f }, slopeMix2 = 0.0f;
    float velSm = 1.0f;

    // per-sample ramped values: current and per-sample increment
    struct Ramp { float cur = 0.0f, inc = 0.0f; void to (float target, int n) { inc = (target - cur) / (float) n; } };
    Ramp rG, rK, rG2, rK2, rF2, rDrive2, rHpG, rDrive, rColour, rLL, rLR, rRL, rRR, rWidth, rAmp, rShared, rOwn;
    bool hpActive = false, f2Active = false;   // filter 2 is skipped at its default (open low pass): bit-transparent, no CPU
    float f2Mix = 0.0f;                         // filter 2 crossfaded in / out of the path (~10 ms) around its default

    // envelope
    float env = 0.0f, envTarget = 0.0f, attA = 0.01f, relA = 0.01f;
    bool voiceIdle = true;

    // effects (block-level)
    float dlyWetSm = 0.0f, revWetSm = 0.0f;
    bool dlyWasRunning = false, revWasRunning = false;
    int activeOrder = 0;      // 0 = delay then reverb, 1 = reverb then delay
    float fxFade = 1.0f;      // both mixes dip to 0 while the order swaps (identical output there: no click)
    juce::uint8 gateStepState[P::numGateSteps] {};
    StereoDelay::Params dlyP;
    NoiseReverb::Params revP;
    bool dlyRun = false, revRun = false;
    float levelPeak = 0.0f;
};
