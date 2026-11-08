#pragma once
#include "PluginProcessor.h"
#include "UI/Widgets.h"
#include "UI/MacroKnob.h"
#include "UI/Pages.h"

class ElukkaNoiseEditor : public juce::AudioProcessorEditor, public DialogHost, private juce::Timer
{
public:
    explicit ElukkaNoiseEditor (ElukkaNoiseProcessor&);
    ~ElukkaNoiseEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    // One window size for both modes: ADVANCED folds the main knobs into a column on the left and shows the pages
    // on the right.
    static constexpr int baseWidth = 780, baseHeight = 450, headerHeight = 92;
    static constexpr int numPages = 5, leftColumnWidth = 84;
    juce::Rectangle<int> pagesArea() const;

private:
    void timerCallback() override;
    void setExpanded (bool expanded, bool keepScale);
    void showPage (int index);
    void updatePresetButton();
    void showPresetMenu (int categoryOnly);
    void stepPreset (int delta);
    void savePresetDialog();
    void renamePresetDialog();
    void deletePresetDialog();
    void reloadPreset();
    void showCredits();
    static juce::Image makeLogo();
    void paintView (juce::Graphics&);


    // Everything is laid out at the base size inside `view`, which is scaled to the window size.
    struct View : juce::Component
    {
        std::function<void (juce::Graphics&)> painter;
        void paint (juce::Graphics& g) override { if (painter) painter (g); }
    };
    struct LogoArea : juce::Component
    {
        std::function<void()> onClick;
        void mouseUp (const juce::MouseEvent& e) override { if (onClick && ! e.mods.isPopupMenu() && getLocalBounds().contains (e.getPosition())) onClick(); }
    };

    ElukkaNoiseProcessor& proc;
    NoiseLookAndFeel lnf;
    juce::TooltipWindow tooltips { this, 700 };
    juce::Image logo, grain;
    View view;
    LogoArea logoArea;
    bool expanded = false;
    bool hostWindowKept = false;

    MacroKnob macro;
    Knob noiseKnob, moveKnob, delayKnob, reverbKnob, hpKnob;
    Knob macroSmall;   // the macro as a normal-size knob in the advanced column
    juce::TextButton catButtons[2];
    juce::TextButton prevBtn { "<" }, nextBtn { ">" }, presetButton, saveBtn { "Save" }, renameBtn { "Rename" }, deleteBtn { "Delete" }, reloadBtn { "Reload" };
    juce::TextButton advancedBtn;
    Led midiLed;
    Meter meter;
    juce::Label midiLabel;

    juce::TextButton tabs[numPages];
    std::unique_ptr<Page> pages[numPages];
    int currentPage = 0;
    int lastUiVersion = -1;
};
