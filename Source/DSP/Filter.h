#pragma once
#include <JuceHeader.h>

// Stereo state-variable filter (Simper's topology-preserving form), 12 or 24 dB, low/band/high pass, with optional
// drive before the filter. All three responses come from the same state, so the type and slope can be crossfaded
// without any discontinuity; cutoff and resonance are set per sample by the engine (it ramps them).
class NoiseFilter
{
public:
    enum Type { LowPass, BandPass, HighPass };

    void prepare (double sampleRate)
    {
        sr = (float) sampleRate;
        reset();
    }
    void reset()
    {
        for (auto& c : s) for (auto& v : c) v = 0.0f;
    }

    // coefficient for a cutoff in Hz
    inline float gFor (float hz) const noexcept
    {
        return std::tan (juce::MathConstants<float>::pi * juce::jlimit (10.0f, sr * 0.47f, hz) / sr);
    }
    // 1/Q of the resonant stage for a resonance 0..1
    // Resonance 0..1 -> Q 0.707..5.5 (until v0.9.9 it went up to 12.7: a narrow, thin peak; this one is about 2.5x wider)
    static inline float kFor (float res) noexcept { return 1.0f / (0.707f + 4.8f * juce::jlimit (0.0f, 1.0f, res)); }

    // typeMix: weights for low, band, high (sum 1); slopeMix: 0 = 12 dB, 1 = 24 dB
    inline void process (float& l, float& r, float g, float k, float drive, const float* typeMix, float slopeMix) noexcept
    {
        if (drive > 0.0005f)
        {
            const float dg = 1.0f + 9.0f * drive, norm = 1.0f / std::sqrt (dg);
            l = std::tanh (l * dg) * norm;
            r = std::tanh (r * dg) * norm;
        }
        const float a1 = 1.0f / (1.0f + g * (g + k)), a2 = g * a1, a3 = g * a2;
        // first stage: Butterworth-ish (k = 1/0.541 for the 24 dB pair; the second stage carries the resonance)
        const float kb = 1.0f / 0.541f;
        const float b1 = 1.0f / (1.0f + g * (g + kb)), b2 = g * b1, b3 = g * b2;
        l = channel (l, 0, g, k, a1, a2, a3, kb, b1, b2, b3, typeMix, slopeMix);
        r = channel (r, 1, g, k, a1, a2, a3, kb, b1, b2, b3, typeMix, slopeMix);
    }

private:
    static inline float stage (float x, float& ic1, float& ic2, float k, float a1, float a2, float a3, const float* mix) noexcept
    {
        const float v3 = x - ic2;
        const float v1 = a1 * ic1 + a2 * v3;
        const float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        const float lp = v2, bp = v1 * k, hp = x - k * v1 - v2;
        return lp * mix[0] + bp * mix[1] + hp * mix[2];
    }
    inline float channel (float x, int c, float, float k, float a1, float a2, float a3, float kb, float b1, float b2, float b3,
                          const float* mix, float slopeMix) noexcept
    {
        auto& st = s[c];
        if (slopeMix <= 0.0005f)
            return stage (x, st[0], st[1], k, a1, a2, a3, mix);
        // 24 dB: first stage (fixed Q) into the resonant stage. Keep the first stage running during the crossfade so
        // the state never jumps when the slope changes.
        const float y1 = stage (x, st[2], st[3], kb, b1, b2, b3, mix);
        const float yIn = x + (y1 - x) * slopeMix;
        return stage (yIn, st[0], st[1], k, a1, a2, a3, mix);
    }

    float sr = 44100.0f;
    float s[2][4] {};
};

// 12 dB high pass used as the separate HP stage (bypassed below 21 Hz).
struct HighPass12
{
    void reset() { ic1[0] = ic1[1] = ic2[0] = ic2[1] = 0.0f; }
    inline float process (float x, int c, float g) noexcept
    {
        constexpr float k = 1.41421356f;
        const float a1 = 1.0f / (1.0f + g * (g + k)), a2 = g * a1, a3 = g * a2;
        const float v3 = x - ic2[c];
        const float v1 = a1 * ic1[c] + a2 * v3;
        const float v2 = ic2[c] + a2 * ic1[c] + a3 * v3;
        ic1[c] = 2.0f * v1 - ic1[c];
        ic2[c] = 2.0f * v2 - ic2[c];
        return x - k * v1 - v2;
    }
    float ic1[2] {}, ic2[2] {};
};
