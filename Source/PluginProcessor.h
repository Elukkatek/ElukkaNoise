#pragma once
#include <JuceHeader.h>
#include "Params.h"
#include "DSP/Engine.h"
#include "Presets.h"
#include "FxPresets.h"

class ElukkaNoiseProcessor : public juce::AudioProcessor
{
public:
    ElukkaNoiseProcessor();
    ~ElukkaNoiseProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "ElukkaNoise"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override;   // from the release, the reverb decay and the delay feedback

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // Everything the UI needs
    juce::AudioProcessorValueTreeState apvts;
    NoiseEngine engine;
    PresetManager presets;
    P::FxMapLive dlyMapLive, revMapLive;     // the effect presets' macro mappings as the engine reads them
    FxPresetManager dlyPresets, revPresets;  // delay and reverb presets (each with its macro mapping)
    std::atomic<int> uiVersion { 0 };   // bumped when a preset loads, so the editor refreshes
    void bumpUiVersion() { ++uiVersion; }
    float uiScale = 1.0f;
    bool uiExpanded = false;
    std::atomic<double> uiHostBpm { 120.0 };
    void seedRandom (juce::uint32 s) { engine.seed (s); }   // repeatable offline renders (tests)

private:
    void handleMidi (const juce::MidiMessage& m);
    double sr = 44100.0;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ElukkaNoiseProcessor)
};
