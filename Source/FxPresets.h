#pragma once
#include <JuceHeader.h>
#include "Params.h"

class ElukkaNoiseProcessor;

// One effect preset (v0.9.20): the effect's knob values that differ from the defaults, how the shared MACRO moves each
// of its knobs (P::FxMapEntry per destination) and, for the reverb, the internals set (character). The delay preset
// defines the delay sound AND its macro behaviour; the reverb preset the reverb's. Instrument presets carry neither.
struct FxPreset
{
    juce::String name;
    std::vector<std::pair<juce::String, float>> values;
    P::FxMapEntry map[P::maxFxMapDests];
    bool mapInUnits[P::maxFxMapDests] {};   // a Range written in the knob's units (converted when applied)
    int character = P::RHall;
    FxPreset& set (const juce::String& id, float v) { values.push_back ({ id, v }); return *this; }
    FxPreset& add (int d, float depth, float curve = 0.0f) { map[d] = { true, P::MapAdd, 0.0f, depth, curve }; return *this; }
    FxPreset& range (int d, float from, float to, float curve = 0.0f) { map[d] = { true, P::MapRange, from, to, curve }; mapInUnits[d] = true; return *this; }
};

// The presets of one effect (delay or reverb): factory (compiled in) and user (XML files in <user presets>/Delay or
// /Reverb), the live mapping the engine reads, and its copy in the plugin state. Loading a preset sets only that
// effect's parameters, mapping and character: MACRO, the instrument and the other effect stay as they are.
class FxPresetManager
{
public:
    enum Kind { Delay, Reverb };
    FxPresetManager (ElukkaNoiseProcessor& p, Kind k, P::FxMapLive& l);

    static const std::vector<FxPreset>& factory (Kind k);
    static juce::String presetForOldCharacter (Kind k, int character);   // the v0.9.17 / v0.9.19 character menus
    static constexpr const char* fileExtension = ".elukkafx";
    juce::File userDir() const;

    struct Entry { juce::String name; bool isFactory; int index; juce::File file; };
    std::vector<Entry> entries;
    int currentIndex = -1;
    juce::String currentName;

    const Kind kind;
    int numDests() const { return kind == Delay ? P::numDlyMapDests : P::numRevMapDests; }
    const P::FxDest* dests() const { return kind == Delay ? P::dlyMapDests : P::revMapDests; }
    const char* prefix() const { return kind == Delay ? "dly_" : "rev_"; }
    const char* title() const { return kind == Delay ? "Delay" : "Reverb"; }

    void refresh();
    void load (int entryIndex);
    bool loadByName (const juce::String& name);
    bool save (const juce::String& name);          // user preset (overwrites the same name)
    bool deletePreset (int entryIndex);            // user presets only: the file goes to the Trash
    int indexOf (const juce::String& name, bool factoryOnly = false) const;
    bool isModified() const;   // knobs or mapping differ from the current preset as loaded / saved

    // The live mapping: the current preset's, possibly edited in the Macro window. Edits go to the engine and the state.
    P::FxMapEntry map[P::maxFxMapDests];
    int character = P::RHall;
    P::FxMapLive& live;
    void setMap (int d, const P::FxMapEntry& e);
    void restoreMap();                                    // back to the current preset's saved mapping
    void selectMappingOf (const juce::String& factoryName);   // mapping and character only, knobs untouched (old states)

    juce::ValueTree toTree (const juce::String& name) const;
    void applyTree (const juce::ValueTree& t);
    void applyFactory (const FxPreset& p);

    // The mapping and the preset name live in a child of the parameter state ("DelayMacro" / "ReverbMacro"), so they
    // travel with the project. readFromState() follows replaceState(); a state without the child gets the default.
    void writeToState();
    void readFromState();

private:
    void setMapping (const P::FxMapEntry* m, int ch);
    void push();   // map / character -> live atomics + state
    ElukkaNoiseProcessor& proc;
    juce::ValueTree loadedTree;   // the current preset as it was loaded or saved (isModified)
};
