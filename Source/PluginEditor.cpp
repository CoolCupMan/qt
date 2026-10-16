#include "PluginEditor.h"
#include "Engine/AudioLoader.h"

namespace
{
    const auto accent = juce::Colour (0xff4fc3a1);

    juce::String clock (double s)
    {
        const auto t = (int) s;
        return juce::String::formatted ("%d:%02d", t / 60, t % 60);
    }
}

//==============================================================================
ParamPage::ParamPage (RemixProcessor& p, const juce::StringArray& ids, std::vector<TextRow> textRows)
{
    for (auto& id : ids)
    {
        auto* param = p.params.getParameter (id);
        jassert (param != nullptr);
        if (param == nullptr) continue;

        Row row;
        row.label = std::make_unique<juce::Label> (id, param->getName (64));
        row.label->setJustificationType (juce::Justification::centredRight);

        if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (param))
        {
            auto box = std::make_unique<juce::ComboBox>();
            box->addItemList (choice->choices, 1);
            row.comboAtt = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (p.params, id, *box);
            row.control = std::move (box);
        }
        else if (dynamic_cast<juce::AudioParameterBool*> (param) != nullptr)
        {
            auto btn = std::make_unique<juce::ToggleButton> ("");
            row.buttonAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (p.params, id, *btn);
            row.control = std::move (btn);
        }
        else
        {
            auto slider = std::make_unique<juce::Slider> (juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight);
            slider->setTextBoxStyle (juce::Slider::TextBoxRight, false, 90, 22);
            slider->setColour (juce::Slider::thumbColourId, accent);
            if (param->getLabel().isNotEmpty())
                slider->setTextValueSuffix (" " + param->getLabel());
            row.sliderAtt = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (p.params, id, *slider);
            row.control = std::move (slider);
        }

        addAndMakeVisible (*row.label);
        addAndMakeVisible (*row.control);
        rows.push_back (std::move (row));
    }

    for (auto& t : textRows)
    {
        Row row;
        row.label = std::make_unique<juce::Label> (t.property, t.label);
        row.label->setJustificationType (juce::Justification::centredRight);

        auto ed = std::make_unique<juce::TextEditor>();
        ed->setTextToShowWhenEmpty (t.hint, juce::Colours::grey);
        auto tree = p.extra();
        const juce::Identifier prop (t.property);
        ed->setText (tree.getProperty (prop).toString(), false);
        auto* raw = ed.get();
        ed->onTextChange = [tree, prop, raw]() mutable { tree.setProperty (prop, raw->getText(), nullptr); };

        addAndMakeVisible (*row.label);
        addAndMakeVisible (*ed);
        row.control = std::move (ed);
        rows.push_back (std::move (row));
    }
}

int ParamPage::getIdealHeight() const { return 8 + (int) rows.size() * 30; }

void ParamPage::resized()
{
    auto area = getLocalBounds().reduced (8, 4);
    const int labelW = juce::jmin (230, area.getWidth() / 3);
    for (auto& r : rows)
    {
        auto line = area.removeFromTop (30).reduced (0, 3);
        r.label->setBounds (line.removeFromLeft (labelW));
        line.removeFromLeft (8);
        if (dynamic_cast<juce::ToggleButton*> (r.control.get()) != nullptr)
            line.setWidth (32);
        r.control->setBounds (line);
    }
}

//==============================================================================
void WaveOverview::paint (juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat();
    g.setColour (juce::Colour (0xff15181c));
    g.fillRoundedRectangle (b, 4.0f);

    auto r = proc.getRendered();
    if (r == nullptr || r->overview.empty())
    {
        g.setColour (juce::Colours::grey);
        g.drawText ("Render to see the remix here. Click to seek.", getLocalBounds(), juce::Justification::centred);
        return;
    }

    const float midY = b.getCentreY();
    const float h = b.getHeight() * 0.45f;
    const auto& o = r->overview;
    juce::Path p;
    for (int x = 0; x < getWidth(); ++x)
    {
        const auto v = o[(size_t) juce::jmin ((int) o.size() - 1, (int) ((double) x / getWidth() * (double) o.size()))];
        p.addRectangle ((float) x, midY - v * h, 1.0f, juce::jmax (1.0f, 2.0f * v * h));
    }
    g.setColour (accent.withAlpha (0.8f));
    g.fillPath (p);

    const double total = (double) r->audio.size() / r->sampleRate;

    // 63/27 grid hints
    g.setColour (juce::Colours::white.withAlpha (0.08f));
    for (double t = 0; t < total; t += 90.0)
        g.fillRect ((float) (t / total * getWidth()), 0.0f, 1.0f, b.getHeight());

    const double pos = proc.getDisplayPositionSeconds();
    const auto px = (float) (pos / total * getWidth());
    g.setColour (juce::Colours::white);
    g.fillRect (px, 0.0f, 2.0f, b.getHeight());

    g.setColour (juce::Colours::lightgrey);
    g.setFont (12.0f);
    g.drawText (clock (pos) + " / " + clock (total) + "    seed " + juce::String (r->seed),
                getLocalBounds().reduced (6, 2), juce::Justification::topRight);
}

void WaveOverview::mouseDown (const juce::MouseEvent& e)
{
    if (auto r = proc.getRendered())
        proc.seekSeconds ((double) e.x / juce::jmax (1, getWidth()) * (double) r->audio.size() / r->sampleRate);
}

//==============================================================================
RemixEditor::RemixEditor (RemixProcessor& p)
    : AudioProcessorEditor (p), proc (p), overview (p)
{
    title.setText ("WAV Remixer", juce::dontSendNotification);
    title.setFont (juce::FontOptions (22.0f, juce::Font::bold));
    title.setColour (juce::Label::textColourId, accent);
    addAndMakeVisible (title);

    for (auto* b : { &renderBtn, &cancelBtn, &playBtn, &exportBtn, &addFilesBtn, &addFolderBtn, &removeBtn, &clearBtn,
                     &browseOutBtn, &lastSeedBtn, &randomSeedBtn })
        addAndMakeVisible (*b);
    renderBtn.setColour (juce::TextButton::buttonColourId, accent.darker (0.4f));

    renderBtn.onClick = [this] { proc.startRender (proc.params.getRawParameterValue ("autoExport")->load() >= 0.5f); };
    cancelBtn.onClick = [this] { proc.cancelRender(); };
    playBtn.onClick   = [this] { proc.setPlaying (! proc.isPlaying()); };
    exportBtn.onClick = [this] { exportAs(); };
    addFilesBtn.onClick  = [this] { chooseFiles(); };
    addFolderBtn.onClick = [this] { chooseFolder(); };
    removeBtn.onClick    = [this] { removeSelected(); };
    clearBtn.onClick     = [this] { proc.setInputs ({}); refreshInputs(); };
    browseOutBtn.onClick = [this] { chooseOutputFolder(); };
    lastSeedBtn.onClick  = [this]
    {
        if (auto r = proc.getRendered())
            seedEdit.setText (juce::String (r->seed), true);
    };
    randomSeedBtn.onClick = [this] { seedEdit.setText ("0", true); };

    addAndMakeVisible (progressBar);

    inputsLabel.setText ("Inputs  (drop WAV files or folders anywhere)", juce::dontSendNotification);
    addAndMakeVisible (inputsLabel);
    inputList.setModel (this);
    inputList.setMultipleSelectionEnabled (true);
    inputList.setRowHeight (20);
    inputList.setColour (juce::ListBox::backgroundColourId, juce::Colour (0xff15181c));
    addAndMakeVisible (inputList);

    outDirLabel.setText ("Output folder", juce::dontSendNotification);
    outDirEdit.setTextToShowWhenEmpty ("same folder as the first input", juce::Colours::grey);
    outNameLabel.setText ("File name", juce::dontSendNotification);
    seedLabel.setText ("Seed (0 = random)", juce::dontSendNotification);
    for (auto* l : { &outDirLabel, &outNameLabel, &seedLabel }) addAndMakeVisible (*l);

    bindText (outDirEdit, "outputDir");
    bindText (outNameEdit, "outputName");
    bindText (seedEdit, "seed");
    seedEdit.setInputRestrictions (10, "0123456789");

    const auto bg = juce::Colour (0xff1e2227);
    auto page = [&] (const juce::String& name, ParamPage* pg)
    {
        auto* vp = new juce::Viewport();
        vp->setViewedComponent (pg, true);
        vp->setScrollBarsShown (true, false);
        pg->setSize (600, pg->getIdealHeight());
        tabs.addTab (name, bg, vp, true);
    };

    page ("Structure", new ParamPage (proc, { "lengthSec", "engineMode", "longBlockSec", "subLoops", "shortBlockSec",
                                              "montageStructure", "segMinMs", "segMaxMs", "crossfadeMs" }));
    page ("Variation", new ParamPage (proc, { "reverseProb", "stutterProb", "stutterCooldown", "layerProb", "layerGain", "periodicFx" }));
    page ("Sorting", new ParamPage (proc, { "sortScope", "primaryMode", "orderMode", "mixA", "mixB", "mixC", "mixD" },
                                    { { "Instrument sequence", "instrSeq", "e.g. synth pad, electric piano, drum kit, 808" },
                                      { "Instrument emphasis %", "instrPct", "e.g. synth pad:50, drum kit:30" } }));
    page ("Cleanup", new ParamPage (proc, { "trimDb", "internalSilMs", "gateDb", "gateReduction", "centerCut", "excludePerc" }));
    page ("Output", new ParamPage (proc, { "targetPeak", "softClip", "declick", "dcBlock", "renderRate", "outFormat",
                                           "autoExport", "writeParts" },
                                   { { "Part size (MB)", "partMB", "1800" } }));
    page ("Playback", new ParamPage (proc, { "level", "syncHost", "loopPlay", "passInput" }));
    addAndMakeVisible (tabs);

    addAndMakeVisible (overview);

    logView.setMultiLine (true);
    logView.setReadOnly (true);
    logView.setScrollbarsShown (true);
    logView.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::plain));
    logView.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff15181c));
    addAndMakeVisible (logView);

    refreshInputs();
    setResizable (true, true);
    setResizeLimits (900, 620, 2400, 1600);
    setSize (1120, 760);
    startTimerHz (25);
}

RemixEditor::~RemixEditor()
{
    inputList.setModel (nullptr);
}

void RemixEditor::bindText (juce::TextEditor& ed, const juce::Identifier& prop)
{
    auto tree = proc.extra();
    ed.setText (tree.getProperty (prop).toString(), false);
    ed.onTextChange = [tree, prop, &ed]() mutable { tree.setProperty (prop, ed.getText(), nullptr); };
    addAndMakeVisible (ed);
}

//==============================================================================
void RemixEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff1e2227));
    if (dragOver)
    {
        g.setColour (accent);
        g.drawRect (getLocalBounds(), 3);
    }
}

void RemixEditor::resized()
{
    auto area = getLocalBounds().reduced (10);

    auto top = area.removeFromTop (34);
    title.setBounds (top.removeFromLeft (180));
    for (auto* b : { &renderBtn, &cancelBtn, &playBtn, &exportBtn })
    {
        b->setBounds (top.removeFromLeft (96).reduced (3, 2));
    }
    top.removeFromLeft (10);
    progressBar.setBounds (top.reduced (0, 4));
    area.removeFromTop (8);

    auto left = area.removeFromLeft (juce::jmax (300, area.getWidth() / 3));
    area.removeFromLeft (10);

    inputsLabel.setBounds (left.removeFromTop (22));
    auto out = left.removeFromBottom (3 * 30 + 6);
    auto btns = left.removeFromBottom (32);
    inputList.setBounds (left.reduced (0, 2));
    const int bw = btns.getWidth() / 4;
    for (auto* b : { &addFilesBtn, &addFolderBtn, &removeBtn, &clearBtn })
        b->setBounds (btns.removeFromLeft (bw).reduced (2, 3));

    out.removeFromTop (6);
    auto line = out.removeFromTop (30);
    outDirLabel.setBounds (line.removeFromLeft (110));
    browseOutBtn.setBounds (line.removeFromRight (32).reduced (2, 3));
    outDirEdit.setBounds (line.reduced (0, 3));
    line = out.removeFromTop (30);
    outNameLabel.setBounds (line.removeFromLeft (110));
    outNameEdit.setBounds (line.reduced (0, 3));
    line = out.removeFromTop (30);
    seedLabel.setBounds (line.removeFromLeft (110));
    randomSeedBtn.setBounds (line.removeFromRight (64).reduced (2, 3));
    lastSeedBtn.setBounds (line.removeFromRight (80).reduced (2, 3));
    seedEdit.setBounds (line.reduced (0, 3));

    auto tabsArea = area.removeFromTop (juce::jmax (300, area.getHeight() / 2));
    tabs.setBounds (tabsArea);
    for (int i = 0; i < tabs.getNumTabs(); ++i)
        if (auto* vp = dynamic_cast<juce::Viewport*> (tabs.getTabContentComponent (i)))
            if (auto* pg = dynamic_cast<ParamPage*> (vp->getViewedComponent()))
                pg->setSize (juce::jmax (300, tabsArea.getWidth() - 12), pg->getIdealHeight());

    area.removeFromTop (8);
    overview.setBounds (area.removeFromTop (90));
    area.removeFromTop (8);
    logView.setBounds (area);
}

//==============================================================================
void RemixEditor::timerCallback()
{
    const bool rendering = proc.isRendering();
    progressValue = rendering ? proc.getProgress() : (proc.getRendered() != nullptr ? 1.0 : 0.0);
    renderBtn.setEnabled (! rendering);
    cancelBtn.setEnabled (rendering);
    const bool haveAudio = proc.getRendered() != nullptr;
    playBtn.setEnabled (haveAudio);
    exportBtn.setEnabled (haveAudio && ! rendering);
    playBtn.setButtonText (proc.isPlaying() ? "Stop" : "Play");
    wasRendering = rendering;

    int next = logIndex;
    auto lines = proc.getLogLines (logIndex, next);
    if (! lines.isEmpty())
    {
        logView.moveCaretToEnd();
        logView.insertTextAtCaret (lines.joinIntoString ("\n") + "\n");
    }
    logIndex = next;

    overview.repaint();
}

int RemixEditor::getNumRows() { return inputs.size(); }

void RemixEditor::paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected)
{
    if (! juce::isPositiveAndBelow (row, inputs.size())) return;
    if (selected) g.fillAll (accent.withAlpha (0.35f));
    const juce::File f (inputs[row]);
    g.setColour (f.exists() ? juce::Colours::white : juce::Colours::indianred);
    g.setFont (13.0f);
    const auto text = (f.isDirectory() ? "[folder] " : "") + f.getFileName() + "   " + f.getParentDirectory().getFullPathName();
    g.drawText (text, 6, 0, w - 8, h, juce::Justification::centredLeft, true);
}

void RemixEditor::refreshInputs()
{
    inputs = proc.getInputs();
    inputList.updateContent();
    inputList.repaint();
}

void RemixEditor::addPaths (const juce::StringArray& paths)
{
    auto current = proc.getInputs();
    for (auto& p : paths)
    {
        const juce::File f (p);
        if (f.isDirectory() || remix::AudioLoader::isSupportedExtension (f))
            current.addIfNotAlreadyThere (f.getFullPathName());
    }
    proc.setInputs (current);

    // Default the output folder to the first input's folder, like the scripts did.
    if (outDirEdit.getText().isEmpty() && ! current.isEmpty())
    {
        const juce::File first (current[0]);
        outDirEdit.setText ((first.isDirectory() ? first : first.getParentDirectory()).getFullPathName(), true);
    }
    refreshInputs();
}

void RemixEditor::removeSelected()
{
    auto current = proc.getInputs();
    auto sel = inputList.getSelectedRows();
    for (int i = sel.size(); --i >= 0;)
        current.remove (sel[i]);
    proc.setInputs (current);
    inputList.deselectAllRows();
    refreshInputs();
}

void RemixEditor::filesDropped (const juce::StringArray& files, int, int)
{
    dragOver = false;
    addPaths (files);
    repaint();
}

void RemixEditor::chooseFiles()
{
    chooser = std::make_unique<juce::FileChooser> ("Add audio files", juce::File(), "*.wav;*.wave;*.bwf;*.aif;*.aiff;*.flac;*.ogg;*.mp3");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::canSelectMultipleItems,
                          [this] (const juce::FileChooser& fc)
                          {
                              juce::StringArray paths;
                              for (auto& f : fc.getResults()) paths.add (f.getFullPathName());
                              addPaths (paths);
                          });
}

void RemixEditor::chooseFolder()
{
    chooser = std::make_unique<juce::FileChooser> ("Add a folder (scanned recursively)");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                          [this] (const juce::FileChooser& fc)
                          {
                              if (fc.getResult() != juce::File())
                                  addPaths ({ fc.getResult().getFullPathName() });
                          });
}

void RemixEditor::chooseOutputFolder()
{
    chooser = std::make_unique<juce::FileChooser> ("Output folder", juce::File (outDirEdit.getText()));
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                          [this] (const juce::FileChooser& fc)
                          {
                              if (fc.getResult() != juce::File())
                                  outDirEdit.setText (fc.getResult().getFullPathName(), true);
                          });
}

void RemixEditor::exportAs()
{
    chooser = std::make_unique<juce::FileChooser> ("Export the current remix to folder", juce::File (outDirEdit.getText()));
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                          [this] (const juce::FileChooser& fc)
                          {
                              if (fc.getResult() == juce::File()) return;
                              juce::String err;
                              if (! proc.exportTo (fc.getResult(), err))
                                  proc.appendLog ("Export failed: " + err);
                          });
}
