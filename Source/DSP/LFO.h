#pragma once
#include <JuceHeader.h>
#include "../Params.h"

// LFO / random source evaluated once per control block. Output is -1..1 (or 0..1 when unipolar), smoothed by a
// one-pole so phase resets and stepped waves never jump the destinations.
class LFO
{
public:
    struct Params
    {
        int wave = P::WSine;
        double rateHz = 1.0;
        float phaseStart = 0.0f;
        bool retrig = true, unipolar = false;
        float smoothMs = 2.0f;
        float jump = 0.0f;           // 0..1: push each new random value away from the previous one (the S&H uses 15 %)
        double lockedPhase = -1.0;   // >= 0: follow the host's song position (synced, free-running, host playing)
    };

    void prepare (double sampleRate, juce::uint32 seed)
    {
        sr = sampleRate;
        rng.setSeed ((juce::int64) seed);
        phase = 0.0;
        held = rng.nextFloat() * 2.0f - 1.0f;
        next = rng.nextFloat() * 2.0f - 1.0f;
        smoothed = 0.0f;
        snap = true;
    }

    void trigger (const Params& p)
    {
        if (! p.retrig) return;
        phase = (double) p.phaseStart;
        phase -= std::floor (phase);
        newRandom();
        snap = true;
    }

    // value for the coming block, then advance by numSamples
    float process (const Params& p, int numSamples)
    {
        jump = p.jump;
        if (p.lockedPhase >= 0.0)
        {
            if (p.lockedPhase < phase - 0.5) newRandom();   // wrapped round (or the playhead jumped back)
            phase = p.lockedPhase;
        }
        float v;
        const float ph = (float) phase;
        switch (p.wave)
        {
            case P::WTriangle:     v = ph < 0.5f ? 4.0f * ph - 1.0f : 3.0f - 4.0f * ph; break;
            case P::WSawUp:        v = 2.0f * ph - 1.0f; break;
            case P::WSawDown:      v = 1.0f - 2.0f * ph; break;
            case P::WSquare:       v = ph < 0.5f ? 1.0f : -1.0f; break;
            case P::WSampleHold:   v = held; break;
            case P::WSmoothRandom: { const float t = 0.5f - 0.5f * std::cos (ph * juce::MathConstants<float>::pi); v = held + (next - held) * t; break; }
            default:               v = std::sin (ph * juce::MathConstants<float>::twoPi); break;
        }
        if (p.unipolar) v = 0.5f * v + 0.5f;

        phase += p.rateHz * (double) numSamples / sr;
        if (phase >= 1.0)
        {
            phase -= std::floor (phase);
            newRandom();
        }
        if (snap || p.smoothMs <= 0.01f)
            smoothed = v;
        else
            smoothed += (v - smoothed) * (1.0f - std::exp (-(float) numSamples / (p.smoothMs * 0.001f * (float) sr)));
        snap = false;
        return smoothed;
    }

    double getPhase() const { return phase; }

    // Phase of a synced LFO locked to the song: bar 1 beat 1 is the start of the shape (plus the phase offset).
    static double songPhase (double ppq, double beats, float start)
    {
        const double x = ppq / juce::jmax (1.0e-6, beats) + (double) start;
        return x - std::floor (x);
    }

private:
    void newRandom()
    {
        held = jumpFrom (held, next);
        next = rng.nextFloat() * 2.0f - 1.0f;
    }

    // Jump: measure the new value as a fraction t of the room between the previous value and the edge on its
    // side, then bend t with 1 - (1 - t)^n. Small moves grow a lot, values already near the edge barely change.
    float jumpFrom (float prev, float r) const
    {
        if (jump <= 0.0f) return r;
        const float n = std::pow (8.0f, jump);   // exponent 1 .. 8 (15 % = 1.37)
        const float room = r > prev ? 1.0f - prev : prev + 1.0f;
        if (room <= 1.0e-6f) return r;
        const float t = std::abs (r - prev) / room;
        const float d = (1.0f - std::pow (1.0f - t, n)) * room;
        return r > prev ? prev + d : prev - d;
    }

    double sr = 44100.0, phase = 0.0;
    float held = 0.0f, next = 0.0f, smoothed = 0.0f, jump = 0.0f;
    bool snap = true;
    juce::Random rng;
};
