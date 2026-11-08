#include "PluginProcessor.h"
#include "PluginEditor.h"

ElukkaNoiseProcessor::ElukkaNoiseProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "ElukkaNoise", P::createLayout()),
      presets (*this), dlyPresets (*this, FxPresetManager::Delay, dlyMapLive), revPresets (*this, FxPresetManager::Reverb, revMapLive)
{
    engine.attach (apvts);
    engine.attachFxMaps (dlyMapLive, revMapLive);
    dlyPresets.refresh(); dlyPresets.load (0);   // the default effect presets (Clean Rhythmic, Clean Space)
    revPresets.refresh(); revPresets.load (0);
    presets.refresh();
    const int def = presets.indexOf ("Clean Noise Rise", "Risers / Falls", true);
    if (def >= 0) presets.load (def);
}

bool ElukkaNoiseProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::stereo() || out == juce::AudioChannelSet::mono();
}

void ElukkaNoiseProcessor::prepareToPlay (double sampleRate, int)
{
    sr = sampleRate;
    engine.prepare (sampleRate);
}

void ElukkaNoiseProcessor::handleMidi (const juce::MidiMessage& m)
{
    if (m.isNoteOn()) engine.noteOn (m.getNoteNumber(), m.getFloatVelocity());
    else if (m.isNoteOff()) engine.noteOff (m.getNoteNumber());
    else if (m.isAllSoundOff()) engine.allSoundOff();   // panic: silence at once
    else if (m.isAllNotesOff()) engine.allNotesOff();   // release as usual
}

double ElukkaNoiseProcessor::getTailLengthSeconds() const
{
    // What can still sound after the last note: the release, then the longer of the reverb's decay (T60 with a margin,
    // plus the pre-delay) and the delay's echoes down to -60 dB (feedback near 100 % sustains: capped at 30 s).
    auto v = [this] (const char* id) { return (double) apvts.getRawParameterValue (id)->load(); };
    double tail = v ("env_release") * 0.001 + 0.2;
    if (v ("rev_on") > 0.5) tail = juce::jmax (tail, 1.3 * v ("rev_decay") + v ("rev_pre") * 0.001 + 0.5);
    if (v ("dly_on") > 0.5)
    {
        const double fb = juce::jlimit (0.001, 0.97, v ("dly_fb"));
        const double time = v ("dly_sync") > 0.5
            ? P::dlyDivisionBeats[juce::jlimit (0, P::numDlyDivisions - 1, juce::roundToInt ((float) v ("dly_division")))] * 60.0 / uiHostBpm.load()
            : v ("dly_time") * 0.001;
        tail = juce::jmax (tail, time * std::log (0.001) / std::log (fb) + 0.5);
    }
    return juce::jmin (30.0, tail);
}

void ElukkaNoiseProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    if (n == 0) return;

    Transport t;
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
        {
            if (auto bpm = pos->getBpm()) t.bpm = juce::jlimit (20.0, 999.0, *bpm);
            if (auto ppq = pos->getPpqPosition()) t.ppq = *ppq;
            t.playing = pos->getIsPlaying();
        }
    uiHostBpm = t.bpm;

    // Render around the MIDI events so note timing is sample-accurate. The engine always renders two channels; a
    // mono output gets the sum (rendered in slices through a stack buffer, no allocation).
    float* L = buffer.getWritePointer (0);
    float* R = buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : nullptr;
    float monoScratch[1024];
    auto renderRange = [&] (int from, int to)
    {
        while (from < to)
        {
            const int m = R != nullptr ? to - from : juce::jmin (1024, to - from);
            Transport tt = t;
            tt.ppq = t.ppq + (double) from / sr * t.bpm / 60.0;
            float* r = R != nullptr ? R + from : monoScratch;
            engine.render (L + from, r, m, tt);
            if (R == nullptr)
                for (int i = 0; i < m; ++i) L[from + i] = 0.5f * (L[from + i] + r[i]);
            from += m;
        }
    };
    int pos = 0;
    for (const auto meta : midi)
    {
        const int at = juce::jlimit (pos, n, meta.samplePosition);
        if (at > pos) { renderRange (pos, at); pos = at; }
        handleMidi (meta.getMessage());
    }
    if (pos < n) renderRange (pos, n);
    for (int ch = 2; ch < buffer.getNumChannels(); ++ch) buffer.clear (ch, 0, n);
}

void ElukkaNoiseProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    // keep the note state and the modulators running so un-bypassing continues where it was; output silence
    processBlock (buffer, midi);
    buffer.clear();
}

void ElukkaNoiseProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    state.setProperty ("presetName", presets.currentName, nullptr);
    state.setProperty ("presetCategory", presets.currentCategory, nullptr);
    state.setProperty ("uiScale", uiScale, nullptr);
    state.setProperty ("uiExpanded", uiExpanded, nullptr);
    state.setProperty ("stateVersion", 3, nullptr);   // 2 = v0.9.20: effect presets with their macro mappings; 3 = v0.9.23: Stereo + Width, reverb Tone, no mod rates
    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void ElukkaNoiseProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
    {
        auto t = juce::ValueTree::fromXml (*xml);
        if (! t.isValid() || ! t.hasType (apvts.state.getType())) return;
        const int version = (int) t.getProperty ("stateVersion", 0);
        P::migrateOldDelayParams (t);                 // states saved before v0.9.19
        if (version < 2) P::migrateMatrixDestsV2 (t);   // before v0.9.20: the destination table shrank
        if (version < 3) { P::migrateMatrixDestsV3 (t); P::migrateStereoAndTone (t); }   // before v0.9.23: Width -> Noise Stereo, Low Pass / High Pass -> Tone
        apvts.replaceState (t);
        dlyPresets.readFromState();
        revPresets.readFromState();
        if (version < 2)
        {
            // the old character menus pick the effect preset whose mapping the state gets (its knobs are in the state)
            for (const auto& c : t)
                if (c.hasType ("PARAM"))
                {
                    if (c["id"].toString() == "rev_char") revPresets.selectMappingOf (FxPresetManager::presetForOldCharacter (FxPresetManager::Reverb, juce::roundToInt ((float) c["value"])));
                    if (c["id"].toString() == "dly_char") dlyPresets.selectMappingOf (FxPresetManager::presetForOldCharacter (FxPresetManager::Delay, juce::roundToInt ((float) c["value"])));
                }
        }
        presets.currentName = t.getProperty ("presetName", presets.currentName).toString();
        presets.currentCategory = t.getProperty ("presetCategory", presets.currentCategory).toString();
        if (t.hasProperty ("uiScale")) uiScale = juce::jlimit (0.6f, 2.0f, (float) t["uiScale"]);
        uiExpanded = (bool) t.getProperty ("uiExpanded", false);
        presets.refresh();
        presets.currentIndex = presets.indexOf (presets.currentName, presets.currentCategory);
        bumpUiVersion();
    }
}

juce::AudioProcessorEditor* ElukkaNoiseProcessor::createEditor()
{
    return new ElukkaNoiseEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new ElukkaNoiseProcessor();
}
