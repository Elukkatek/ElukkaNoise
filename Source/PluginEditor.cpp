#include "PluginEditor.h"
#include "BinaryData.h"
#if JUCE_MAC
 #include "UI/HostWindow.h"
#endif

ElukkaNoiseEditor::ElukkaNoiseEditor (ElukkaNoiseProcessor& p)
    : AudioProcessorEditor (&p), proc (p), macro (p.apvts),
      noiseKnob (p.apvts, "noise_colour", "NOISE", "Noise colour: brown, pink or white"),
      moveKnob (p.apvts, "l1_depth", "MOVEMENT", "Depth of LFO 1 (the preset's main movement)"),
      delayKnob (p.apvts, "dly_wet", "DELAY", "Delay mix"),
      reverbKnob (p.apvts, "rev_wet", "REVERB", "Reverb mix"),
      hpKnob (p.apvts, "hp_cutoff", "HIGH PASS", "High pass filter: removes the lows (20 Hz = off). Filters the noise before the delay and reverb"),
      macroSmall (p.apvts, "macro", "MACRO", "MACRO: automate this. 0% to 100% is a riser, 100% to 0% a fall")
{
    setLookAndFeel (&lnf);
    logo = makeLogo();
    grain = makeGrain (baseWidth, baseHeight, 0.045f, 0.55f, 7u);   // header and knobs only: the pages stay plain
    view.painter = [this] (juce::Graphics& g) { paintView (g); };
    view.setOpaque (false);
    addAndMakeVisible (view);
    logoArea.setMouseCursor (juce::MouseCursor::PointingHandCursor);
    logoArea.onClick = [this] { showCredits(); };
    view.addAndMakeVisible (logoArea);

    view.addAndMakeVisible (macro);
    view.addChildComponent (macroSmall);
    for (auto* k : { &noiseKnob, &moveKnob, &delayKnob, &reverbKnob, &hpKnob })
    {
        view.addAndMakeVisible (k);
        if (const int d = P::destForParam (k->getName()); d >= 0) k->setLive (&p.engine.uiDest[d]);
    }
    // the DELAY and REVERB knobs are the effects' Mix: their macro mapping comes from the effect presets
    delayKnob.setFxMap (&p.engine.uiDlyMap[P::MdMix], [&p] { return p.dlyPresets.map[P::MdMix]; });
    reverbKnob.setFxMap (&p.engine.uiRevMap[P::MrMix], [&p] { return p.revPresets.map[P::MrMix]; });

    const char* catNames[] { "RISERS / FALLS", "FX" };
    for (int i = 0; i < 2; ++i)
    {
        catButtons[i].setButtonText (catNames[i]);
        catButtons[i].onClick = [this, i] { showPresetMenu (i); };
        catButtons[i].setTooltip (juce::String ("Browse the ") + catNames[i] + " presets");
        view.addAndMakeVisible (catButtons[i]);
    }
    presetButton.onClick = [this] { showPresetMenu (-1); };
    presetButton.setTooltip ("Current preset: click to browse all presets");
    prevBtn.onClick = [this] { stepPreset (-1); };
    nextBtn.onClick = [this] { stepPreset (1); };
    saveBtn.onClick = [this] { savePresetDialog(); };
    saveBtn.setTooltip ("Save the current sound as a user preset");
    renameBtn.onClick = [this] { renamePresetDialog(); };
    deleteBtn.onClick = [this] { deletePresetDialog(); };
    reloadBtn.onClick = [this] { reloadPreset(); };
    reloadBtn.setTooltip ("Restore the preset's saved values (factory presets cannot be changed, so this brings the original back)");
    for (auto* c : std::initializer_list<juce::Component*> { &presetButton, &prevBtn, &nextBtn, &saveBtn, &renameBtn, &deleteBtn, &reloadBtn, &midiLed, &meter, &midiLabel })
        view.addAndMakeVisible (c);
    midiLabel.setText ("MIDI", juce::dontSendNotification);
    midiLabel.setColour (juce::Label::textColourId, Col::dim);
    midiLabel.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
    midiLabel.setJustificationType (juce::Justification::centred);

    advancedBtn.setClickingTogglesState (true);
    advancedBtn.onClick = [this] { setExpanded (advancedBtn.getToggleState(), true); };
    advancedBtn.setTooltip ("Show the detailed controls");
    view.addAndMakeVisible (advancedBtn);

    const char* names[] { "NOISE & FILTER", "LFOs", "TRANCE GATE", "EFFECTS", "MOD MATRIX" };   // v0.9.22: OUTPUT folded into NOISE
    pages[0] = std::make_unique<NoiseFilterPage> (p);
    pages[1] = std::make_unique<LfoPage> (p);
    pages[2] = std::make_unique<GatePage> (p);
    pages[3] = std::make_unique<EffectsPage> (p);
    pages[4] = std::make_unique<MatrixPage> (p);
    for (int i = 0; i < numPages; ++i)
    {
        tabs[i].setButtonText (names[i]);
        tabs[i].setClickingTogglesState (true);
        tabs[i].setRadioGroupId (1001);
        tabs[i].onClick = [this, i] { showPage (i); };
        view.addChildComponent (tabs[i]);
        view.addChildComponent (*pages[i]);
    }
    tabs[0].setToggleState (true, juce::dontSendNotification);
    showPage (0);
    updatePresetButton();

    // Drag the corner to scale the UI between 60% and 200%; the size and the expanded state are saved with the project.
    expanded = proc.uiExpanded;
    advancedBtn.setToggleState (expanded, juce::dontSendNotification);
    setResizable (true, true);
    setExpanded (expanded, true);
    startTimerHz (30);
}

ElukkaNoiseEditor::~ElukkaNoiseEditor()
{
    closeAll();   // open dialogs and call-outs go with the window
    stopTimer();
    setLookAndFeel (nullptr);
}

void ElukkaNoiseEditor::setExpanded (bool e, bool keepScale)
{
    expanded = e;
    proc.uiExpanded = e;
    advancedBtn.setButtonText (juce::String::fromUTF8 (e ? "ADVANCED  \xe2\x96\xb4" : "ADVANCED  \xe2\x96\xbe"));
    advancedBtn.setTooltip (e ? "Back to the big macro" : "Show every control (the knobs move into a column on the left)");
    const float scale = keepScale ? juce::jlimit (0.6f, 2.0f, proc.uiScale) : 1.0f;
    setResizeLimits ((int) (baseWidth * 0.6), (int) (baseHeight * 0.6), baseWidth * 2, baseHeight * 2);
    getConstrainer()->setFixedAspectRatio ((double) baseWidth / (double) baseHeight);
    macro.setVisible (! e);
    macroSmall.setVisible (e);
    for (int i = 0; i < numPages; ++i)
    {
        tabs[i].setVisible (e);
        pages[i]->setVisible (e && i == currentPage);
    }
    const int w = juce::roundToInt (baseWidth * scale), h = juce::roundToInt (baseHeight * scale);
    if (getWidth() == w && getHeight() == h) resized(); else setSize (w, h);
    repaint();
}

juce::Image ElukkaNoiseEditor::makeLogo()
{
    // The artwork is black ink on white (alpha where present is opaque): ink darkness becomes alpha, the ink is
    // painted white, and the result is cropped to the ink.
    auto src = juce::ImageCache::getFromMemory (BinaryData::logo_png, BinaryData::logo_pngSize);
    if (! src.isValid()) return {};
    juce::Image out (juce::Image::ARGB, src.getWidth(), src.getHeight(), true);
    int x0 = src.getWidth(), y0 = src.getHeight(), x1 = 0, y1 = 0;
    for (int y = 0; y < src.getHeight(); ++y)
        for (int x = 0; x < src.getWidth(); ++x)
        {
            const auto c = src.getPixelAt (x, y);
            const float ink = (1.0f - c.getPerceivedBrightness()) * c.getFloatAlpha();
            const float a = juce::jlimit (0.0f, 1.0f, (ink - 0.12f) / 0.6f);
            if (a > 0.0f)
            {
                out.setPixelAt (x, y, Col::text.withAlpha (a));
                if (a > 0.3f) { x0 = juce::jmin (x0, x); y0 = juce::jmin (y0, y); x1 = juce::jmax (x1, x); y1 = juce::jmax (y1, y); }
            }
        }
    if (x1 <= x0 || y1 <= y0) return out;
    return out.getClippedImage ({ x0, y0, x1 - x0 + 1, y1 - y0 + 1 });
}

void ElukkaNoiseEditor::paint (juce::Graphics& g)
{
    g.fillAll (Col::bg);
    if (grain.isValid())
        g.drawImage (grain, getLocalBounds().toFloat(), juce::RectanglePlacement::stretchToFit);
    if (expanded)
    {
        // the pages sit on plain black (no grain), so they are easy to read
        const float scale = juce::jmin ((float) getWidth() / (float) baseWidth, (float) getHeight() / (float) baseHeight);
        g.setColour (Col::bg);
        g.fillRect (pagesArea().toFloat() * scale);
    }
}

juce::Rectangle<int> ElukkaNoiseEditor::pagesArea() const
{
    // in view coordinates: right of the knob column, below the header
    return { leftColumnWidth, headerHeight, baseWidth - leftColumnWidth, baseHeight - headerHeight };
}

void ElukkaNoiseEditor::paintView (juce::Graphics& g)
{
    auto header = view.getLocalBounds().removeFromTop (headerHeight);
    g.setColour (Col::border);
    g.drawHorizontalLine (header.getBottom() - 1, 0.0f, (float) baseWidth);
    if (logo.isValid())
        g.drawImage (logo, juce::Rectangle<float> (16.0f, 6.0f, 196.0f, 66.0f), juce::RectanglePlacement::centred | juce::RectanglePlacement::xLeft);
    g.setColour (Col::dim);
    g.setFont (juce::Font (juce::FontOptions (9.5f, juce::Font::bold)));
    g.drawText ("NOISE RISERS / FALLS / FX", 16, 75, 200, 12, juce::Justification::centredLeft);
    if (expanded)
    {
        g.setColour (Col::border);
        g.drawVerticalLine (leftColumnWidth - 1, (float) headerHeight, (float) baseHeight);
    }
}

void ElukkaNoiseEditor::resized()
{
    const float scale = juce::jmin ((float) getWidth() / (float) baseWidth, (float) getHeight() / (float) baseHeight);   // fits either way a host sizes the window
    proc.uiScale = scale;
    view.setBounds (0, 0, baseWidth, baseHeight);
    view.setTransform (juce::AffineTransform::scale (scale));

    auto r = view.getLocalBounds();
    auto header = r.removeFromTop (headerHeight);
    logoArea.setBounds (16, 6, 196, 66);
    header.removeFromLeft (230);

    // right end: MIDI light and level meter
    auto right = header.removeFromRight (70).reduced (0, 10);
    meter.setBounds (right.removeFromRight (14));
    right.removeFromRight (10);
    auto ledArea = right.removeFromRight (40);
    midiLed.setBounds (ledArea.removeFromTop (34).withSizeKeepingCentre (22, 22));
    midiLabel.setBounds (ledArea.removeFromTop (14));

    // preset bar: two rows; ADVANCED sits at the end of the second
    auto bar = header.reduced (6, 10);
    auto top = bar.removeFromTop (28);
    catButtons[0].setBounds (top.removeFromLeft (120));
    top.removeFromLeft (4);
    catButtons[1].setBounds (top.removeFromLeft (50));
    top.removeFromLeft (8);
    prevBtn.setBounds (top.removeFromLeft (26));
    top.removeFromLeft (3);
    nextBtn.setBounds (top.removeFromLeft (26));
    top.removeFromLeft (6);
    presetButton.setBounds (top);
    bar.removeFromTop (6);
    auto bottom = bar.removeFromTop (26);
    reloadBtn.setBounds (bottom.removeFromLeft (64));
    bottom.removeFromLeft (4);
    saveBtn.setBounds (bottom.removeFromLeft (56));
    bottom.removeFromLeft (4);
    renameBtn.setBounds (bottom.removeFromLeft (68));
    bottom.removeFromLeft (4);
    deleteBtn.setBounds (bottom.removeFromLeft (64));
    advancedBtn.setBounds (bottom.removeFromRight (116));

    Knob* small[] { &noiseKnob, &moveKnob, &delayKnob, &reverbKnob, &hpKnob };
    if (! expanded)
    {
        // the big macro in the middle, the five knobs under it
        auto main = r;
        auto knobRow = main.removeFromBottom (92).withTrimmedBottom (20);
        auto krow = knobRow.withSizeKeepingCentre (5 * 76, knobRow.getHeight());
        for (auto* k : small) k->setBounds (krow.removeFromLeft (76));
        macro.setBounds (main.withSizeKeepingCentre (270, 262).translated (0, 4));
        return;
    }

    // advanced: the macro (normal size) and the five knobs in a column on the left, evenly spaced
    auto column = r.removeFromLeft (leftColumnWidth).reduced (4, 6);
    Knob* all[] { &macroSmall, &noiseKnob, &moveKnob, &delayKnob, &reverbKnob, &hpKnob };
    const int cell = column.getHeight() / 6;
    for (auto* k : all) k->setBounds (column.removeFromTop (cell).withSizeKeepingCentre (column.getWidth(), juce::jmin (cell, 58)));

    auto pagesR = pagesArea().reduced (8, 6);
    auto tabRow = pagesR.removeFromTop (24);
    const int tw = (tabRow.getWidth() - (numPages - 1) * 4) / numPages;
    for (auto& t : tabs)
    {
        t.setBounds (tabRow.removeFromLeft (tw));
        tabRow.removeFromLeft (4);
    }
    pagesR.removeFromTop (6);
    for (auto& p : pages) p->setBounds (pagesR);
}

void ElukkaNoiseEditor::showPage (int index)
{
    currentPage = index;
    for (int i = 0; i < numPages; ++i)
        pages[i]->setVisible (expanded && i == index);
    pages[index]->refresh();
}

void ElukkaNoiseEditor::updatePresetButton()
{
    auto& pm = proc.presets;
    const bool known = pm.currentIndex >= 0 && pm.currentIndex < (int) pm.entries.size();
    const bool isUser = known && ! pm.entries[(size_t) pm.currentIndex].isFactory;
    juce::String text = pm.currentName;
    if (known) text = (pm.entries[(size_t) pm.currentIndex].isFactory ? "" : "User: ") + text;
    presetButton.setButtonText (text + juce::String::fromUTF8 ("  \xe2\x96\xbe"));
    for (int i = 0; i < 2; ++i)
        catButtons[i].setToggleState (pm.currentCategory == PresetManager::categories()[i], juce::dontSendNotification);
    deleteBtn.setEnabled (isUser);
    renameBtn.setEnabled (isUser);
    reloadBtn.setEnabled (known);
    deleteBtn.setTooltip (isUser ? "Delete this preset (the file goes to the Trash)" : "Only your own saved presets can be deleted");
    renameBtn.setTooltip (isUser ? "Rename this preset" : "Only your own saved presets can be renamed");
}

void ElukkaNoiseEditor::showPresetMenu (int categoryOnly)
{
    auto& pm = proc.presets;
    juce::PopupMenu menu;
    const auto& cats = PresetManager::categories();
    auto addCategory = [&] (int c, juce::PopupMenu& target)
    {
        bool any = false;
        for (int i = 0; i < (int) pm.entries.size(); ++i)
        {
            auto& e = pm.entries[(size_t) i];
            if (e.category != cats[c]) continue;
            if (e.isFactory) { target.addItem (i + 1, e.name, true, i == pm.currentIndex); any = true; }
        }
        bool userHeader = false;
        for (int i = 0; i < (int) pm.entries.size(); ++i)
        {
            auto& e = pm.entries[(size_t) i];
            if (e.category != cats[c] || e.isFactory) continue;
            if (! userHeader) { target.addSeparator(); target.addSectionHeader ("User"); userHeader = true; }
            target.addItem (i + 1, e.name, true, i == pm.currentIndex);
            any = true;
        }
        return any;
    };
    juce::Component* target = &presetButton;
    // Init (plain white noise) sits at the top of every preset menu
    if (const int init = pm.indexOf ("Init", "Init", true); init >= 0)
    {
        menu.addItem (init + 1, "Init  (white noise)", true, init == pm.currentIndex);
        menu.addSeparator();
    }
    if (categoryOnly >= 0)
    {
        menu.addSectionHeader (cats[categoryOnly]);
        addCategory (categoryOnly, menu);
        target = &catButtons[categoryOnly];
    }
    else
    {
        for (int c = 0; c < cats.size(); ++c)
        {
            juce::PopupMenu sub;
            addCategory (c, sub);
            menu.addSubMenu (cats[c], sub, true, nullptr, pm.currentCategory == cats[c]);
        }
    }
    juce::Component::SafePointer<ElukkaNoiseEditor> safe (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (target).withMinimumWidth (juce::jmax (200, target->getWidth())),
                        [safe] (int result)
                        {
                            if (safe == nullptr || result <= 0) return;
                            safe->proc.presets.load (result - 1);
                            safe->updatePresetButton();
                        });
}

void ElukkaNoiseEditor::stepPreset (int delta)
{
    auto& pm = proc.presets;
    const int n = (int) pm.entries.size();
    if (n == 0) return;
    // step within the current category (from Init: through everything)
    int pos = pm.currentIndex;
    const bool anyCategory = pm.currentCategory == "Init";
    for (int tries = 0; tries < n; ++tries)
    {
        pos = pos < 0 ? (delta > 0 ? 0 : n - 1) : (pos + delta + n) % n;
        if (anyCategory || pm.entries[(size_t) pos].category == pm.currentCategory) break;
    }
    pm.load (pos);
    updatePresetButton();
}

void ElukkaNoiseEditor::reloadPreset()
{
    auto& pm = proc.presets;
    if (pm.currentIndex >= 0) { pm.load (pm.currentIndex); updatePresetButton(); }
}

void ElukkaNoiseEditor::savePresetDialog()
{
    // the instrument preset stores the names of the delay and reverb presets, not their settings: edits to an effect
    // only survive as an effect preset of their own
    juce::String note = "Saved in " + PresetManager::userPresetDir().getFullPathName()
                      + "\nLoads with the delay preset \"" + proc.dlyPresets.currentName + "\" and the reverb preset \"" + proc.revPresets.currentName + "\".";
    for (auto* fxp : { &proc.dlyPresets, &proc.revPresets })
        if (fxp->isModified())
            note += juce::String ("\nThe ") + juce::String (fxp->title()).toLowerCase() + " has been changed since \"" + fxp->currentName + "\" was loaded: save it as a "
                  + juce::String (fxp->title()).toLowerCase() + " preset first to keep those changes.";
    auto* w = new juce::AlertWindow ("Save Preset", note, juce::MessageBoxIconType::NoIcon);
    w->addTextEditor ("name", proc.presets.currentName, "Name:");
    w->addComboBox ("category", PresetManager::categories(), "Category:");
    if (auto* box = w->getComboBoxComponent ("category"))
        box->setSelectedItemIndex (juce::jmax (0, PresetManager::categories().indexOf (proc.presets.currentCategory)));
    w->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    juce::Component::SafePointer<ElukkaNoiseEditor> safe (this);
    track (*w);
    w->enterModalState (true, juce::ModalCallbackFunction::create ([safe, w] (int result)
    {
        if (safe == nullptr || result != 1) return;
        const auto name = PresetManager::legalName (w->getTextEditorContents ("name"));
        auto* box = w->getComboBoxComponent ("category");
        auto cat = box != nullptr ? box->getText() : PresetManager::categories()[0];
        juce::String problem;
        if (name.isEmpty()) problem = "Enter a name.";
        else if (safe->proc.presets.indexOf (name, cat, true) >= 0) problem = "\"" + name + "\" is a factory preset: choose another name.";
        else if (! safe->proc.presets.save (name, cat)) problem = "Could not write the preset file.";
        if (problem.isNotEmpty())
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Save Preset", problem);
        safe->updatePresetButton();
    }), true);
}

void ElukkaNoiseEditor::renamePresetDialog()
{
    auto& pm = proc.presets;
    const int index = pm.currentIndex;
    if (index < 0 || index >= (int) pm.entries.size() || pm.entries[(size_t) index].isFactory) return;
    const auto file = pm.entries[(size_t) index].file;
    auto* w = new juce::AlertWindow ("Rename Preset", "", juce::MessageBoxIconType::NoIcon);
    w->addTextEditor ("name", pm.entries[(size_t) index].name, "New name:");
    w->addButton ("Rename", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    juce::Component::SafePointer<ElukkaNoiseEditor> safe (this);
    track (*w);
    w->enterModalState (true, juce::ModalCallbackFunction::create ([safe, w, index, file] (int result)
    {
        if (safe == nullptr || result != 1) return;
        auto& presets = safe->proc.presets;
        if (index >= (int) presets.entries.size() || presets.entries[(size_t) index].file != file
            || ! presets.rename (index, w->getTextEditorContents ("name")))
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Rename Preset", "Could not rename the preset (is the name in use, or a factory preset's?).");
        safe->updatePresetButton();
    }), true);
}

void ElukkaNoiseEditor::deletePresetDialog()
{
    auto& pm = proc.presets;
    const int index = pm.currentIndex;
    if (index < 0 || index >= (int) pm.entries.size() || pm.entries[(size_t) index].isFactory) return;
    const auto file = pm.entries[(size_t) index].file;
    auto* w = new juce::AlertWindow ("Delete Preset", "Delete \"" + pm.entries[(size_t) index].name + "\"?\nThe file is moved to the Trash.",
                                     juce::MessageBoxIconType::WarningIcon);
    w->addButton ("Delete", 1);   // Return and Escape both cancel: only a click deletes
    w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey), juce::KeyPress (juce::KeyPress::returnKey));
    juce::Component::SafePointer<ElukkaNoiseEditor> safe (this);
    track (*w);
    w->enterModalState (true, juce::ModalCallbackFunction::create ([safe, index, file] (int result)
    {
        if (safe == nullptr || result != 1) return;
        auto& presets = safe->proc.presets;
        if (index >= (int) presets.entries.size() || presets.entries[(size_t) index].file != file || ! presets.deletePreset (index))
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Delete Preset", "Could not delete the preset.");
        safe->updatePresetButton();
    }), true);
}

void ElukkaNoiseEditor::showCredits()
{
    auto text = std::make_unique<juce::Label>();
    text->setText ("ElukkaNoise\nVibecoded by Elukka\nAll the work done by Claude", juce::dontSendNotification);
    text->setJustificationType (juce::Justification::centred);
    text->setColour (juce::Label::textColourId, Col::text);
    text->setFont (juce::Font (juce::FontOptions (13.0f)));
    text->setSize (220, 66);
    launch (std::move (text), getLocalArea (&logoArea, logoArea.getLocalBounds()), this);
}

void ElukkaNoiseEditor::timerCallback()
{
   #if JUCE_MAC
    if (! hostWindowKept) hostWindowKept = keepHostWindowVisible (*this);
   #endif
    const int v = proc.uiVersion.load();
    if (v != lastUiVersion)
    {
        lastUiVersion = v;
        for (auto& p : pages) p->refresh();
        updatePresetButton();
    }
    auto& e = proc.engine;
    midiLed.set (e.uiNoteOn.load());
    meter.set (e.uiLevel.load());
    macro.setDisplay (e.uiCutoff.load(), e.uiPan.load(), e.uiWidth.load(), e.uiNoteOn.load());
    for (auto* k : { &noiseKnob, &moveKnob, &delayKnob, &reverbKnob, &hpKnob }) k->tickMod();
    if (expanded) pages[currentPage]->tick();
}
