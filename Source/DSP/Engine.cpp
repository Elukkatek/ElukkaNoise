#include "Engine.h"

void NoiseEngine::attach (juce::AudioProcessorValueTreeState& s)
{
    auto r = [&] (const juce::String& id) { auto* p = s.getRawParameterValue (id); jassert (p != nullptr); return p; };
    raw.macro = r ("macro"); raw.fxOrder = r ("fx_order"); raw.macroLo = r ("macro_lo"); raw.macroHi = r ("macro_hi"); raw.compMakeup = r ("comp_makeup"); raw.compOn = r ("comp_on"); raw.noiseStereo = r ("noise_stereo"); raw.noiseVary = r ("noise_vary");
    raw.fltType = r ("flt_type"); raw.fltSlope = r ("flt_slope"); raw.flt2Type = r ("flt2_type"); raw.flt2Slope = r ("flt2_slope");
    for (int i = 0; i < 2; ++i)
    {
        const juce::String p = "l" + juce::String (i + 1) + "_";
        raw.lWave[i] = r (p + "wave"); raw.lSync[i] = r (p + "sync"); raw.lDiv[i] = r (p + "div"); raw.lPhase[i] = r (p + "phase");
        raw.lRetrig[i] = r (p + "retrig"); raw.lSmooth[i] = r (p + "smooth");
    }
    raw.shSync = r ("sh_sync"); raw.shDiv = r ("sh_div"); raw.shSmooth = r ("sh_smooth"); raw.shRetrig = r ("sh_retrig");
    raw.gateOn = r ("gate_on"); raw.gateRetrig = r ("gate_retrig"); raw.gateDiv = r ("gate_div"); raw.gateDuty = r ("gate_duty");
    raw.gateAtt = r ("gate_att"); raw.gateRel = r ("gate_rel"); raw.gateHold = r ("gate_hold"); raw.gateDecay = r ("gate_decay");
    raw.gateSus = r ("gate_sus"); raw.gateSwing = r ("gate_swing"); raw.gateLen = r ("gate_len"); raw.gateRandom = r ("gate_random");
    raw.gateRateBase = r ("gate_rate");
    for (int i = 0; i < P::numGateSteps; ++i) raw.gateSteps[i] = r (P::gateStep (i));
    raw.dlyOn = r ("dly_on"); raw.dlySync = r ("dly_sync"); raw.dlyMode = r ("dly_mode"); raw.dlyTime = r ("dly_time"); raw.dlyDiv = r ("dly_division");
    raw.revOn = r ("rev_on");
    for (int i = 0; i < P::numDlyMapDests; ++i) { raw.dlyBase[i] = r (P::dlyMapDests[i].paramID); dlyRanges[i] = s.getParameter (P::dlyMapDests[i].paramID)->getNormalisableRange(); }
    for (int i = 0; i < P::numRevMapDests; ++i) { raw.revBase[i] = r (P::revMapDests[i].paramID); revRanges[i] = s.getParameter (P::revMapDests[i].paramID)->getNormalisableRange(); }
    for (auto& v : fxLastRaw) v = -1.0e30f;
    raw.envAttack = r ("env_attack"); raw.envRelease = r ("env_release"); raw.velSens = r ("vel_sens"); raw.retrig = r ("retrig");
    for (int d = 0; d < P::NumDst; ++d)
    {
        raw.base[d] = d == P::DAmp ? nullptr : r (P::dests[d].paramID);
        if (d != P::DAmp) ranges[d] = s.getParameter (P::dests[d].paramID)->getNormalisableRange();
        lastRaw[d] = -1.0e30f;
    }
    for (int k = 0; k < P::numMatrixSlots; ++k)
    {
        raw.mSrc[k] = r (P::mx (k, "src")); raw.mCrv[k] = r (P::mx (k, "crv")); raw.mAmt[k] = r (P::mx (k, "amt"));
        raw.mDst[k] = r (P::mx (k, "dst")); raw.mAux[k] = r (P::mx (k, "aux"));
    }
}

void NoiseEngine::prepare (double sampleRate)
{
    sr = sampleRate;
    noise.prepare (sr);
    filter.prepare (sr);
    filter2.prepare (sr);
    delay.prepare (sr);
    reverb.prepare (sr);
    comp.prepare (sr);
    reset();
}

void NoiseEngine::reseedAll()
{
    noise.reseed (NoiseGen::defaultSeed + seedBase);
    lfo[0].prepare (sr, 11u + seedBase);
    lfo[1].prepare (sr, 23u + seedBase);
    sh.prepare (sr, 37u + seedBase);
    gate.prepare (sr, 41u + seedBase);
}

void NoiseEngine::reset()
{
    activeOrder = (int) raw.fxOrder->load();   // a (re)loaded project starts in its order, without a fade
    fxFade = 1.0f;
    reseedAll();
    filter.reset();
    filter2.reset();
    hp.reset();
    delay.reset();
    reverb.reset();
    comp.reset();
    numHeld = 0;
    env = 0.0f; envTarget = 0.0f;
    voiceIdle = true;
    primed = false;
    dlyRun = revRun = false;
    dlyWasRunning = revWasRunning = false;
    dlyWetSm = revWetSm = 0.0f;
    for (auto& v : off) v = 0.0f;
    lfoVal[0] = lfoVal[1] = shVal = 0.0f;
    uiNoteOn = false;
}

float NoiseEngine::norm (int d)
{
    if (d == P::DAmp) return 1.0f;
    const float v = raw.base[d]->load();
    if (v != lastRaw[d])   // the conversion has a pow(): only redo it when the knob moved
    {
        lastRaw[d] = v;
        lastNorm[d] = ranges[d].convertTo0to1 (juce::jlimit (ranges[d].start, ranges[d].end, v));
    }
    return lastNorm[d];
}

void NoiseEngine::noteOn (int note, float vel)
{
    note = juce::jlimit (0, 127, note);
    for (int i = 0; i < numHeld; ++i)
        if (held[i] == note) { for (int j = i; j < numHeld - 1; ++j) held[j] = held[j + 1]; --numHeld; break; }
    held[numHeld++] = note;
    velocity = juce::jlimit (0.0f, 1.0f, vel);
    if (! voiceIdle && numHeld > 1) {}   // legato over a held note: nothing restarts unless Retrigger says so

    const bool retrig = raw.retrig->load() > 0.5f;
    if (retrig || voiceIdle)
    {
        for (int i = 0; i < 2; ++i)
        {
            LFO::Params p;
            p.retrig = raw.lRetrig[i]->load() > 0.5f;
            p.phaseStart = raw.lPhase[i]->load();
            lfo[i].trigger (p);
        }
        LFO::Params sp;
        sp.retrig = raw.shRetrig->load() > 0.5f;
        sh.trigger (sp);
    }
    // gate note retrigger: step 1 starts on this note (its song position is taken by the next control block)
    if (raw.gateRetrig->load() > 0.5f)
    {
        gate.trigger (true);
        gateOriginPending = true;
    }
    // Vary off: every note starts the same noise burst. Vary on: the noise just runs on (random phase).
    if (raw.noiseVary->load() < 0.5f) noise.reseed (NoiseGen::defaultSeed + seedBase);
    envTarget = 1.0f;
    voiceIdle = false;
    uiNoteOn = true;
}

void NoiseEngine::noteOff (int note)
{
    for (int i = 0; i < numHeld; ++i)
        if (held[i] == note) { for (int j = i; j < numHeld - 1; ++j) held[j] = held[j + 1]; --numHeld; break; }
    if (numHeld == 0) { envTarget = 0.0f; uiNoteOn = false; }
}

void NoiseEngine::allNotesOff()
{
    numHeld = 0;
    envTarget = 0.0f;
    uiNoteOn = false;
}

void NoiseEngine::allSoundOff()
{
    allNotesOff();
    env = 0.0f;
    voiceIdle = true;
    filter.reset(); filter2.reset(); hp.reset();
    delay.reset(); reverb.reset(); comp.reset();   // the tails too (a full clear: this is the one place a step is wanted)
    dlyWasRunning = revWasRunning = false;
}

void NoiseEngine::control (int n, const Transport& t)
{
    const float fn = (float) n;
    auto coef = [&] (float seconds) { return primed ? 1.0f - std::exp (-fn / (seconds * (float) sr)) : 1.0f; };
    const float kBase = coef (0.010f), kMacro = coef (0.015f), kFast = coef (0.004f);

    // MACRO, mapped into its range (floor .. ceiling) and smoothed so stepped host automation becomes a continuous move
    const float lo = raw.macroLo->load(), hi = raw.macroHi->load();
    macroSm += ((lo + (hi - lo) * raw.macro->load() * 0.01f) * 0.01f - macroSm) * kMacro;

    // Modulation matrix. Sources: this block's macro and velocity, the LFO / S&H values from the previous control block
    // (16 samples ago: inaudible, and it lets LFOs modulate each other).
    const float srcVal[P::NumSrc] { 0.0f, macroSm, lfoVal[0], lfoVal[1], shVal, velocity };
    for (auto& v : off) v = 0.0f;
    for (int k = 0; k < P::numMatrixSlots; ++k)
    {
        const int src = (int) raw.mSrc[k]->load(), d = (int) raw.mDst[k]->load() - 1;
        const float amt = raw.mAmt[k]->load();
        if (src <= P::SNone || src >= P::NumSrc || d < 0 || d >= P::NumDst || amt == 0.0f) continue;
        float v = srcVal[src];
        const float c = raw.mCrv[k]->load();
        if (c != 0.0f) v = v < 0.0f ? -P::shapeMacro (-v, c) : P::shapeMacro (v, c);
        const int aux = (int) raw.mAux[k]->load();
        if (aux > P::SNone && aux < P::NumSrc)
            v *= juce::jlimit (0.0f, 1.0f, srcVal[aux]);
        off[d] += amt * v;
    }
    for (int d = 0; d < P::NumDst; ++d)
    {
        baseSm[d] += (norm (d) - baseSm[d]) * kBase;
        resolved[d] = juce::jlimit (0.0f, 1.0f, baseSm[d] + off[d]);
    }
    auto resolve = [&] (int d) { return resolved[d]; };
    auto value = [&] (int d) { return ranges[d].convertFrom0to1 (resolved[d]); };

    // LFOs and S&H
    const double beatHz = t.bpm / 60.0;
    for (int i = 0; i < 2; ++i)
    {
        LFO::Params p;
        p.wave = (int) raw.lWave[i]->load();
        const bool sync = raw.lSync[i]->load() > 0.5f;
        const int div = juce::jlimit (0, P::numSyncRates - 1, (int) raw.lDiv[i]->load());
        const int rd = i == 0 ? P::DLfo1Rate : P::DLfo2Rate, dd = i == 0 ? P::DLfo1Depth : P::DLfo2Depth;
        p.rateHz = sync ? beatHz / (double) P::syncRateBeats[div] : (double) value (rd);
        p.phaseStart = raw.lPhase[i]->load();
        p.retrig = raw.lRetrig[i]->load() > 0.5f;
        p.unipolar = true;   // 0..1, as in ElukkaSynth
        p.smoothMs = value (i == 0 ? P::DLfo1Smooth : P::DLfo2Smooth);
        if (sync && ! p.retrig && t.playing) p.lockedPhase = LFO::songPhase (t.ppq, (double) P::syncRateBeats[div], p.phaseStart);
        lfoVal[i] = lfo[i].process (p, n) * resolve (dd);
    }
    {
        LFO::Params p;
        p.wave = P::WSampleHold;
        p.unipolar = true;
        const bool sync = raw.shSync->load() > 0.5f;
        const int div = juce::jlimit (0, P::numSyncRates - 1, (int) raw.shDiv->load());
        p.rateHz = sync ? beatHz / (double) P::syncRateBeats[div] : (double) value (P::DShRate);
        p.retrig = raw.shRetrig->load() > 0.5f;
        p.smoothMs = value (P::DShSmooth);
        p.jump = 0.15f;   // fixed Jump: new values are pushed a little away from the last one (chosen by ear)
        if (sync && ! p.retrig && t.playing) p.lockedPhase = LFO::songPhase (t.ppq, (double) P::syncRateBeats[div], 0.0f);
        shVal = sh.process (p, n) * resolve (P::DShDepth);
    }

    // Resolve the voice destinations
    const float cutoffHz = value (P::DCutoff);
    const float hpHz = value (P::DHp);
    const float res = resolve (P::DRes), drive = resolve (P::DDrive), colour = resolve (P::DColour);
    const float cutoff2Hz = value (P::DCutoff2), res2 = resolve (P::DRes2), drive2 = resolve (P::DDrive2);
    const float pan = value (P::DPan);
    const float stereoVal = value (P::DNoiseStereo);   // 0..1: how independent the two channels' noise is; 1..2: extra width
    const float width = juce::jmax (1.0f, stereoVal);
    const float levelDb = value (P::DLevel);
    const float ampMod = juce::jlimit (0.0f, 1.0f, 1.0f + off[P::DAmp]);
    resolved[P::DAmp] = ampMod;

    velSm += ((1.0f - raw.velSens->load() * (1.0f - velocity)) - velSm) * kFast;
    float shared, own;
    NoiseGen::stereoWeights (juce::jmin (1.0f, stereoVal), shared, own);
    // Stereo pan (v0.9.24): the left and right channels are panned separately, each with a constant-power law. Panning
    // right slides the left channel across to the right edge while the right channel stays put (and the mirror image to
    // the left), so the whole image moves and stays wide instead of one side fading out. The two channels share some
    // noise (Stereo below 100 %) or are anti-correlated (the extra width above 100 %), so summing them into one side
    // changes the level; 'panComp' undoes that from the correlation those settings give (the filters treat both alike).
    const float posL = pan > 0.0f ? -1.0f + 2.0f * pan : -1.0f, posR = pan < 0.0f ? 1.0f + 2.0f * pan : 1.0f;
    const float aL = (posL + 1.0f) * 0.25f * juce::MathConstants<float>::pi, aR = (posR + 1.0f) * 0.25f * juce::MathConstants<float>::pi;
    const float midPow = 1.0f + shared, sidePow = (1.0f - shared) * width * width;
    const float corr = (midPow - sidePow) / (midPow + sidePow);
    const float panComp = 1.0f / std::sqrt (juce::jmax (0.25f, 1.0f + corr * std::cos (aL - aR)));

    rG.to (filter.gFor (cutoffHz), n);
    rK.to (NoiseFilter::kFor (res), n);
    rG2.to (filter2.gFor (cutoff2Hz), n);
    rK2.to (NoiseFilter::kFor (res2), n);
    rDrive2.to (drive2, n);
    rHpG.to (filter.gFor (hpHz), n);
    hpActive = hpHz > 21.0f || rHpG.cur > filter.gFor (21.0f);
    rDrive.to (drive, n);
    rColour.to (colour, n);
    rLL.to (std::cos (aL) * panComp, n); rLR.to (std::sin (aL) * panComp, n); rRL.to (std::cos (aR) * panComp, n); rRR.to (std::sin (aR) * panComp, n);
    rWidth.to (width, n);
    rAmp.to (juce::Decibels::decibelsToGain (levelDb) * velSm * ampMod, n);
    rShared.to (shared, n); rOwn.to (own, n);
    if (! primed)
        for (auto* rp : { &rG, &rK, &rG2, &rK2, &rF2, &rDrive2, &rHpG, &rDrive, &rColour, &rLL, &rLR, &rRL, &rRR, &rWidth, &rAmp, &rShared, &rOwn }) { rp->cur += rp->inc * fn; rp->inc = 0.0f; }

    // filter type / slope crossfades (~10 ms)
    const int type = juce::jlimit (0, 2, (int) raw.fltType->load());
    for (int i = 0; i < 3; ++i) typeMix[i] += ((i == type ? 1.0f : 0.0f) - typeMix[i]) * kBase;
    slopeMix += ((raw.fltSlope->load() < 0.5f ? 1.0f : 0.0f) - slopeMix) * kBase;
    const int type2 = juce::jlimit (0, 2, (int) raw.flt2Type->load());
    for (int i = 0; i < 3; ++i) typeMix2[i] += ((i == type2 ? 1.0f : 0.0f) - typeMix2[i]) * kBase;
    slopeMix2 += ((raw.flt2Slope->load() < 0.5f ? 1.0f : 0.0f) - slopeMix2) * kBase;
    // filter 2 is out of the path at its default (an open low pass, which would still take the top octave off white
    // noise): it crossfades in over ~10 ms when a knob or the matrix moves it, and out again (then its state is cleared)
    const bool f2Default = type2 == P::FLowPass && cutoff2Hz >= 19990.0f && res2 <= 0.0005f && drive2 <= 0.0005f;
    f2Mix += ((f2Default ? 0.0f : 1.0f) - f2Mix) * kBase;
    if (f2Default && f2Mix < 0.0005f) f2Mix = 0.0f;
    rF2.to (f2Mix, n);
    const bool f2Was = f2Active;
    f2Active = f2Mix > 0.0f || rF2.cur > 0.0f;
    if (f2Was && ! f2Active) filter2.reset();

    // envelope coefficients: attack reaches ~98 % in Attack, release falls ~43 dB in Release
    attA = 1.0f - std::exp (-4.0f / (value (P::DEnvAttack) * 0.001f * (float) sr));
    relA = 1.0f - std::exp (-5.0f / (value (P::DEnvRelease) * 0.001f * (float) sr));

    // gate
    TranceGate::Params gp;
    gp.on = raw.gateOn->load() > 0.5f;
    gp.retrig = raw.gateRetrig->load() > 0.5f;
    if (gateOriginPending) { gateOrigin = t.ppq; gateOriginPending = false; }
    gp.origin = gateOrigin;
    gp.depth = resolve (P::DGateDepth);
    gp.duty = value (P::DGateLen);
    gp.attackMs = value (P::DGateAtt);
    gp.holdMs = value (P::DGateHold);
    gp.decayMs = value (P::DGateDecay);
    gp.sustain = value (P::DGateSus);
    gp.releaseMs = value (P::DGateRel);
    gp.swing = value (P::DGateSwing);
    gp.length = (int) raw.gateLen->load();
    gp.random = raw.gateRandom->load() > 0.5f;
    for (int i = 0; i < P::numGateSteps; ++i) gateStepState[i] = (juce::uint8) juce::jlimit (0, 2, (int) raw.gateSteps[i]->load());
    gp.steps = gateStepState;

    gp.stepBeats = (double) P::gateDivBeats[juce::jlimit (0, P::gateDivNames.size() - 1, (int) raw.gateDiv->load())];
    {
        // Modulation of Gate Rate moves the division in whole octaves (1/8 -> 1/16 -> 1/32), so the steps stay on the
        // song grid while a riser speeds the gate up.
        const double ratio = (double) value (P::DGateRate) / juce::jmax (0.01, (double) raw.gateRateBase->load());
        const int octaves = juce::jlimit (-3, 4, (int) std::lround (std::log2 (juce::jmax (1.0e-6, ratio))));
        gp.stepBeats /= std::pow (2.0, (double) octaves);
    }
    gp.bpm = t.bpm;
    gp.ppq = t.playing ? t.ppq : -1.0;
    gate.beginBlock (gp, n);
    // The gate retriggers the noise like a note: with Vary off every opening step (not a tie) restarts the same noise
    // burst; with Vary on every step plays fresh, random noise. The colour filters (pink / brown) keep their state, so
    // nothing jumps while the gate is only part-closed.
    if (gp.on && gate.stepStarted() && raw.noiseVary->load() < 0.5f) noise.reseed (NoiseGen::defaultSeed + seedBase);

    // Effects: each knob smoothed (10 ms), then its effect preset's macro mapping on top in normalised space, then back to
    // a real value. The knobs themselves are never written, so sweeps cannot drift. (Their own smoothing handles the rest.)
    auto fx = [&] (int slot, std::atomic<float>* base, const juce::NormalisableRange<float>& rg, const P::FxMapLive* m, int d, std::atomic<float>& ui)
    {
        const float v = base->load();
        if (v != fxLastRaw[slot]) { fxLastRaw[slot] = v; fxLastNorm[slot] = rg.convertTo0to1 (juce::jlimit (rg.start, rg.end, v)); }
        fxSm[slot] += (fxLastNorm[slot] - fxSm[slot]) * kBase;
        const float eff = m != nullptr ? m->apply (d, fxSm[slot], macroSm) : fxSm[slot];
        ui.store (eff, std::memory_order_relaxed);
        return rg.convertFrom0to1 (eff);
    };
    float dv[P::numDlyMapDests], rv[P::numRevMapDests];
    for (int i = 0; i < P::numDlyMapDests; ++i) dv[i] = fx (i, raw.dlyBase[i], dlyRanges[i], dlyMap, i, uiDlyMap[i]);
    for (int i = 0; i < P::numRevMapDests; ++i) rv[i] = fx (P::maxFxMapDests + i, raw.revBase[i], revRanges[i], revMap, i, uiRevMap[i]);

    const bool dlyOn = raw.dlyOn->load() > 0.5f;
    dlyP.wet = dv[P::MdMix];
    dlyWetSm += ((dlyOn ? dlyP.wet : 0.0f) - dlyWetSm) * coef (0.02f);
    dlyP.wet = dlyWetSm;
    dlyP.feedback = dv[P::MdFb];
    dlyP.timeSeconds = raw.dlySync->load() > 0.5f
        ? (float) (P::dlyDivisionBeats[juce::jlimit (0, P::numDlyDivisions - 1, juce::roundToInt (raw.dlyDiv->load()))] / beatHz)
        : raw.dlyTime->load() * 0.001f;
    if (raw.dlySync->load() > 0.5f)   // a division the buffer cannot hold (1/1 below 57 bpm) drops an octave: still on the grid
        while (dlyP.timeSeconds > StereoDelay::maxSeconds) dlyP.timeSeconds *= 0.5f;
    dlyP.filter = dv[P::MdFilter]; dlyP.width = dv[P::MdWidth]; dlyP.offset = dv[P::MdOffset];
    dlyP.mod = dv[P::MdMod]; dlyP.modRateHz = P::modRateFor (dlyP.mod); dlyP.duck = dv[P::MdDuck];
    dlyP.mode = (int) raw.dlyMode->load();
    // An effect at wet 0 is skipped. When it goes idle its buffers are cleared (silently: its output is at 0 then), so
    // turning it up later never replays an old, frozen tail.
    dlyRun = (dlyOn && dv[P::MdMix] > 1.0e-4f) || dlyWetSm > 1.0e-5f;   // wanted (a mix above -80 dB) or still fading out
    if (! dlyRun && dlyWasRunning) delay.beginClear();          // cleared in pieces while idle: no big fill in one callback
    else if (dlyRun && ! dlyWasRunning) delay.finishClear();
    if (! dlyRun) delay.clearStep (n * 16);
    dlyWasRunning = dlyRun;

    const bool revOn = raw.revOn->load() > 0.5f;
    const float revWet = rv[P::MrMix];
    revWetSm += ((revOn ? revWet : 0.0f) - revWetSm) * coef (0.02f);
    revP.mix = revWetSm;
    revP.size = rv[P::MrSize];
    revP.decay = rv[P::MrDecay];
    revP.lpHz = P::reverbToneLp (rv[P::MrTone]);
    revP.hpHz = P::reverbToneHp (rv[P::MrTone]);
    revP.preDelayMs = rv[P::MrPre];
    revP.width = rv[P::MrWidth];
    revP.mod = rv[P::MrMod];
    revP.modRateHz = P::modRateFor (revP.mod);
    revP.duck = rv[P::MrDuck];
    revP.character = revMap != nullptr ? revMap->character.load() : (int) P::RHall;
    compP.amount = raw.compOn->load() > 0.5f ? resolve (P::DCompAmt) : 0.0f;   // off = amount 0 (glides, then skipped)
    compP.speed = resolve (P::DCompSpeed);
    compP.makeup = raw.compMakeup->load() > 0.5f;
    revRun = (revOn && revWet > 1.0e-4f) || revWetSm > 1.0e-5f;
    if (! revRun && revWasRunning) reverb.beginClear();
    else if (revRun && ! revWasRunning) reverb.finishClear();
    if (! revRun) reverb.clearStep (n * 16);
    revWasRunning = revRun;

    primed = true;
    uiMacro = macroSm;
    uiCutoff = resolve (P::DCutoff);
    uiPan = pan;
    uiWidth = width;
    uiLfo1 = lfoVal[0];
    uiLfo2 = lfoVal[1];
    uiLfoPhase[0] = (float) lfo[0].getPhase();
    uiLfoPhase[1] = (float) lfo[1].getPhase();
    uiSh = shVal;
}

void NoiseEngine::renderVoice (float* L, float* R, int n)
{
    const float* tm = typeMix;
    const float* tm2 = typeMix2;
    for (int i = 0; i < n; ++i)
    {
        float l, r;
        noise.next (rColour.cur, rShared.cur, rOwn.cur, l, r);
        if (hpActive)
        {
            l = hp.process (l, 0, rHpG.cur);
            r = hp.process (r, 1, rHpG.cur);
        }
        filter.process (l, r, rG.cur, rK.cur, rDrive.cur, tm, slopeMix);
        if (f2Active)
        {
            float fl = l, fr = r;
            filter2.process (fl, fr, rG2.cur, rK2.cur, rDrive2.cur, tm2, slopeMix2);
            l += (fl - l) * rF2.cur;
            r += (fr - r) * rF2.cur;
        }
        env += (envTarget - env) * (envTarget > env ? attA : relA);
        const float a = rAmp.cur * env * gate.next();
        const float mid = 0.5f * (l + r), side = 0.5f * (l - r) * rWidth.cur;
        const float wl = (mid + side) * a, wr = (mid - side) * a;
        L[i] = wl * rLL.cur + wr * rRL.cur;
        R[i] = wl * rLR.cur + wr * rRR.cur;
        rG.cur += rG.inc; rK.cur += rK.inc; rG2.cur += rG2.inc; rK2.cur += rK2.inc; rF2.cur += rF2.inc; rDrive2.cur += rDrive2.inc; rHpG.cur += rHpG.inc; rDrive.cur += rDrive.inc; rColour.cur += rColour.inc;
        rLL.cur += rLL.inc; rLR.cur += rLR.inc; rRL.cur += rRL.inc; rRR.cur += rRR.inc; rWidth.cur += rWidth.inc; rAmp.cur += rAmp.inc;
        rShared.cur += rShared.inc; rOwn.cur += rOwn.inc;
    }
}

void NoiseEngine::render (float* L, float* R, int n, const Transport& t)
{
    constexpr int blockSize = 16;
    int pos = 0;
    Transport tt = t;
    while (pos < n)
    {
        const int m = juce::jmin (blockSize, n - pos);
        tt.ppq = t.ppq + (double) pos / sr * t.bpm / 60.0;
        control (m, tt);
        if (voiceIdle)
        {
            // nothing sounding: keep the modulators running, output silence. The ramps still reach their targets, so a
            // note starts from the current cutoff, pan, gain ... (not from where they were when the last note ended).
            juce::FloatVectorOperations::clear (L + pos, m);
            juce::FloatVectorOperations::clear (R + pos, m);
            for (int i = 0; i < m; ++i) gate.next();
            for (auto* rp : { &rG, &rK, &rG2, &rK2, &rF2, &rDrive2, &rHpG, &rDrive, &rColour, &rLL, &rLR, &rRL, &rRR, &rWidth, &rAmp, &rShared, &rOwn }) rp->cur += rp->inc * (float) m;
            if (envTarget > 0.0f) voiceIdle = false;
        }
        else
        {
            renderVoice (L + pos, R + pos, m);
            if (envTarget <= 0.0f && env < 1.0e-5f) { voiceIdle = true; env = 0.0f; }
        }
        pos += m;
    }

    // Effects in the chosen order. A change of order fades both mixes out (12 ms), swaps, and fades them back in.
    const int order = (int) raw.fxOrder->load();
    const float fadeStep = (float) n / (0.012f * (float) sr);
    if (order != activeOrder)
    {
        fxFade = juce::jmax (0.0f, fxFade - fadeStep);
        if (fxFade <= 0.0f) activeOrder = order;
    }
    else fxFade = juce::jmin (1.0f, fxFade + fadeStep);
    auto dp = dlyP;
    auto rp = revP;
    dp.wet *= fxFade;
    rp.mix *= fxFade;
    // (with the delay and reverb mixes at 0 every order sounds the same, so the swap never jumps)
    const bool compRun = comp.isActive (compP);
    for (int k = 0; k < P::numFx; ++k)
        switch (P::fxOrders[juce::jlimit (0, 5, activeOrder)][k])
        {
            case 0: if (compRun) comp.process (L, R, n, compP); break;
            case 1: if (dlyRun) delay.process (L, R, n, dp); break;
            default: if (revRun) reverb.process (L, R, n, rp); break;
        }
    uiCompIn = compRun ? comp.uiIn : -100.0f;
    uiCompOut = compRun ? comp.uiOut : -100.0f;
    uiCompGr = compRun ? comp.uiGr : 0.0f;
    uiCompLevel = compRun ? comp.uiLevel : -100.0f;

    // output: gentle soft clip above 0.85 so resonant peaks never hard clip
    float peak = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        for (float* x : { L + i, R + i })
        {
            const float v = *x, a = std::abs (v);
            if (a > 0.85f) *x = (v < 0.0f ? -1.0f : 1.0f) * (0.85f + 0.15f * std::tanh ((a - 0.85f) / 0.15f));
            peak = juce::jmax (peak, std::abs (*x));
        }
    }
    uiLevel = peak;
    uiGate = gate.envelope();
    uiGateStep = gate.currentStep();
    uiGateRandomBits = (int) gate.currentRandomBits();
    for (int d = 0; d < P::NumDst; ++d) uiDest[d].store (resolved[d], std::memory_order_relaxed);
    uiEnv = env;
}
