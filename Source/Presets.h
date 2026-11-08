#pragma once
#include <JuceHeader.h>
#include "Params.h"

class ElukkaNoiseProcessor;

struct FactoryPreset
{
    juce::String name, category;
    std::vector<std::pair<juce::String, float>> values;   // parameter values that differ from the defaults
    int slots = 0;                                         // modulation matrix slots used so far
    juce::String delayPreset, reverbPreset;                // the effect presets it loads with (v0.9.21, by name)
    FactoryPreset& set (const juce::String& id, float v) { values.push_back ({ id, v }); return *this; }
};

// Factory presets (compiled in) and user presets (XML files in the per-user application data folder).
// Loading sets every instrument parameter and (v0.9.21) loads the delay and reverb presets the instrument preset names:
// it stores their names, never their contents (see FxPresets.h), so either effect can be changed afterwards without
// touching the instrument. MACRO (the host automates it) is left alone. A preset without the names (saved before
// v0.9.21) leaves the effects as they are; a name that no longer exists loads that effect's default preset.
class PresetManager
{
public:
    explicit PresetManager (ElukkaNoiseProcessor& p) : proc (p) {}

    static const juce::StringArray& categories();   // "Risers / Falls", "FX"
    static const std::vector<FactoryPreset>& factory();
    static juce::File userPresetDir();
    static juce::File overrideDir;   // tests: when set, user presets live here instead
    static constexpr const char* fileExtension = ".elukkanoise";
    // A preset name as it is stored: the file-name characters JUCE strips (" # @ , ; : < > * ^ | ? \ /) are removed,
    // leading dots too, and it is trimmed, so the name in the menu, in the file and on disk are always the same.
    static juce::String legalName (const juce::String& name)
    {
        auto n = juce::File::createLegalFileName (name.trim()).trimCharactersAtStart (".").trim();
        // Windows device names (CON, NUL, COM1, ...) cannot be files there: an underscore makes them ordinary
        static const juce::StringArray reserved { "CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
                                                  "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9" };
        if (reserved.contains (n.upToFirstOccurrenceOf (".", false, false).trim().toUpperCase())) n += "_";
        return n;
    }

    struct Entry { juce::String name, category; bool isFactory; int index; juce::File file; };
    std::vector<Entry> entries;
    int currentIndex = -1;
    juce::String currentName { "Clean Noise Rise" }, currentCategory { "Risers / Falls" };

    void refresh();
    void load (int entryIndex);
    bool loadByName (const juce::String& name, const juce::String& category);
    bool save (const juce::String& name, const juce::String& category);          // user preset (overwrites same name)
    bool deletePreset (int entryIndex);                                            // user presets only: moves the file to the Trash
    bool rename (int entryIndex, const juce::String& newName);                     // user presets only
    int indexOf (const juce::String& name, const juce::String& category, bool factoryOnly = false) const;

    // Preset contents as a tree (all parameters except MACRO, plus metadata)
    juce::ValueTree toTree (const juce::String& name, const juce::String& category) const;
    void applyTree (const juce::ValueTree& t);
    void applyFactory (const FactoryPreset& p);
    void applyFxRefs (const juce::String& delayName, const juce::String& reverbName);   // "" = leave that effect alone

private:
    ElukkaNoiseProcessor& proc;
};
