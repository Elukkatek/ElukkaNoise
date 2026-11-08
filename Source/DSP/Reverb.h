#pragma once
#include <JuceHeader.h>
#include <complex>
#include "../Params.h"

// Reverb built for broadband noise (v0.9.17). Noise shows up every resonance, flutter echo and low-end pile-up a
// reverb has, so the tail is made as dense and even as possible and loses its lows and highs faster than its mids:
//
//   in -> ducking detector (the dry level) ; pre-delay -> high pass, bandwidth low pass -> 4 allpass diffusers per
//   channel -> 8-line FDN (Hadamard feedback; each line: delay modulated by its own LFO phase, two short allpasses,
//   high shelf, DC blocker, low shelf, decay gain) -> two orthogonal L/R tap sets -> width (mid / side)
//   -> duck -> dry / wet crossfade
//
// Decay is the T60 of the band that rings longest (the mids, between the low shelf and Low Pass): the decay gain is
// set against the loop filters' peak response, so nothing rings longer than Decay and the loop gain never exceeds 1.
// The damping is shelving (second order, so the midrange between the shelves is flat), not a full roll-off: above
// Low Pass the tail decays several times faster (the character's high ratio), below a corner just above High Pass
// about twice as fast (its low ratio); linear interpolation in the modulated lines adds its own loss above ~5 kHz. The tail darkens
// as it fades and a long tail never turns into a low rumble. Seven characters
// set the internals (diffusion, size scale, modulation scale, the two ratios, input band); the internals and every
// parameter glide, so nothing clicks. Everything is allocated in prepare(); the audio path never allocates or locks.
class NoiseReverb
{
public:
    static constexpr int N = 8;

    struct Params
    {
        float mix = 0.0f;          // 0..1 dry / wet
        float decay = 3.0f;        // seconds (T60 of the midrange)
        float size = 0.65f;        // 0..1
        float lpHz = 1200.0f;      // damping cut-off inside the tank (and the input band)
        float hpHz = 110.0f;       // input low cut; the tank's own low cut follows it
        float preDelayMs = 10.0f;  // 0..200
        float width = 1.0f;        // 0 = mono, 1 = full
        float mod = 0.25f;         // 0..1 delay modulation depth
        float modRateHz = 0.5f;    // 0.05..5
        float duck = 0.0f;         // 0..1: how far the dry input pushes the wet level down
        int character = P::RHall;
    };

    void prepare (double sampleRate)
    {
        sr = sampleRate;
        rateScale = (float) (sr / 44100.0);
        for (int c = 0; c < 2; ++c)
            for (int k = 0; k < numDiffusers; ++k)
                diffuser[c][k].prepare ((int) std::lround (diffuserMs[c][k] * 0.001 * sr));
        for (int i = 0; i < N; ++i)
        {
            const int maxLen = (int) (lineMs[i] * 0.001 * sr * maxSizeScale) + (int) (2.0f * maxModDepth * rateScale) + 8;
            lines[i].assign ((size_t) juce::nextPowerOfTwo (maxLen + 1), 0.0f);
            mask[i] = (int) lines[i].size() - 1;
            lineBuf[i] = lines[i].data();
            // the loop allpasses: power-of-two rings with one shared write index, so the per-line work has no branches
            ap1Len[i] = juce::jmax (1, (int) std::lround (loopApMs[i] * 0.001 * sr));
            ap2Len[i] = juce::jmax (1, (int) std::lround (loopAp2Ms[i] * 0.001 * sr));
            ap1Buf[i].assign ((size_t) juce::nextPowerOfTwo (ap1Len[i] + 1), 0.0f);
            ap2Buf[i].assign ((size_t) juce::nextPowerOfTwo (ap2Len[i] + 1), 0.0f);
            ap1Mask[i] = (int) ap1Buf[i].size() - 1; ap2Mask[i] = (int) ap2Buf[i].size() - 1;
            ap1[i] = ap1Buf[i].data(); ap2[i] = ap2Buf[i].data();
        }
        preLen = juce::nextPowerOfTwo ((int) (0.21 * sr) + 4);
        for (auto& b : pre) b.assign ((size_t) preLen, 0.0f);
        auto onePole = [this] (float hz) { return 1.0f - std::exp (-juce::MathConstants<float>::twoPi * hz / (float) sr); };
        auto overMs = [this] (float ms) { return 1.0f - std::exp (-1.0f / (ms * 0.001f * (float) sr)); };
        detRmsA = overMs (3.0f); detAtkA = overMs (1.0f); detRelA = overMs (50.0f); duckSmA = overMs (4.0f);
        detRefDecay = std::exp (-1.0f / (2.0f * (float) sr));
        juce::ignoreUnused (onePole);
        preA = overMs (22.7f);   // the pre-delay glide (was a fixed 0.001 per sample: 22.7 ms at 44.1 kHz)
        // every buffer as one list, for reset() and the piecewise clear
        spans.clear();
        for (auto& l : lines) spans.push_back ({ l.data(), (int) l.size() });
        for (auto& a : ap1Buf) spans.push_back ({ a.data(), (int) a.size() });
        for (auto& a : ap2Buf) spans.push_back ({ a.data(), (int) a.size() });
        for (auto& c : diffuser) for (auto& d : c) spans.push_back ({ d.buf.data(), (int) d.buf.size() });
        for (auto& b : pre) spans.push_back ({ b.data(), (int) b.size() });
        reset();
    }

    // true while the tail is still audible (or input arrives)
    bool isActive() const { return energy > 1.0e-7f; }

    void reset()
    {
        resetState();
        clearSpan = -1;
        for (auto& s : spans) std::fill (s.first, s.first + s.second, 0.0f);
    }

    // Clearing in pieces (v0.9.26), as in StereoDelay: beginClear() when the reverb goes idle, clearStep() every
    // control block, finishClear() if it is needed again before the clear is complete.
    void beginClear() { resetState(); clearSpan = 0; clearOffset = 0; }
    void clearStep (int budget)
    {
        if (clearSpan < 0) return;
        while (budget > 0 && clearSpan < (int) spans.size())
        {
            auto& s = spans[(size_t) clearSpan];
            const int m = juce::jmin (budget, s.second - clearOffset);
            std::fill (s.first + clearOffset, s.first + clearOffset + m, 0.0f);
            clearOffset += m; budget -= m;
            if (clearOffset >= s.second) { ++clearSpan; clearOffset = 0; }
        }
        if (clearSpan >= (int) spans.size()) clearSpan = -1;
    }
    void finishClear() { clearStep (std::numeric_limits<int>::max()); }

private:
    void resetState()
    {
        for (auto& c : diffuser) for (auto& d : c) d.pos = 0;
        writePos = apPos = 0;
        for (auto* z : { lowCut, hiZ0, hiZ1, loZ0, loZ1 }) z[0] = z[1] = V4();
        inLowCut[0] = inLowCut[1] = inBand[0] = inBand[1] = 0.0f;
        for (int j = 0; j < numLfos; ++j) lfo[j] = { j & 1 ? 0.0f : 1.0f, j & 1 ? 1.0f : 0.0f };
        prePos = 0;
        curPre = -1.0f;
        energy = 0.0f;
        inFade = 0.0f;   // after a reset the input fades in, so the tail never starts with a hard cut-in
        detRms = detEnv = detRef = 0.0f;
        duckGain = 1.0f;
        primed = false;
    }

public:
    // Adds the reverb to L/R in place (dry / wet crossfade by p.mix). Works in pieces of up to 128 samples so the
    // glided internals (gains, shelf coefficients, modulation depth) move in small steps whatever the host block size.
    void process (float* L, float* R, int n, const Params& p)
    {
        for (int start = 0; start < n; start += subBlock)
            processPiece (L + start, R + start, juce::jmin (subBlock, n - start), p);
    }

private:
    void processPiece (float* L, float* R, int n, const Params& p)
    {
        updateTargets (p, n);
        const float invN = 1.0f / (float) juce::jmax (1, n);
        float len[N], dLen[N];
        alignas (16) float gArr[N], dGArr[N];
        for (int i = 0; i < N; ++i)
        {
            len[i] = curLen[i];
            dLen[i] = (tgtLen[i] - curLen[i]) * invN;
            curLen[i] = tgtLen[i];
            gArr[i] = curGain[i];   // the decay gain ramps per sample too: a Decay jump fades the tail, it never steps it
            dGArr[i] = (gain[i] - curGain[i]) * invN;
            curGain[i] = gain[i];
        }
        // the per-line arithmetic runs on two 4-wide registers (lines 0-3 and 4-7)
        V4 g[2] { V4::fromRawArray (gArr), V4::fromRawArray (gArr + 4) }, dG[2] { V4::fromRawArray (dGArr), V4::fromRawArray (dGArr + 4) };
        const V4 hb0 = V4::expand (hi.b0), hb1 = V4::expand (hi.b1), hb2 = V4::expand (hi.b2), ha1 = V4::expand (hi.a1), ha2 = V4::expand (hi.a2),
                 hg = V4::expand (hi.g), hg1 = V4::expand (1.0f - hi.g);
        const V4 lb0 = V4::expand (lo.b0), lb1 = V4::expand (lo.b1), lb2 = V4::expand (lo.b2), la1 = V4::expand (lo.a1), la2 = V4::expand (lo.a2),
                 lg = V4::expand (lo.g), lg1 = V4::expand (1.0f - lo.g);
        const V4 dcA = V4::expand (lowCutA), apG = V4::expand (loopApGain);
        const V4 tapL[2] { V4::fromRawArray (outL), V4::fromRawArray (outL + 4) }, tapR[2] { V4::fromRawArray (outR), V4::fromRawArray (outR + 4) };
        float mix = curMix;
        const float dMix = (tgtMix - curMix) * invN;
        curMix = tgtMix;

        const float depth = modDepth;
        const float tgtPre = juce::jlimit (0.0f, (float) preLen - 4.0f, (float) (juce::jlimit (0.0f, 200.0f, p.preDelayMs) * 0.001 * sr));
        if (curPre < 0.0f) curPre = tgtPre;
        const int preMask = preLen - 1;
        const float fadeStep = 1.0f / (0.01f * (float) sr);
        const float duckAmt = sDuck, width = sWidth;
        float e = 0.0f;
        for (int s = 0; s < n; ++s)
        {
            // Ducking detector on the dry input: a 3 ms RMS, followed with a 1 ms attack / 50 ms release, measured
            // against the loudest level of the last couple of seconds. A gate pulse is ducked at once, and the wet
            // level is most of the way back by the time the next 1/16 pulse arrives, so the reverb blooms between them.
            {
                const float x2 = 0.5f * (L[s] * L[s] + R[s] * R[s]);
                detRms += (x2 - detRms) * detRmsA;
                const float lvl = std::sqrt (detRms);
                detEnv += (lvl - detEnv) * (lvl > detEnv ? detAtkA : detRelA);
                detRef = juce::jmax (detEnv, detRef * detRefDecay);
                const float ratio = detRef > 1.0e-6f ? detEnv / detRef : 0.0f;
                duckGain += ((1.0f - duckAmt * ratio) - duckGain) * duckSmA;
            }
            // pre-delay (glided, so changes bend instead of click)
            curPre += (tgtPre - curPre) * preA;
            float in[2];
            {
                float rp = (float) prePos - curPre;
                if (rp < 0.0f) rp += (float) preLen;
                const int i0 = (int) rp, i1 = (i0 + 1) & preMask;
                const float fr = rp - (float) i0;
                for (int c = 0; c < 2; ++c)
                {
                    const float a0 = pre[c][(size_t) (i0 & preMask)], a1 = pre[c][(size_t) i1];
                    in[c] = a0 + (a1 - a0) * fr;
                }
                inFade = juce::jmin (1.0f, inFade + fadeStep);
                pre[0][(size_t) prePos] = L[s] * inFade;
                pre[1][(size_t) prePos] = R[s] * inFade;
                prePos = (prePos + 1) & preMask;
                e = juce::jmax (e, std::abs (L[s]) + std::abs (R[s]));
            }
            // input: low cut, bandwidth, diffusion (different lengths left and right, so the channels decorrelate early)
            for (int c = 0; c < 2; ++c)
            {
                inLowCut[c] += inLowCutA * (in[c] - inLowCut[c]);
                float x = in[c] - inLowCut[c];
                inBand[c] += inBandA * (x - inBand[c]);
                x = inBand[c];
                for (int k = 0; k < numDiffusers; ++k) x = diffuser[c][k].process (x, inDiffGain);
                in[c] = x;
            }

            // modulation: four quadrature oscillators at slightly different rates give every line its own phase
            float m[N];
            for (int j = 0; j < numLfos; ++j)
            {
                lfo[j].s += lfoW[j] * lfo[j].c;
                lfo[j].c -= lfoW[j] * lfo[j].s;
                m[2 * j] = lfo[j].s;
                m[2 * j + 1] = lfo[j].c;
            }

            // read the lines (fractional delay, linear interpolation): scalar gathers into an aligned array, then the
            // arithmetic across the lines in 4-wide registers
            alignas (16) float v[N], y[N], d1[N], w1[N];
            for (int i = 0; i < N; ++i)
            {
                const float d = len[i] + depth * (1.0f + m[i]);
                len[i] += dLen[i];
                const int mk = mask[i];
                const float rp = (float) ((writePos & mk) + (mk + 1)) - d;   // one length ahead, so it never goes negative
                const int i0 = (int) rp;
                const float fr = rp - (float) i0;
                const float* b = lineBuf[i];
                const float a0 = b[i0 & mk], a1 = b[(i0 + 1) & mk];
                // reading i0+1 is one sample *less* delay; interpolate between the two neighbours
                v[i] = a0 + (a1 - a0) * fr;
            }
            // two short allpasses per line build density quickly (the second with the opposite sign)
            for (int i = 0; i < N; ++i) d1[i] = ap1[i][(apPos - ap1Len[i]) & ap1Mask[i]];
            for (int h = 0; h < 2; ++h)
            {
                const V4 vv = V4::fromRawArray (v + 4 * h), dd = V4::fromRawArray (d1 + 4 * h);
                const V4 ww = vv + dd * apG;
                ww.copyToRawArray (w1 + 4 * h);
                (dd - ww * apG).copyToRawArray (v + 4 * h);
            }
            for (int i = 0; i < N; ++i) ap1[i][apPos & ap1Mask[i]] = w1[i];
            for (int i = 0; i < N; ++i) d1[i] = ap2[i][(apPos - ap2Len[i]) & ap2Mask[i]];
            for (int h = 0; h < 2; ++h)
            {
                const V4 vv = V4::fromRawArray (v + 4 * h), dd = V4::fromRawArray (d1 + 4 * h);
                const V4 ww = vv - dd * apG;
                ww.copyToRawArray (w1 + 4 * h);
                (dd + ww * apG).copyToRawArray (v + 4 * h);
            }
            for (int i = 0; i < N; ++i) ap2[i][apPos & ap2Mask[i]] = w1[i];
            ++apPos;
            // high shelf (above Low Pass: extra loss every pass), DC blocker, low shelf (below the corner: extra loss,
            // no rumble build-up), decay gain
            for (int h = 0; h < 2; ++h)
            {
                V4 x = V4::fromRawArray (v + 4 * h);
                V4 t = hb0 * x + hiZ0[h];
                hiZ0[h] = hb1 * x - ha1 * t + hiZ1[h];
                hiZ1[h] = hb2 * x - ha2 * t;
                x = hg * x + hg1 * t;
                lowCut[h] = lowCut[h] + dcA * (x - lowCut[h]);
                x = x - lowCut[h];
                t = lb0 * x + loZ0[h];
                loZ0[h] = lb1 * x - la1 * t + loZ1[h];
                loZ1[h] = lb2 * x - la2 * t;
                x = lg * x + lg1 * t;
                (x * g[h]).copyToRawArray (y + 4 * h);
                g[h] = g[h] + dG[h];
            }

            // orthogonal (Hadamard) feedback plus input, written back into the lines
            float fb[N];
            hadamard (y, fb);
            for (int i = 0; i < N; ++i) lineBuf[i][writePos & mask[i]] = fb[i] + ((i & 1) ? in[1] : in[0]) * injSign[i];
            ++writePos;

            // stereo out: two orthogonal combinations of the line outputs, then width, ducking, level
            const V4 y0 = V4::fromRawArray (y), y1 = V4::fromRawArray (y + 4);
            float wl = (y0 * tapL[0] + y1 * tapL[1]).sum(), wr = (y0 * tapR[0] + y1 * tapR[1]).sum();
            const float mid = 0.5f * (wl + wr), side = 0.5f * (wl - wr) * width;
            const float g = wetGain * duckGain;
            wl = (mid + side) * g;
            wr = (mid - side) * g;

            L[s] = L[s] * (1.0f - mix) + wl * mix;
            R[s] = R[s] * (1.0f - mix) + wr * mix;
            mix += dMix;
            e = juce::jmax (e, std::abs (wl) + std::abs (wr));
        }
        energy = e;
        writePos &= 0x3fffffff;
        apPos &= 0x3fffffff;
        // keep the oscillators on the unit circle
        for (auto& o : lfo)
        {
            const float r = 1.5f - 0.5f * (o.s * o.s + o.c * o.c);
            o.s *= r; o.c *= r;
        }
    }

public:
    // T60 actually applied, and the ducking gain (for tests and the UI)
    float getDecay() const { return curDecay; }
    float getDuckGain() const { return duckGain; }

private:
    // Schroeder allpass on a power-of-two ring (no wrap branch)
    struct Allpass
    {
        std::vector<float> buf;
        int len = 1, mask = 0, pos = 0;
        void prepare (int l) { len = juce::jmax (1, l); buf.assign ((size_t) juce::nextPowerOfTwo (len + 1), 0.0f); mask = (int) buf.size() - 1; pos = 0; }
        void clear() { std::fill (buf.begin(), buf.end(), 0.0f); pos = 0; }
        inline float process (float x, float g) noexcept
        {
            const float d = buf[(size_t) ((pos - len) & mask)];
            const float w = x + g * d;
            buf[(size_t) (pos & mask)] = w;
            pos = (pos + 1) & mask;   // masked: never overflows, however long the session
            return d - g * w;
        }
    };

    // Shelving cut built on a Butterworth crossover: the band to keep (below the corner for the high shelf, above it
    // for the low shelf) passes at 1, the rest at the cut gain, with a two-octave transition and no loss further away.
    // The per-line state lives outside, so one set of coefficients serves every line.
    struct Shelf
    {
        float g = 1.0f, b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
        void set (bool high, float hz, float gainLin, double sr)
        {
            g = juce::jlimit (0.0f, 1.0f, gainLin);
            const float w = juce::MathConstants<float>::twoPi * juce::jmin (hz, (float) sr * 0.45f) / (float) sr;
            const float cw = std::cos (w), alpha = std::sin (w) / std::sqrt (2.0f);
            const float a0 = 1.0f + alpha;
            const float s = high ? 1.0f : -1.0f;   // the kept band: low pass for the high shelf, high pass for the low one
            b0 = 0.5f * (1.0f - s * cw) / a0;
            b1 = s * (1.0f - s * cw) / a0;
            b2 = b0;
            a1 = -2.0f * cw / a0;
            a2 = (1.0f - alpha) / a0;
        }
        std::complex<float> response (const std::complex<float>& z1) const
        {
            const auto z2 = z1 * z1;
            return g + (1.0f - g) * (b0 + b1 * z1 + b2 * z2) / (1.0f + a1 * z1 + a2 * z2);
        }
    };

    static void hadamard (const float* x, float* out) noexcept
    {
        // fast Walsh-Hadamard transform, scaled to be orthonormal
        float t[N];
        for (int i = 0; i < N; ++i) t[i] = x[i];
        for (int h = 1; h < N; h *= 2)
            for (int i = 0; i < N; i += 2 * h)
                for (int j = i; j < i + h; ++j)
                {
                    const float a = t[j], b = t[j + h];
                    t[j] = a + b;
                    t[j + h] = a - b;
                }
        const float scale = 1.0f / std::sqrt ((float) N);
        for (int i = 0; i < N; ++i) out[i] = t[i] * scale;
    }

    // Per-character internals; the live values glide towards these.
    // hiRatio / loRatio: how many times faster than Decay the highs (above Low Pass) and the lows (below ~200 Hz) die
    struct Internals { float inDiff, loopAp, sizeScale, modScale, hiRatio, loRatio, bandHz; };
    static Internals internals (int c)
    {
        switch (c)
        {
            case P::RTight:     return { 0.55f, 0.40f, 0.60f, 0.7f, 4.0f, 2.5f, 16000.0f };
            case P::REndless:   return { 0.70f, 0.55f, 1.12f, 1.0f, 4.0f, 1.8f, 15000.0f };
            case P::RDarkBloom: return { 0.72f, 0.55f, 1.05f, 0.9f, 8.0f, 1.5f, 9000.0f };
            case P::RBright:    return { 0.60f, 0.48f, 0.95f, 1.0f, 3.0f, 2.2f, 20000.0f };
            case P::RModulated: return { 0.66f, 0.50f, 1.00f, 1.8f, 5.0f, 2.0f, 15000.0f };
            case P::RPsy:       return { 0.74f, 0.58f, 1.08f, 2.4f, 4.0f, 2.0f, 17000.0f };
            default:            return { 0.66f, 0.50f, 1.00f, 1.0f, 5.0f, 2.0f, 15000.0f };   // Clean Hall
        }
    }

    void updateTargets (const Params& p, int n)
    {
        // glide over ~60 ms (per block): parameter and character changes never jump
        const float k = primed ? 1.0f - std::exp (-(float) n / (0.06f * (float) sr)) : 1.0f;
        const auto iv = internals (juce::jlimit (0, P::numRevCharacters - 1, p.character));
        auto glide = [k] (float& v, float target) { v += (target - v) * k; };
        glide (mInDiff, iv.inDiff); glide (mLoopAp, iv.loopAp); glide (mSizeScale, iv.sizeScale);
        glide (mModScale, iv.modScale); glide (mHiRatio, iv.hiRatio); glide (mLoRatio, iv.loRatio); glide (mBand, iv.bandHz);
        glide (sDecay, juce::jlimit (0.1f, 30.0f, p.decay));
        // the filters glide on a log scale
        glide (sLogLp, std::log (juce::jlimit (200.0f, 20000.0f, p.lpHz)));
        glide (sLogHp, std::log (juce::jlimit (20.0f, 2000.0f, p.hpHz)));
        const float lpHz = std::exp (sLogLp), hpHz = std::exp (sLogHp);
        glide (sMod, juce::jlimit (0.0f, 1.0f, p.mod));
        glide (sLogRate, std::log (juce::jlimit (0.02f, 8.0f, p.modRateHz)));
        glide (sWidth, juce::jlimit (0.0f, 1.0f, p.width));
        glide (sDuck, juce::jlimit (0.0f, 1.0f, p.duck));
        // size glides slower (~250 ms): changing delay lengths bends pitch while it moves
        const float ks = primed ? 1.0f - std::exp (-(float) n / (0.25f * (float) sr)) : 1.0f;
        sSize += (juce::jlimit (0.0f, 1.0f, p.size) - sSize) * ks;
        tgtMix = juce::jlimit (0.0f, 1.0f, p.mix);
        if (! primed) curMix = tgtMix;

        inDiffGain = mInDiff;
        loopApGain = mLoopAp;
        auto onePole = [this] (float hz) { return 1.0f - std::exp (-juce::MathConstants<float>::twoPi * juce::jmin (hz, (float) sr * 0.45f) / (float) sr); };
        inBandA = onePole (juce::jmin (mBand, lpHz * 3.0f));   // the input band follows Low Pass: a dark setting starts dark
        inLowCutA = onePole (hpHz);
        lowCutA = onePole (10.0f);   // DC blocker only: the low shelf (which follows High Pass) does the tail's low cut
        modDepth = sMod * mModScale * baseModDepth * rateScale;
        const float rate = std::exp (sLogRate);
        for (int j = 0; j < numLfos; ++j)
            lfoW[j] = juce::MathConstants<float>::twoPi * rate * lfoRatio[j] / (float) sr;

        // Size: 0..1 -> 0.28..1 of the longest delay set (times the character's scale)
        const float scale = (0.28f + 0.72f * sSize) * mSizeScale;
        curDecay = sDecay;
        // g = 10^(-3 * loopTime / T60); the loop is the delay line plus its allpasses. The shelves take the highs and
        // lows to g^ratio per pass (T60 / ratio), set from the average loop so one coefficient set serves every line.
        float loop[N], meanLoop = 0.0f;
        for (int i = 0; i < N; ++i)
        {
            tgtLen[i] = (float) (lineMs[i] * 0.001 * sr) * scale;
            if (! primed) curLen[i] = tgtLen[i];
            if (! primed) curGain[i] = -1.0f;
            loop[i] = tgtLen[i] + modDepth + (float) (ap1Len[i] + ap2Len[i]);
            meanLoop += loop[i] / (float) N;
        }
        const float gMean = std::pow (10.0f, -3.0f * meanLoop / (sDecay * (float) sr));
        // (the shelves cut at most 20 dB: for a very short Decay the highs and lows cannot sensibly die any faster, and a
        // deeper, fast-moving cut would be a transient of its own)
        hi.set (true, lpHz, juce::jmax (0.1f, std::pow (gMean, mHiRatio - 1.0f)), sr);
        lo.set (false, juce::jlimit (150.0f, 2000.0f, 2.2f * hpHz), juce::jmax (0.1f, std::pow (gMean, mLoRatio - 1.0f)), sr);   // just above the high pass
        // the filters' peak response (60 Hz .. 8 kHz) is made up in the gain: the band that rings longest gets exactly
        // T60 = Decay, everything else is shorter, and the net loop gain stays below 1
        float maxMag = 1.0e-3f;
        for (int f = 0; f < 16; ++f)
        {
            const float hz = 60.0f * std::pow (8000.0f / 60.0f, (float) f / 15.0f);
            const std::complex<float> z1 = std::polar (1.0f, -juce::MathConstants<float>::twoPi * hz / (float) sr);
            const auto lowCutH = 1.0f - std::complex<float> (lowCutA) / (1.0f - (1.0f - lowCutA) * z1);
            maxMag = juce::jmax (maxMag, std::abs (hi.response (z1) * lowCutH * lo.response (z1)));
        }
        // ...and the linear interpolation's average loss at 1 kHz (reading between two samples), so Decay is the
        // T60 at 1 kHz
        const float interp = std::sqrt (1.0f - (1.0f - std::cos (juce::MathConstants<float>::twoPi * 1000.0f / (float) sr)) / 3.0f);
        for (int i = 0; i < N; ++i)
        {
            gain[i] = juce::jmin (0.9995f, std::pow (10.0f, -3.0f * loop[i] / (sDecay * (float) sr))) / (maxMag * interp);
            if (curGain[i] < 0.0f) curGain[i] = gain[i];
        }
        // the wet level grows with the decay; keep long settings from getting much louder than short ones
        wetGain = wetBase / std::sqrt (1.0f + 0.15f * sDecay);
        primed = true;
    }

    static constexpr int numDiffusers = 4, numLfos = 4, subBlock = 128;
    static constexpr float maxSizeScale = 1.12f;
    static constexpr float baseModDepth = 12.0f;                   // samples at 44.1 kHz for mod 100 %, scale 1
    static constexpr float maxModDepth = baseModDepth * 2.4f;      // Psy Space's scale
    static constexpr float wetBase = 2.4f;   // matched to the previous reverb's level at the riser setting (100 % size, 6.7 s)
    static constexpr double diffuserMs[2][numDiffusers] { { 4.77, 3.59, 9.33, 12.71 }, { 4.31, 3.17, 8.71, 13.37 } };
    // line lengths at full size (ms): spread over more than an octave, no two in a simple ratio, long enough for a
    // dense mode pattern without becoming audible echoes
    static constexpr double lineMs[N] { 43.7, 97.3, 61.1, 79.9, 53.3, 89.3, 71.7, 67.1 };
    static constexpr double loopApMs[N] { 7.9, 5.3, 9.7, 6.7, 8.3, 4.9, 10.3, 6.1 };
    static constexpr double loopAp2Ms[N] { 3.1, 4.3, 2.3, 3.7, 2.9, 4.1, 2.6, 3.4 };
    static constexpr float lfoRatio[numLfos] { 1.0f, 0.83f, 1.19f, 1.47f };
    // output taps: two orthogonal sign patterns (mono sum keeps half of the lines, so it never cancels out)
    alignas (16) static constexpr float outL[N] { 0.5f, 0.5f, -0.5f, -0.5f, 0.5f, 0.5f, -0.5f, -0.5f };
    alignas (16) static constexpr float outR[N] { 0.5f, -0.5f, 0.5f, -0.5f, 0.5f, -0.5f, 0.5f, -0.5f };
    static constexpr float injGain = 0.5f;
    static constexpr float injSign[N] { injGain, injGain, -injGain, -injGain, injGain, injGain, -injGain, -injGain };

    double sr = 44100.0;
    float rateScale = 1.0f;
    Allpass diffuser[2][numDiffusers];
    std::vector<float> lines[N], ap1Buf[N], ap2Buf[N];
    float* lineBuf[N] {};
    float* ap1[N] {};
    float* ap2[N] {};
    int writePos = 0, apPos = 0, mask[N] {};   // rings are powers of two long, so (pos & mask) indexes each
    int ap1Len[N] {}, ap1Mask[N] {}, ap2Len[N] {}, ap2Mask[N] {};
    float gain[N] {}, curGain[N] {};
    using V4 = juce::dsp::SIMDRegister<float>;
    V4 lowCut[2], hiZ0[2], hiZ1[2], loZ0[2], loZ1[2];   // filter states, lines 0-3 and 4-7
    Shelf hi, lo;
    float inLowCut[2] {}, inBand[2] {};
    struct Osc { float s, c; } lfo[numLfos] {};
    float lfoW[numLfos] {};
    float curLen[N] {}, tgtLen[N] {};
    float curMix = 0.0f, tgtMix = 0.0f;
    std::vector<float> pre[2];
    int preLen = 0, prePos = 0;
    float curPre = -1.0f, preA = 0.001f, energy = 0.0f, inFade = 1.0f;
    std::vector<std::pair<float*, int>> spans;   // every buffer, built in prepare()
    int clearSpan = -1, clearOffset = 0;           // the piecewise clear's position (-1: not clearing)
    float inDiffGain = 0.66f, loopApGain = 0.5f, inBandA = 0.9f, inLowCutA = 0.01f, lowCutA = 0.005f;
    float modDepth = 0.0f, wetGain = 0.5f, curDecay = 3.0f;
    // ducking detector
    float detRmsA = 0.01f, detAtkA = 0.05f, detRelA = 0.001f, detRefDecay = 0.99999f, duckSmA = 0.01f;
    float detRms = 0.0f, detEnv = 0.0f, detRef = 0.0f, duckGain = 1.0f;
    // glided values
    float mInDiff = 0.66f, mLoopAp = 0.50f, mSizeScale = 1.0f, mModScale = 1.0f, mHiRatio = 5.0f, mLoRatio = 2.0f, mBand = 15000.0f;   // (Clean Hall)
    float sDecay = 3.0f, sMod = 0.25f, sSize = 0.65f, sLogLp = std::log (1200.0f), sLogHp = std::log (110.0f),
          sLogRate = std::log (0.5f), sWidth = 1.0f, sDuck = 0.0f;
    bool primed = false;
};
