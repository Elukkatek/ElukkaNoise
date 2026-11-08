#pragma once
// Click detection for noise signals. Noise itself is full of sample-to-sample jumps, so the second difference used
// for tonal material is useless here. Instead: the first difference d[n] of a band-limited (filtered) noise has a
// Gaussian-like spread; a discontinuity (gain step, buffer jump, state reset) shows as an outlier far beyond the local
// spread of d. Any |d| above `sigmas` times the local RMS of d (over ±window samples, the sample itself left out) is a
// click. Gaussian noise exceeds 8 sigma about once in 10^15 samples, so true noise never trips it.
#include <JuceHeader.h>

struct ClickReport { int clicks = 0; float worstSigmas = 0.0f; int firstAt = -1; };

inline ClickReport detectClicks (const juce::AudioBuffer<float>& b, float sigmas = 8.0f, int skipStart = 0, int window = 256)
{
    ClickReport r;
    const int n = b.getNumSamples();
    std::vector<float> d ((size_t) n, 0.0f);
    std::vector<double> d2 ((size_t) n + 1, 0.0);
    for (int ch = 0; ch < b.getNumChannels(); ++ch)
    {
        const float* x = b.getReadPointer (ch);
        for (int i = 1; i < n; ++i) d[(size_t) i] = x[i] - x[i - 1];
        d2[0] = 0.0;
        for (int i = 0; i < n; ++i) d2[(size_t) i + 1] = d2[(size_t) i] + (double) d[(size_t) i] * d[(size_t) i];
        int lastClick = -100000;
        for (int i = juce::jmax (2, skipStart); i < n; ++i)
        {
            const int lo = juce::jmax (0, i - window), hi = juce::jmin (n, i + window + 1);
            const double sum = d2[(size_t) hi] - d2[(size_t) lo] - (double) d[(size_t) i] * d[(size_t) i];
            const double rms = std::sqrt (juce::jmax (1.0e-20, sum / (double) (hi - lo - 1)));
            const float s = (float) (std::abs (d[(size_t) i]) / rms);
            if (rms < 1.0e-6) continue;   // silence: nothing to click against
            r.worstSigmas = juce::jmax (r.worstSigmas, s);
            if (s > sigmas && i - lastClick > 1000)
            {
                ++r.clicks;
                if (r.firstAt < 0) r.firstAt = i;
                lastClick = i;
            }
        }
    }
    return r;
}
