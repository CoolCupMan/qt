#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "Engine/RemixEngine.h"

/** A finished render, shared between the render thread, the audio thread
    and the editor. Immutable once published. */
struct RenderedRemix
{
    remix::Clip  audio;
    double       sampleRate = 44100.0;
    juce::uint32 seed = 0;
    std::vector<float> overview;   // per-bucket peak for the waveform display
};

class RemixProcessor final : public juce::AudioProcessor
{
public:
    RemixProcessor();
    ~RemixProcessor() override;

    //==========================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override                        { return true; }

    const juce::String getName() const override            { return JucePlugin_Name; }
    bool acceptsMidi() const override                      { return false; }
    bool producesMidi() const override                     { return false; }
    bool isMidiEffect() const override                     { return false; }
    double getTailLengthSeconds() const override           { return 0.0; }

    int getNumPrograms() override                          { return 1; }
    int getCurrentProgram() override                       { return 0; }
    void setCurrentProgram (int) override                  {}
    const juce::String getProgramName (int) override       { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    //==========================================================================
    juce::AudioProcessorValueTreeState params;

    /** Non-automatable settings (paths, text fields). Lives inside params.state. */
    juce::ValueTree extra();

    juce::StringArray getInputs();
    void setInputs (const juce::StringArray&);

    void startRender (bool exportWhenDone);
    void cancelRender();
    bool isRendering() const;
    double getProgress() const                             { return progress.load(); }

    bool exportTo (const juce::File& folder, juce::String& error);

    std::shared_ptr<const RenderedRemix> getRendered() const;

    // Free-running transport (standalone, or plugin with host sync off)
    void setPlaying (bool shouldPlay);
    bool isPlaying() const                                 { return playing.load(); }
    void seekSeconds (double seconds);
    double getDisplayPositionSeconds() const               { return displayPos.load(); }

    /** Log lines appended by the render thread; `fromIndex` lets the editor fetch only new lines. */
    juce::StringArray getLogLines (int fromIndex, int& nextIndex) const;
    void appendLog (const juce::String& line);
    void clearLog();

    remix::Settings buildSettings();

private:
    class RenderJob;
    friend class RenderJob;

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void publish (std::shared_ptr<const RenderedRemix>);
    void purgeRetired();

    std::unique_ptr<RenderJob> job;
    std::atomic<double> progress { 0.0 };

    mutable juce::SpinLock renderedLock;
    std::shared_ptr<const RenderedRemix> rendered;
    std::vector<std::shared_ptr<const RenderedRemix>> retired;
    juce::CriticalSection retiredLock;

    std::atomic<bool>   playing { false };
    std::atomic<double> freePos { 0.0 };     // in render samples
    std::atomic<double> displayPos { 0.0 };  // seconds
    std::atomic<double> seekRequest { -1.0 };
    double hostRate = 44100.0;
    juce::SmoothedValue<float> levelSmoothed;

    mutable juce::CriticalSection logLock;
    juce::StringArray logLines;
    int logBase = 0;

    JUCE_DECLARE_WEAK_REFERENCEABLE (RemixProcessor)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RemixProcessor)
};
