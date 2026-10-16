#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include "PluginProcessor.h"

/** One tab of automatically laid-out parameter rows (+ optional text fields). */
class ParamPage final : public juce::Component
{
public:
    struct TextRow { juce::String label, property, hint; };

    ParamPage (RemixProcessor&, const juce::StringArray& paramIds, std::vector<TextRow> textRows = {});
    void resized() override;
    int getIdealHeight() const;

private:
    struct Row
    {
        std::unique_ptr<juce::Label> label;
        std::unique_ptr<juce::Component> control;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>   sliderAtt;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> comboAtt;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>   buttonAtt;
    };
    std::vector<Row> rows;
};

class WaveOverview final : public juce::Component
{
public:
    explicit WaveOverview (RemixProcessor& p) : proc (p) {}
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent& e) override { mouseDown (e); }

private:
    RemixProcessor& proc;
};

class RemixEditor final : public juce::AudioProcessorEditor,
                          public juce::FileDragAndDropTarget,
                          private juce::Timer,
                          private juce::ListBoxModel
{
public:
    explicit RemixEditor (RemixProcessor&);
    ~RemixEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    bool isInterestedInFileDrag (const juce::StringArray&) override { return true; }
    void filesDropped (const juce::StringArray& files, int, int) override;
    void fileDragEnter (const juce::StringArray&, int, int) override { dragOver = true; repaint(); }
    void fileDragExit (const juce::StringArray&) override            { dragOver = false; repaint(); }

private:
    void timerCallback() override;
    int  getNumRows() override;
    void paintListBoxItem (int row, juce::Graphics&, int w, int h, bool selected) override;
    void deleteKeyPressed (int) override { removeSelected(); }

    void addPaths (const juce::StringArray&);
    void removeSelected();
    void chooseFiles();
    void chooseFolder();
    void chooseOutputFolder();
    void exportAs();
    void refreshInputs();
    void bindText (juce::TextEditor&, const juce::Identifier&);

    RemixProcessor& proc;

    juce::Label       title;
    juce::TextButton  renderBtn { "Render" }, cancelBtn { "Cancel" }, playBtn { "Play" }, exportBtn { "Export..." };
    double            progressValue = 0.0;
    juce::ProgressBar progressBar { progressValue };

    juce::Label       inputsLabel;
    juce::ListBox     inputList;
    juce::TextButton  addFilesBtn { "Add files..." }, addFolderBtn { "Add folder..." },
                      removeBtn { "Remove" }, clearBtn { "Clear" };
    juce::StringArray inputs;

    juce::Label       outDirLabel, outNameLabel, seedLabel;
    juce::TextEditor  outDirEdit, outNameEdit, seedEdit;
    juce::TextButton  browseOutBtn { "..." }, lastSeedBtn { "Reuse last" }, randomSeedBtn { "Random" };

    juce::TabbedComponent tabs { juce::TabbedButtonBar::TabsAtTop };
    WaveOverview      overview;
    juce::TextEditor  logView;
    int               logIndex = 0;
    bool              dragOver = false;
    bool              wasRendering = false;

    std::unique_ptr<juce::FileChooser> chooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RemixEditor)
};
