#pragma once
#include <JuceHeader.h>
#include "../Params.h"

// Trance gate: a 16-step sequencer (Off / On / Tie per step, 1..16 steps, swing), always in note values of the host
// tempo. With note retrigger on, step 1 starts on each note; off, the steps follow the song position. Each opened step runs an attack / hold / decay / sustain envelope; the gate closes after
// Length (a share of the step, unless the next step is tied) and the envelope releases. All segments are one-pole
// moves from wherever the envelope is, so retriggers and ties never jump.  gain = 1 - depth * (1 - envelope).
class TranceGate
{
public:
    struct Params
    {
        bool on = false, retrig = true, random = false;
        float depth = 1.0f, duty = 0.5f, attackMs = 3.0f, holdMs = 0.0f, decayMs = 80.0f, sustain = 1.0f, releaseMs = 20.0f;
        float swing = 50.0f;             // percent of a step pair given to the first step (50 = straight, 75 = hard shuffle)
        double stepBeats = 0.25;         // step length in beats
        double origin = 0.0;             // song position (beats) of the last note, for note retrigger
        int length = P::numGateSteps;
        const juce::uint8* steps = nullptr;   // numGateSteps states (P::GateStep)
        double bpm = 120.0, ppq = -1.0;  // ppq >= 0: host is playing (locked to the song)
    };

    void prepare (double sampleRate, juce::uint32 seed)
    {
        sr = sampleRate;
        rng.setSeed ((juce::int64) seed);
        env = 1.0f;
        stage = Decay;
        wasOpen = true;
        pos = 0.0;
        lastStep = -1;
        randomBits = 0xffff;
        current = 0;
    }

    void trigger (bool retrig)
    {
        if (retrig) { pos = 0.0; lastStep = -1; }
    }

    // Works out the step state for the coming numSamples and the per-sample envelope coefficients.
    void beginBlock (const Params& p, int numSamples)
    {
        // host playing: steps counted from the song position (or from the note, with retrigger); stopped: they run on at
        // the host tempo from the last trigger
        if (p.ppq >= 0.0) pos = (p.ppq - (p.retrig ? p.origin : 0.0)) / p.stepBeats;
        const double stepPos = pos;
        pos += (p.bpm / 60.0) / p.stepBeats * (double) numSamples / sr;
        if (pos > 1.0e9) pos -= 1.0e9;

        // swing: the first step of each pair gets `swing` percent of the pair
        const double s = juce::jlimit (0.5, 0.75, (double) p.swing * 0.01);
        const double pairPos = stepPos * 0.5, pair = std::floor (pairPos), f = pairPos - pair;
        long long stepIndex;
        double frac;
        if (f < s) { stepIndex = (long long) pair * 2; frac = f / s; }
        else       { stepIndex = (long long) pair * 2 + 1; frac = (f - s) / (1.0 - s); }

        const int len = juce::jlimit (1, P::numGateSteps, p.length);
        const int step = (int) (((stepIndex % len) + len) % len);
        const bool newStep = stepIndex != lastStep;
        if (newStep && p.random && step == 0) randomBits = (juce::uint16) ((rng.nextInt() & 0xffff) | 0x0001);
        lastStep = stepIndex;
        current = step;

        auto state = [&] (int i)
        {
            if (p.random) return ((randomBits >> i) & 1) != 0 ? (int) P::GOn : (int) P::GOff;
            return p.steps != nullptr ? (int) p.steps[i] : (int) P::GOn;
        };
        const int cur = state (step), next = state ((step + 1) % len);
        const bool open = cur != P::GOff && (frac < (double) p.duty || next == P::GTie);
        started = open && (! wasOpen || (newStep && cur == P::GOn));   // a tie continues without a new attack
        if (started) stage = Attack;
        else if (! open && wasOpen) stage = Release;
        wasOpen = open;

        auto coef = [this] (float ms) { return 1.0f - std::exp (-1.0f / (juce::jmax (0.1f, ms) * 0.001f * (float) sr)); };
        attA = coef (p.attackMs);
        decA = coef (p.decayMs);
        relA = coef (p.releaseMs);
        sus = juce::jlimit (0.0f, 1.0f, p.sustain);
        holdSamples = (int) (p.holdMs * 0.001f * (float) sr);
        // depth glides (~3 ms), so switching the gate on or off mid-step, or a depth jump, never steps the level
        depthTarget = p.on ? p.depth : 0.0f;
        depthA = 1.0f - std::exp (-1.0f / (0.003f * (float) sr));
    }

    inline float next() noexcept
    {
        switch (stage)
        {
            case Attack:
                env += (1.0f - env) * attA;
                if (env >= 0.995f) { stage = Hold; holdLeft = holdSamples; }
                break;
            case Hold:
                if (--holdLeft <= 0) stage = Decay;
                break;
            case Decay:   env += (sus - env) * decA; break;
            case Release: env += (0.0f - env) * relA; break;
        }
        depth += (depthTarget - depth) * depthA;
        return 1.0f - depth * (1.0f - env);
    }

    float envelope() const { return env; }
    int currentStep() const { return current; }
    bool stepStarted() const { return started; }   // a step opened at the start of this block (not a tie)
    juce::uint16 currentRandomBits() const { return randomBits; }

private:
    enum Stage { Attack, Hold, Decay, Release };
    double sr = 44100.0, pos = 0.0;
    long long lastStep = -1;
    Stage stage = Decay;
    bool wasOpen = true, started = false;
    float env = 1.0f, attA = 0.01f, decA = 0.01f, relA = 0.01f, sus = 1.0f, depth = 0.0f, depthTarget = 0.0f, depthA = 0.01f;
    int holdSamples = 0, holdLeft = 0, current = 0;
    juce::uint16 randomBits = 0xffff;
    juce::Random rng;
};
