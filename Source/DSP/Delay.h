#pragma once
#include <JuceHeader.h>
#include "../Params.h"

// Stereo delay built for noise (v0.9.19): clean synced echoes, wide ping-pong and evolving filtered feedback.
//
//   in -> ducking detector (the dry level) ; two lines (left, right = left * (1 + offset)), each read by a crossfading
//   pair of heads (a time change fades to the new time in 30 ms: exact at once, no pitch bend) plus a slow sine
//   modulation (left / right in quadrature) -> second-order low pass + high pass in the loop (the Filter knob) -> loop
//   limiter -> written back by mode (mono / stereo / ping-pong / wide ping-pong, all as glided mixing weights, so a
//   mode change crossfades) -> width (mid / side) -> duck -> dry / wet crossfade
//
// Feedback 100 % sustains: the loop limiter (a smooth gain on the fed-back signal when its RMS passes -12 dBFS, a
// little above a typical noise input) keeps the level bounded without clipping. Every value glides and the coefficients update every 128 samples, so
// nothing clicks; the buffers are never cleared while audible. Everything is allocated in prepare().
class StereoDelay
{
public:
    struct Params
    {
        float timeSeconds = 0.375f;   // the left line; the right follows Offset
        float feedback = 0.6f;        // 0..1 (per repeat; once per round trip in ping-pong, as before)
        float wet = 0.0f;
        float filter = 0.0f;          // -1 dark .. 0 neutral .. +1 thin and bright (repeats evolve)
        float width = 1.0f;           // 0 = mono echoes, 1 = full
        float offset = 0.0f;          // -0.5..0.5: right time = left time * (1 + offset)
        float mod = 0.0f;             // 0..1 delay modulation depth
        float modRateHz = 0.5f;
        float duck = 0.0f;
        int mode = P::DmPingPong;
    };

    // The Filter knob's low pass and high pass: left closes the low pass (and lifts the high pass a little, so dark
    // never means muddy), right lifts the high pass; the centre is neutral (20 kHz / 100 Hz).
    static float filterLp (float f) { return f < 0.0f ? 20000.0f * std::pow (350.0f / 20000.0f, -f) : 20000.0f; }
    static float filterHp (float f) { return f < 0.0f ? 100.0f * std::pow (1.5f, -f) : 100.0f * std::pow (50.0f, f); }

    void prepare (double sampleRate)
    {
        sr = sampleRate;
        len = juce::nextPowerOfTwo ((int) (maxSeconds * sr) + 4);
        for (auto& b : buf) b.assign ((size_t) len, 0.0f);
        auto overMs = [this] (float ms) { return 1.0f - std::exp (-1.0f / (ms * 0.001f * (float) sr)); };
        detRmsA = overMs (3.0f); detAtkA = overMs (1.0f); detRelA = overMs (50.0f); duckSmA = overMs (4.0f);
        detRefDecay = std::exp (-1.0f / (2.0f * (float) sr));
        limAtkA = overMs (1.0f); limRelA = overMs (200.0f); limSmA = overMs (5.0f);
        fadeStep = 1.0f / (0.03f * (float) sr);
        reset();
    }

    void reset()
    {
        resetState();
        clearCursor = -1;
        for (auto& b : buf) std::fill (b.begin(), b.end(), 0.0f);
    }

    // Clearing the buffers in pieces (v0.9.26): when the delay goes idle the engine calls beginClear() and then
    // clearStep() every control block, so the 2-8 MB fill never lands in one audio callback. If the delay is needed
    // again before the clear is complete, finishClear() does the rest at once (rare, and bounded).
    void beginClear() { resetState(); clearCursor = 0; }
    void clearStep (int budget)
    {
        if (clearCursor < 0) return;
        const int total = 2 * len;
        while (budget > 0 && clearCursor < total)
        {
            const int b = clearCursor / len, o = clearCursor % len, m = juce::jmin (budget, len - o);
            std::fill (buf[b].begin() + o, buf[b].begin() + o + m, 0.0f);
            clearCursor += m; budget -= m;
        }
        if (clearCursor >= total) clearCursor = -1;
    }
    void finishClear() { clearStep (std::numeric_limits<int>::max()); }

private:
    void resetState()
    {
        pos = 0;
        for (int c = 0; c < 2; ++c) { cur[c] = from[c] = -1.0f; fade[c] = 1.0f; lpZ[c][0] = lpZ[c][1] = hpZ[c][0] = hpZ[c][1] = 0.0f; }
        curWet = -1.0f;
        energy = 0.0f;
        inFade = 0.0f;   // after a reset the input fades into the buffer, so the echoes never replay a hard cut-in
        detRms = detEnv = detRef = 0.0f;
        duckGain = 1.0f;
        limRms = limEnv = 0.0f; limGain = 1.0f;
        lfo = { 1.0f, 0.0f };
        primed = false;
    }

public:
    // true while the tail is still audible (or input arrives); the caller may skip processing otherwise
    bool isActive() const { return energy > 1.0e-7f; }

    // Adds the echoes to L/R in place (dry / wet crossfade). Works in pieces of up to 128 samples so the glided
    // values move in small steps whatever the host block size.
    void process (float* L, float* R, int n, const Params& p)
    {
        for (int start = 0; start < n; start += subBlock)
            processPiece (L + start, R + start, juce::jmin (subBlock, n - start), p);
    }

    static constexpr float maxSeconds = 4.2f;

    // for tests
    float currentDelaySamples (int c) const { return cur[c]; }
    float getDuckGain() const { return duckGain; }

private:
    void updateTargets (const Params& p, int n)
    {
        // glide over ~60 ms (per piece): parameter and mode changes never jump
        const float k = primed ? 1.0f - std::exp (-(float) n / (0.06f * (float) sr)) : 1.0f;
        auto glide = [k] (float& v, float target) { v += (target - v) * k; };
        glide (sFb, juce::jlimit (0.0f, 1.0f, p.feedback));
        glide (sWidth, juce::jlimit (0.0f, 1.0f, p.width));
        glide (sDuck, juce::jlimit (0.0f, 1.0f, p.duck));
        glide (sMod, juce::jlimit (0.0f, 1.0f, p.mod));
        glide (sLogRate, std::log (juce::jlimit (0.02f, 8.0f, p.modRateHz)));
        const float f = juce::jlimit (-1.0f, 1.0f, p.filter);
        glide (sLogLp, std::log (filterLp (f)));
        glide (sLogHp, std::log (filterHp (f)));
        // the write path as mixing weights: [inL, inMono, left echo, right echo] into each line, plus how far the reads
        // are summed to mono first (Mono mode). Gliding these crossfades a mode change instead of stepping it.
        float wl[4] {}, wr[4] {}, mono = 0.0f;
        switch (p.mode)
        {
            case P::DmMono:         wl[1] = 1.0f; wl[2] = sFb; wr[1] = 1.0f; wr[2] = sFb; mono = 1.0f; break;
            case P::DmStereo:       wl[0] = 1.0f; wl[2] = sFb; wr[0] = 1.0f; wr[3] = sFb; break;
            case P::DmWidePingPong: wl[0] = 1.0f; wl[3] = sFb; wr[0] = 1.0f; wr[2] = sFb; break;   // cross feedback: every echo swaps sides
            default:                wl[1] = 1.0f; wl[3] = sFb * sFb; wr[2] = 1.0f; break;         // ping-pong: the bounce at full level, feedback once per round trip
        }
        for (int i = 0; i < 4; ++i) { glide (gL[i], wl[i]); glide (gR[i], wr[i]); }
        glide (gMono, mono);
        if (! primed) { for (int i = 0; i < 4; ++i) { mL[i] = gL[i]; mR[i] = gR[i]; } mMono = gMono; }

        // the loop filters (Butterworth): targets, ramped per sample in processPiece (a per-piece coefficient step is
        // a click on dark material)
        setButterworth (lpT, std::exp (sLogLp), true);
        setButterworth (hpT, std::exp (sLogHp), false);
        if (! primed) for (int i = 0; i < 5; ++i) { lpC[i] = lpT[i]; hpC[i] = hpT[i]; }
        // up to 6 ms; ramped per sample in processPiece (a per-piece step in the read offset is a click on dark material)
        tgtModDepth = sMod * 0.006f * (float) sr;
        if (! primed) modDepth = tgtModDepth;
        lfoW = juce::MathConstants<float>::twoPi * std::exp (sLogRate) / (float) sr;

        // delay times: a change fades to the new time (30 ms) unless a fade is already running
        const float tL = juce::jlimit (1.0f, (float) len - 8.0f - 2.0f * juce::jmax (modDepth, tgtModDepth), (float) (juce::jlimit (0.001f, maxSeconds, p.timeSeconds) * sr));
        const float tR = juce::jlimit (1.0f, (float) len - 8.0f - 2.0f * juce::jmax (modDepth, tgtModDepth), tL * (1.0f + juce::jlimit (-0.5f, 0.5f, p.offset)));
        const float target[2] { tL, tR };
        for (int c = 0; c < 2; ++c)
        {
            if (cur[c] < 0.0f) { cur[c] = from[c] = target[c]; fade[c] = 1.0f; }
            else if (fade[c] >= 1.0f && std::abs (target[c] - cur[c]) > 0.0001f * (float) sr)   // more than 0.1 ms
            {
                from[c] = cur[c];
                cur[c] = target[c];
                fade[c] = 0.0f;
            }
        }
        if (curWet < 0.0f) curWet = p.wet;
        primed = true;
    }

    void processPiece (float* L, float* R, int n, const Params& p)
    {
        updateTargets (p, n);
        const float modInc = (tgtModDepth - modDepth) / (float) n;
        float lpInc[5], hpInc[5], mIncL[4], mIncR[4];
        for (int i = 0; i < 5; ++i) { lpInc[i] = (lpT[i] - lpC[i]) / (float) n; hpInc[i] = (hpT[i] - hpC[i]) / (float) n; }
        for (int i = 0; i < 4; ++i) { mIncL[i] = (gL[i] - mL[i]) / (float) n; mIncR[i] = (gR[i] - mR[i]) / (float) n; }
        const float monoInc = (gMono - mMono) / (float) n;
        const float wetInc = (p.wet - curWet) / (float) n;
        const int mask = len - 1;
        const float inFadeStep = 1.0f / (0.01f * (float) sr);
        const float duckAmt = sDuck, width = sWidth;
        float e = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            const float inL = L[i], inR = R[i];
            // ducking detector on the dry input (as in the reverb): 3 ms RMS, 1 ms attack / 50 ms release, against the
            // loudest level of the last couple of seconds
            {
                const float x2 = 0.5f * (inL * inL + inR * inR);
                detRms += (x2 - detRms) * detRmsA;
                const float lvl = std::sqrt (detRms);
                detEnv += (lvl - detEnv) * (lvl > detEnv ? detAtkA : detRelA);
                detRef = juce::jmax (detEnv, detRef * detRefDecay);
                const float ratio = detRef > 1.0e-6f ? detEnv / detRef : 0.0f;
                duckGain += ((1.0f - duckAmt * ratio) - duckGain) * duckSmA;
            }
            // modulation: one sine, the right line in quadrature (the depth ramps to its target over the piece)
            modDepth += modInc;
            lfo.s += lfoW * lfo.c;
            lfo.c -= lfoW * lfo.s;
            const float modOff[2] { modDepth * (1.0f + lfo.s), modDepth * (1.0f + lfo.c) };

            // read the two lines: the active head, crossfaded (equal power) from the previous one while a time change
            // settles
            float rd[2];
            for (int c = 0; c < 2; ++c)
            {
                const float* b = buf[c].data();
                auto readAt = [&] (float d)
                {
                    const float rp = (float) (pos + len) - d - modOff[c];
                    const int i0 = (int) rp;
                    const float t = rp - (float) i0;
                    const float a0 = b[i0 & mask], a1 = b[(i0 + 1) & mask];
                    return a0 + (a1 - a0) * t;
                };
                if (fade[c] < 1.0f)
                {
                    fade[c] = juce::jmin (1.0f, fade[c] + fadeStep);
                    const float gNew = std::sqrt (fade[c]), gOld = std::sqrt (1.0f - fade[c]);
                    rd[c] = readAt (from[c]) * gOld + readAt (cur[c]) * gNew;
                }
                else rd[c] = readAt (cur[c]);
            }
            for (int i = 0; i < 5; ++i) { lpC[i] += lpInc[i]; hpC[i] += hpInc[i]; }
            for (int i = 0; i < 4; ++i) { mL[i] += mIncL[i]; mR[i] += mIncR[i]; }   // the mode weights ramp per sample too
            mMono += monoInc;
            // Mono mode sums the reads first (glided)
            const float mid0 = 0.5f * (rd[0] + rd[1]);
            rd[0] += (mid0 - rd[0]) * mMono;
            rd[1] += (mid0 - rd[1]) * mMono;

            // the loop filters: second-order low pass then high pass per channel
            float f[2];
            for (int c = 0; c < 2; ++c)
            {
                float x = rd[c];
                float y = lpC[0] * x + lpZ[c][0];
                lpZ[c][0] = lpC[1] * x - lpC[3] * y + lpZ[c][1];
                lpZ[c][1] = lpC[2] * x - lpC[4] * y;
                x = y;
                y = hpC[0] * x + hpZ[c][0];
                hpZ[c][0] = hpC[1] * x - hpC[3] * y + hpZ[c][1];
                hpZ[c][1] = hpC[2] * x - hpC[4] * y;
                f[c] = y;
            }
            // loop limiter: a smooth gain on what goes back in, so feedback 100 % sustains instead of running away
            {
                limRms += (0.5f * (f[0] * f[0] + f[1] * f[1]) - limRms) * detRmsA;
                const float lvl = std::sqrt (limRms);
                limEnv += (lvl - limEnv) * (lvl > limEnv ? limAtkA : limRelA);
                const float want = limEnv > limThreshold ? limThreshold / limEnv : 1.0f;
                limGain += (want - limGain) * limSmA;
            }
            const float fl = f[0] * limGain, fr = f[1] * limGain;

            // write back by the mode's weights
            inFade = juce::jmin (1.0f, inFade + inFadeStep);
            const float wL = inL * inFade, wR = inR * inFade, wM = 0.5f * (wL + wR);
            buf[0][(size_t) pos] = mL[0] * wL + mL[1] * wM + mL[2] * fl + mL[3] * fr;
            buf[1][(size_t) pos] = mR[0] * wR + mR[1] * wM + mR[2] * fl + mR[3] * fr;
            pos = (pos + 1) & mask;

            // stereo out: width on the echoes, ducking, dry / wet
            const float mid = 0.5f * (f[0] + f[1]), side = 0.5f * (f[0] - f[1]) * width;
            const float g = duckGain;
            const float echoL = (mid + side) * g, echoR = (mid - side) * g;
            L[i] = inL + (echoL - inL) * curWet;
            R[i] = inR + (echoR - inR) * curWet;
            curWet += wetInc;
            e = juce::jmax (e, std::abs (echoL) + std::abs (echoR) + std::abs (inL) + std::abs (inR));
        }
        curWet = p.wet;
        energy = e;
        // keep the oscillator on the unit circle
        const float r = 1.5f - 0.5f * (lfo.s * lfo.s + lfo.c * lfo.c);
        lfo.s *= r; lfo.c *= r;
    }

    // Butterworth biquad (b0, b1, b2, a1, a2), low or high pass
    void setButterworth (float* c, float hz, bool lowPass) const
    {
        const float w = juce::MathConstants<float>::twoPi * juce::jlimit (5.0f, (float) sr * 0.45f, hz) / (float) sr;
        const float cw = std::cos (w), alpha = std::sin (w) / std::sqrt (2.0f), a0 = 1.0f + alpha;
        const float s = lowPass ? 1.0f : -1.0f;
        c[0] = 0.5f * (1.0f - s * cw) / a0;
        c[1] = s * (1.0f - s * cw) / a0;
        c[2] = c[0];
        c[3] = -2.0f * cw / a0;
        c[4] = (1.0f - alpha) / a0;
    }

    static constexpr int subBlock = 128;
    static constexpr float limThreshold = 0.25f;   // RMS, -12 dBFS

    double sr = 44100.0;
    std::vector<float> buf[2];
    int len = 0, pos = 0, clearCursor = -1;   // -1: not clearing
    float cur[2] { -1.0f, -1.0f }, from[2] { -1.0f, -1.0f }, fade[2] { 1.0f, 1.0f }, fadeStep = 0.001f;
    float curWet = -1.0f, energy = 0.0f, inFade = 1.0f;
    float lpC[5] { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f }, hpC[5] { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f }, lpT[5] {}, hpT[5] {};
    float lpZ[2][2] {}, hpZ[2][2] {};
    float mL[4] {}, mR[4] {}, mMono = 0.0f;
    float gL[4] {}, gR[4] {}, gMono = 0.0f;   // the glided targets the mode weights ramp towards within a piece
    float modDepth = 0.0f, tgtModDepth = 0.0f, lfoW = 0.0f;
    struct Osc { float s, c; } lfo { 1.0f, 0.0f };
    // ducking detector
    float detRmsA = 0.01f, detAtkA = 0.05f, detRelA = 0.001f, detRefDecay = 0.99999f, duckSmA = 0.01f;
    float detRms = 0.0f, detEnv = 0.0f, detRef = 0.0f, duckGain = 1.0f;
    // loop limiter
    float limAtkA = 0.05f, limRelA = 0.0001f, limSmA = 0.01f, limRms = 0.0f, limEnv = 0.0f, limGain = 1.0f;
    // glided values
    float sFb = 0.6f, sWidth = 1.0f, sDuck = 0.0f, sMod = 0.0f, sLogRate = std::log (0.5f), sLogLp = std::log (20000.0f), sLogHp = std::log (100.0f);
    bool primed = false;
};
