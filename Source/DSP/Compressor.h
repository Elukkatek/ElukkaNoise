#pragma once
#include <JuceHeader.h>

// Two-knob compressor for noise. Stereo-linked RMS detection (a short window, so noise's peaky crest does not make
// the gain grainy), soft knee, attack / release in the dB domain.
//   Amount 0..1: threshold 0 .. -42 dB and ratio 1:1 .. 8:1 together (0 = off).
//   Speed 0..1: slow (30 ms attack, 400 ms release: smooth glue on sweeps) .. fast (1 ms / 30 ms: every trance-gate
//   pulse pumps on its own).
//   Makeup: predicts the gain reduction from the compressor's own curve at the material's average level (weighted
//   towards its loud parts, so gated noise is judged by its pulses) and adds it back, moving with the same attack and
//   release as the compression. Turning Amount up or down then keeps the loudness steady, with no catch-up.
class Compressor
{
public:
    struct Params { float amount = 0.0f, speed = 0.5f; bool makeup = true; };

    void prepare (double sampleRate)
    {
        sr = (float) sampleRate;
        reset();
    }
    void reset()
    {
        rms = 0.0f; grDb = 0.0f; wNum = wDen = 0.0f; levelW = -60.0f; staticGr = 0.0f; makeupDb = 0.0f; amountSm = 0.0f;
        uiIn = uiOut = -100.0f; uiGr = 0.0f;
    }

    // true while it changes the signal (amount up, or still gliding back to unity)
    bool isActive (const Params& p) const { return p.amount > 0.0f || amountSm > 1.0e-4f || grDb < -0.01f || makeupDb > 0.01f; }

    void process (float* L, float* R, int n, const Params& p)
    {
        auto coef = [this] (float ms) { return 1.0f - std::exp (-1.0f / (ms * 0.001f * sr)); };
        const float s = juce::jlimit (0.0f, 1.0f, p.speed);
        const float atkA = coef (30.0f * std::pow (1.0f / 30.0f, s));
        const float relA = coef (400.0f * std::pow (30.0f / 400.0f, s));
        const float rmsA = coef (20.0f * std::pow (3.0f / 20.0f, s));
        const float weightA = coef (300.0f), offA = coef (100.0f), amountA = coef (30.0f);
        constexpr float knee = 10.0f;
        float peakIn = 0.0f, peakOut = 0.0f, worstGr = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            amountSm += (juce::jlimit (0.0f, 1.0f, p.amount) - amountSm) * amountA;
            const float thr = -42.0f * amountSm, slope = 1.0f / (1.0f + 7.0f * amountSm) - 1.0f;   // slope = 1/ratio - 1
            const float l = L[i], r = R[i];
            const float x2 = 0.5f * (l * l + r * r);
            rms += (x2 - rms) * rmsA;
            const float levelDb = 10.0f * std::log10 (rms + 1.0e-12f);
            const float over = levelDb - thr;
            float target = 0.0f;   // gain change in dB (<= 0)
            if (2.0f * over > knee) target = slope * over;
            else if (2.0f * over > -knee) { const float t = over + 0.5f * knee; target = slope * t * t / (2.0f * knee); }
            grDb += (target - grDb) * (target < grDb ? atkA : relA);
            // makeup: the curve's gain change at the average level (weighted by the level, so loud parts count most;
            // held through silence), through the same attack / release as the compression itself
            wNum += (rms * levelDb - wNum) * weightA;
            wDen += (rms - wDen) * weightA;
            if (wDen > 1.0e-9f) levelW = wNum / wDen;
            const float predicted = curveDb (levelW, amountSm);
            staticGr += (predicted - staticGr) * (predicted < staticGr ? atkA : relA);
            makeupDb = p.makeup ? -staticGr : makeupDb + (0.0f - makeupDb) * offA;
            const float g = std::pow (10.0f, (grDb + makeupDb) * 0.05f);
            L[i] = l * g;
            R[i] = r * g;
            peakIn = juce::jmax (peakIn, std::abs (l), std::abs (r));
            peakOut = juce::jmax (peakOut, std::abs (L[i]), std::abs (R[i]));
            worstGr = juce::jmin (worstGr, grDb);
        }
        uiLevel = 10.0f * std::log10 (rms + 1.0e-12f);   // the detector's level (what the curve is read at)
        uiIn = juce::Decibels::gainToDecibels (peakIn, -100.0f);
        uiOut = juce::Decibels::gainToDecibels (peakOut, -100.0f);
        uiGr = worstGr;
    }

    // last block, for the meter
    float uiIn = -100.0f, uiOut = -100.0f, uiGr = 0.0f, uiLevel = -100.0f;
    // the static curve's gain change (dB, <= 0) at an input level, for an amount (the graph draws this)
    static float curveDb (float levelDb, float amount)
    {
        constexpr float knee = 10.0f;
        const float thr = -42.0f * amount, slope = 1.0f / (1.0f + 7.0f * amount) - 1.0f, over = levelDb - thr;
        if (2.0f * over > knee) return slope * over;
        if (2.0f * over > -knee) { const float t = over + 0.5f * knee; return slope * t * t / (2.0f * knee); }
        return 0.0f;
    }
    float currentMakeupDb() const { return makeupDb; }

private:
    float sr = 44100.0f, rms = 0.0f, grDb = 0.0f, amountSm = 0.0f;
    float wNum = 0.0f, wDen = 0.0f, levelW = -60.0f, staticGr = 0.0f, makeupDb = 0.0f;
};
