#pragma once
#include <JuceHeader.h>

// Stereo noise source: two independent white streams (xorshift), each coloured to pink (Kellet's economy filter) and
// brown (leaky integrator). Colour blends brown -> pink -> white; Stereo blends the right channel between a copy of
// the left stream (mono) and its own stream (fully decorrelated). The three colours are level-matched at prepare().
class NoiseGen
{
public:
    void prepare (double sampleRate)
    {
        // The same sound at every sample rate: white noise keeps its spectral density (level grows with the square root
        // of the rate, the extra energy sits above 20 kHz) and the brown integrator keeps its corner frequency.
        level = 0.36f * (float) std::sqrt (sampleRate / 48000.0);
        // above 48 kHz: a fixed Butterworth low pass at 20 kHz keeps the audible band the same as at 44.1/48 kHz
        topOn = sampleRate > 50000.0;
        topG = std::tan (juce::MathConstants<float>::pi * 20000.0f / (float) sampleRate);
        for (auto& v : top) v = 0.0f;
        brownLeak = std::exp (-juce::MathConstants<float>::twoPi * 35.0f / (float) sampleRate);
        // Kellet's pink filter is designed for 44.1 kHz: move its three poles to the same frequencies at this rate
        // and keep each section's low-frequency gain, so the pink tilt is the same at 48, 96 and 192 kHz (v0.9.26).
        {
            constexpr float a0[3] { 0.99765f, 0.96300f, 0.57000f }, g0[3] { 0.0990460f, 0.2965164f, 1.0526913f };
            const float r = (float) (44100.0 / sampleRate);
            for (int i = 0; i < 3; ++i) { pkA[i] = std::pow (a0[i], r); pkB[i] = g0[i] * (1.0f - pkA[i]) / (1.0f - a0[i]); }
        }
        // level-match: run each colour and scale so all three have the same RMS as white
        reseed (0x9e3779b9u);
        double sw = 0.0, sp = 0.0, sb = 0.0;
        constexpr int n = 1 << 16;
        pinkGain = brownGain = 1.0f;
        for (int i = 0; i < n; ++i)
        {
            const float w = white (0);
            const float p = pink (0, w), b = brown (0, w);
            sw += w * w; sp += p * p; sb += b * b;
        }
        pinkGain  = (float) std::sqrt (sw / juce::jmax (1.0e-12, sp));
        brownGain = (float) std::sqrt (sw / juce::jmax (1.0e-12, sb));
        reseed (defaultSeed);
        for (auto& p : pk) p = {};
        br[0] = br[1] = 0.0f;
    }

    void reseed (juce::uint32 seed)
    {
        state[0] = seed * 2654435761u + 1u;
        state[1] = (seed ^ 0x5bd1e995u) * 2246822519u + 7u;
        if (state[0] == 0) state[0] = 1;
        if (state[1] == 0) state[1] = 2;
    }

    static constexpr juce::uint32 defaultSeed = 0x1234567u;

    // One stereo sample. colour 0 = brown, 0.5 = pink, 1 = white; the right channel mixes the left stream (shared)
    // and its own stream (own) with the weights from stereoWeights().
    inline void next (float colour, float shared, float own, float& l, float& r) noexcept
    {
        const float w0 = white (0), w1 = white (1);
        const float c0 = colourise (0, w0, colour), c1 = colourise (1, w1, colour);
        l = c0 * level;
        r = (c0 * shared + c1 * own) * level;
        if (topOn)
        {
            l = topLp (l, top[0], top[1]);
            r = topLp (r, top[2], top[3]);
        }
    }
    // mix weights for the right channel: equal power between the shared and the independent stream
    static void stereoWeights (float stereo, float& shared, float& own) noexcept
    {
        const float a = juce::jlimit (0.0f, 1.0f, stereo) * juce::MathConstants<float>::halfPi;
        shared = std::cos (a); own = std::sin (a);
    }

private:
    inline float white (int c) noexcept
    {
        // xorshift32
        juce::uint32 x = state[c];
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        state[c] = x;
        return (float) (x >> 8) * (1.0f / 8388608.0f) - 1.0f;   // 24 bits -> -1..1
    }
    inline float pink (int c, float w) noexcept
    {
        auto& p = pk[c];
        p.b0 = pkA[0] * p.b0 + w * pkB[0];
        p.b1 = pkA[1] * p.b1 + w * pkB[1];
        p.b2 = pkA[2] * p.b2 + w * pkB[2];
        return (p.b0 + p.b1 + p.b2 + w * 0.1848f) * pinkGain;
    }
    inline float brown (int c, float w) noexcept
    {
        br[c] = brownLeak * br[c] + w * (1.0f - brownLeak);
        return br[c] * brownGain;
    }
    inline float colourise (int c, float w, float colour) noexcept
    {
        const float p = pink (c, w), b = brown (c, w);
        if (colour >= 0.5f)
        {
            const float t = (colour - 0.5f) * 2.0f;
            return p + (w - p) * t;
        }
        const float t = colour * 2.0f;
        return b + (p - b) * t;
    }

    inline float topLp (float x, float& ic1, float& ic2) const noexcept
    {
        constexpr float k = 1.41421356f;
        const float a1 = 1.0f / (1.0f + topG * (topG + k)), a2 = topG * a1, a3 = topG * a2;
        const float v3 = x - ic2;
        const float v1 = a1 * ic1 + a2 * v3;
        const float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        return v2;
    }

    juce::uint32 state[2] { 1, 2 };
    struct Pink { float b0 = 0.0f, b1 = 0.0f, b2 = 0.0f; } pk[2];
    float br[2] {};
    float pinkGain = 1.0f, brownGain = 1.0f, level = 0.36f, brownLeak = 0.995f, topG = 1.0f;
    float pkA[3] { 0.99765f, 0.96300f, 0.57000f }, pkB[3] { 0.0990460f, 0.2965164f, 1.0526913f };   // pink poles / gains at this rate
    bool topOn = false;
    float top[4] {};
};
