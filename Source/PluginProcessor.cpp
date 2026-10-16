#include "PluginProcessor.h"
#include "PluginEditor.h"

using namespace remix;

namespace
{
    const juce::StringArray rateChoices   { "44100", "48000", "88200", "96000" };
    const juce::StringArray formatChoices { "16-bit PCM", "24-bit PCM", "32-bit float" };

    juce::String secondsToClock (double s)
    {
        const auto t = (int) std::lround (s);
        return t >= 3600 ? juce::String::formatted ("%d:%02d:%02d", t / 3600, (t / 60) % 60, t % 60)
                         : juce::String::formatted ("%d:%02d", t / 60, t % 60);
    }

    std::vector<float> makeOverview (const Clip& c, int buckets)
    {
        std::vector<float> o ((size_t) buckets, 0.0f);
        if (c.empty()) return o;
        const double per = (double) c.size() / buckets;
        for (int b = 0; b < buckets; ++b)
        {
            const auto a = (size_t) (b * per), e = std::min (c.size(), (size_t) ((b + 1) * per) + 1);
            float p = 0.0f;
            for (size_t i = a; i < e; ++i) p = std::max ({ p, std::abs (c.L[i]), std::abs (c.R[i]) });
            o[(size_t) b] = p;
        }
        return o;
    }

    /** 4-point Hermite read at a fractional position. */
    inline float hermite (const std::vector<float>& x, double pos) noexcept
    {
        const auto n = (juce::int64) x.size();
        const auto i = (juce::int64) pos;
        const float f = (float) (pos - (double) i);
        auto at = [&] (juce::int64 k) { return (k >= 0 && k < n) ? x[(size_t) k] : 0.0f; };
        const float y0 = at (i - 1), y1 = at (i), y2 = at (i + 1), y3 = at (i + 2);
        const float c1 = 0.5f * (y2 - y0);
        const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
        const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
        return ((c3 * f + c2) * f + c1) * f + y1;
    }
}

//==============================================================================
class RemixProcessor::RenderJob final : public juce::Thread
{
public:
    RenderJob (RemixProcessor& p, Settings st, bool exp)
        : juce::Thread ("Remix render"), owner (p), settings (std::move (st)), exportWhenDone (exp) {}

    ~RenderJob() override { stopThread (30000); }

    void run() override
    {
        Callbacks cb;
        cb.log       = [this] (const juce::String& m) { owner.appendLog (m); };
        cb.progress  = [this] (double v) { owner.progress.store (v); };
        cb.cancelled = [this] { return threadShouldExit(); };

        auto result = RemixEngine::render (settings, cb);
        if (! result.ok)
        {
            owner.appendLog ((result.wasCancelled ? "Render cancelled." : "Render failed: " + result.error));
            return;
        }

        auto r = std::make_shared<RenderedRemix>();
        r->sampleRate = result.sampleRate;
        r->seed = result.seedUsed;
        r->overview = makeOverview (result.audio, 1200);

        if (exportWhenDone)
        {
            juce::String err;
            if (RemixEngine::writeOutputs (settings, result, cb, err).isEmpty())
                owner.appendLog ("Export failed: " + err);
        }

        r->audio = std::move (result.audio);
        owner.publish (std::move (r));
        owner.appendLog ("Done. Press Play (or start the host transport) to listen.");
    }

private:
    RemixProcessor& owner;
    Settings settings;
    bool exportWhenDone;
};

//==============================================================================
RemixProcessor::RemixProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      params (*this, nullptr, "WavRemixer", createLayout())
{
    auto x = extra();
    if (! x.hasProperty ("outputDir"))    x.setProperty ("outputDir", "", nullptr);
    if (! x.hasProperty ("outputName"))   x.setProperty ("outputName", "output.wav", nullptr);
    if (! x.hasProperty ("seed"))         x.setProperty ("seed", "0", nullptr);
    if (! x.hasProperty ("instrSeq"))     x.setProperty ("instrSeq", "", nullptr);
    if (! x.hasProperty ("instrPct"))     x.setProperty ("instrPct", "", nullptr);
    if (! x.hasProperty ("partMB"))       x.setProperty ("partMB", 1800, nullptr);
}

RemixProcessor::~RemixProcessor()
{
    job.reset();
}

juce::AudioProcessorValueTreeState::ParameterLayout RemixProcessor::createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout l;

    auto pct  = AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return String (juce::roundToInt (v * 100)) + " %"; })
                                               .withValueFromStringFunction ([] (const String& t) { return t.getFloatValue() / 100.0f; });
    auto msA  = AudioParameterFloatAttributes().withLabel ("ms");
    auto secA = AudioParameterFloatAttributes().withLabel ("s");
    auto dbA  = AudioParameterFloatAttributes().withLabel ("dB");
    auto lenA = AudioParameterFloatAttributes()
                    .withStringFromValueFunction ([] (float v, int) { return secondsToClock (v); })
                    .withValueFromStringFunction ([] (const String& t) { auto s = RemixEngine::parseDuration (t); return s > 0 ? (float) s : 300.0f; });

    auto skewed = [] (float lo, float hi, float centre, float step = 0.0f)
    {
        NormalisableRange<float> r (lo, hi, step);
        r.setSkewForCentre (centre);
        return r;
    };

    // Structure
    l.add (std::make_unique<AudioParameterFloat>   (ParameterID { "lengthSec", 1 }, "Length", skewed (10.0f, 14400.0f, 600.0f, 1.0f), 300.0f, lenA));
    l.add (std::make_unique<AudioParameterChoice>  (ParameterID { "engineMode", 1 }, "Engine", StringArray { "Loop 63/27 (21/21/21 rotation)", "Montage (file order)" }, 0));
    l.add (std::make_unique<AudioParameterFloat>   (ParameterID { "longBlockSec", 1 }, "Long block", NormalisableRange<float> (6.0f, 240.0f, 1.0f), 63.0f, secA));
    l.add (std::make_unique<AudioParameterInt>     (ParameterID { "subLoops", 1 }, "Motifs per long block", 1, 8, 3));
    l.add (std::make_unique<AudioParameterFloat>   (ParameterID { "shortBlockSec", 1 }, "Variant block", NormalisableRange<float> (0.0f, 120.0f, 1.0f), 27.0f, secA));
    l.add (std::make_unique<AudioParameterBool>    (ParameterID { "montageStructure", 1 }, "Montage: overlay 63/27 grid", true));
    l.add (std::make_unique<AudioParameterFloat>   (ParameterID { "segMinMs", 1 }, "Segment min", skewed (100.0f, 4000.0f, 800.0f, 10.0f), 800.0f, msA));
    l.add (std::make_unique<AudioParameterFloat>   (ParameterID { "segMaxMs", 1 }, "Segment max", skewed (400.0f, 15000.0f, 4000.0f, 10.0f), 4000.0f, msA));
    l.add (std::make_unique<AudioParameterFloat>   (ParameterID { "crossfadeMs", 1 }, "Crossfade", skewed (5.0f, 500.0f, 120.0f, 1.0f), 120.0f, msA));

    // Variation
    l.add (std::make_unique<AudioParameterFloat>   (ParameterID { "reverseProb", 1 }, "Reverse chance", NormalisableRange<float> (0.0f, 1.0f, 0.01f), 0.20f, pct));
    l.add (std::make_unique<AudioParameterFloat>   (ParameterID { "stutterProb", 1 }, "Stutter chance", NormalisableRange<float> (0.0f, 0.5f, 0.01f), 0.07f, pct));
    l.add (std::make_unique<AudioParameterFloat>   (ParameterID { "stutterCooldown", 1 }, "Stutter cooldown", NormalisableRange<float> (0.0f, 120.0f, 1.0f), 30.0f, secA));
    l.add (std::make_unique<AudioParameterFloat>   (ParameterID { "layerProb", 1 }, "Drum layer chance", NormalisableRange<float> (0.0f, 1.0f, 0.01f), 0.35f, pct));
    l.add (std::make_unique<AudioParameterFloat>   (ParameterID { "layerGain", 1 }, "Drum layer gain", NormalisableRange<float> (0.0f, 1.0f, 0.01f), 0.55f, pct));
    l.add (std::make_unique<AudioParameterBool>    (ParameterID { "periodicFx", 1 }, "Periodic stutter/glitch FX", false));

    // Sorting
    l.add (std::make_unique<AudioParameterChoice>  (ParameterID { "sortScope", 1 }, "Sort scope", StringArray { "Both (instrument groups + order)", "Instrument groups only", "Order only (ignore instruments)" }, 0));
    l.add (std::make_unique<AudioParameterChoice>  (ParameterID { "primaryMode", 1 }, "Instrument grouping", StringArray { "Custom sequence (below)", "Music-type priority (melodic > drums > perc)" }, 1));
    l.add (std::make_unique<AudioParameterChoice>  (ParameterID { "orderMode", 1 }, "Order", StringArray { "Alphabetical (0..Z)", "Largest first", "Random", "Random by size", "MIX (A/B/C/D)" }, 0));
    l.add (std::make_unique<AudioParameterInt>     (ParameterID { "mixA", 1 }, "MIX A: alphabetical", 0, 100, 100));
    l.add (std::make_unique<AudioParameterInt>     (ParameterID { "mixB", 1 }, "MIX B: largest first", 0, 100, 0));
    l.add (std::make_unique<AudioParameterInt>     (ParameterID { "mixC", 1 }, "MIX C: random by alpha", 0, 100, 0));
    l.add (std::make_unique<AudioParameterInt>     (ParameterID { "mixD", 1 }, "MIX D: random by size", 0, 100, 0));

    // Cleanup
    l.add (std::make_unique<AudioParameterFloat>   (ParameterID { "trimDb", 1 }, "Silence trim threshold", NormalisableRange<float> (-120.0f, -20.0f, 1.0f), -80.0f, dbA));
    l.add (std::make_unique<AudioParameterFloat>   (ParameterID { "internalSilMs", 1 }, "Remove pauses longer than", NormalisableRange<float> (0.0f, 3000.0f, 10.0f), 0.0f, msA));
    l.add (std::make_unique<AudioParameterFloat>   (ParameterID { "gateDb", 1 }, "Noise gate threshold", NormalisableRange<float> (-120.0f, -20.0f, 1.0f), -120.0f, dbA));
    l.add (std::make_unique<AudioParameterFloat>   (ParameterID { "gateReduction", 1 }, "Gate keeps", NormalisableRange<float> (0.0f, 1.0f, 0.01f), 1.0f, pct));
    l.add (std::make_unique<AudioParameterBool>    (ParameterID { "centerCut", 1 }, "Vocal reduction (center cut)", false));
    l.add (std::make_unique<AudioParameterBool>    (ParameterID { "excludePerc", 1 }, "Skip drum one-shots", false));

    // Master / output
    l.add (std::make_unique<AudioParameterFloat>   (ParameterID { "targetPeak", 1 }, "Target peak", NormalisableRange<float> (0.1f, 1.0f, 0.01f), 0.88f, pct));
    l.add (std::make_unique<AudioParameterBool>    (ParameterID { "softClip", 1 }, "Soft clip", false));
    l.add (std::make_unique<AudioParameterBool>    (ParameterID { "declick", 1 }, "De-click repair", true));
    l.add (std::make_unique<AudioParameterBool>    (ParameterID { "dcBlock", 1 }, "DC blocker", true));
    l.add (std::make_unique<AudioParameterChoice>  (ParameterID { "renderRate", 1 }, "Render sample rate", rateChoices, 0));
    l.add (std::make_unique<AudioParameterChoice>  (ParameterID { "outFormat", 1 }, "File format", formatChoices, 0));
    l.add (std::make_unique<AudioParameterBool>    (ParameterID { "autoExport", 1 }, "Write file after render", true));
    l.add (std::make_unique<AudioParameterBool>    (ParameterID { "writeParts", 1 }, "Also write split parts", false));

    // Playback
    l.add (std::make_unique<AudioParameterFloat>   (ParameterID { "level", 1 }, "Playback level", NormalisableRange<float> (-60.0f, 6.0f, 0.1f), 0.0f, dbA));
    l.add (std::make_unique<AudioParameterBool>    (ParameterID { "syncHost", 1 }, "Follow host transport", true));
    l.add (std::make_unique<AudioParameterBool>    (ParameterID { "loopPlay", 1 }, "Loop playback", true));
    l.add (std::make_unique<AudioParameterBool>    (ParameterID { "passInput", 1 }, "Pass input through", true));

    return l;
}

juce::ValueTree RemixProcessor::extra()
{
    return params.state.getOrCreateChildWithName ("Extra", nullptr);
}

juce::StringArray RemixProcessor::getInputs()
{
    return juce::StringArray::fromLines (extra().getProperty ("inputs").toString());
}

void RemixProcessor::setInputs (const juce::StringArray& paths)
{
    auto clean = paths;
    clean.removeEmptyStrings();
    clean.removeDuplicates (false);
    extra().setProperty ("inputs", clean.joinIntoString ("\n"), nullptr);
}

//==============================================================================
remix::Settings RemixProcessor::buildSettings()
{
    auto f = [this] (const char* id) { return params.getRawParameterValue (id)->load(); };
    auto b = [&] (const char* id) { return f (id) >= 0.5f; };
    auto i = [&] (const char* id) { return juce::roundToInt (f (id)); };
    auto x = extra();

    Settings s;
    s.inputs = getInputs();
    const auto outDir = x.getProperty ("outputDir").toString().trim();
    if (juce::File::isAbsolutePath (outDir))
        s.outputDir = juce::File (outDir);
    else if (! s.inputs.isEmpty())   // default: next to the first input, like the scripts
        s.outputDir = juce::File (s.inputs[0]).isDirectory() ? juce::File (s.inputs[0]) : juce::File (s.inputs[0]).getParentDirectory();
    s.outputName = x.getProperty ("outputName").toString().trim();
    if (s.outputName.isEmpty()) s.outputName = "output.wav";
    s.seed = (juce::uint32) x.getProperty ("seed").toString().getLargeIntValue();
    s.partBytes = (juce::int64) std::max (1, (int) x.getProperty ("partMB")) * 1'000'000;

    s.targetSeconds = f ("lengthSec");
    s.engineMode = i ("engineMode") == 0 ? EngineMode::Loop63_27 : EngineMode::Montage;
    s.longBlockSec = f ("longBlockSec");
    s.subLoops = i ("subLoops");
    s.shortBlockSec = f ("shortBlockSec");
    s.montageStructure = b ("montageStructure");
    s.segMinMs = f ("segMinMs");
    s.segMaxMs = std::max (s.segMinMs, f ("segMaxMs"));
    s.crossfadeMs = f ("crossfadeMs");

    s.reverseProb = f ("reverseProb");
    s.stutterProb = f ("stutterProb");
    s.stutterCooldownSec = f ("stutterCooldown");
    s.layerDrumsProb = f ("layerProb");
    s.layerDrumGain = f ("layerGain");
    s.periodicFx = b ("periodicFx");

    s.sortScope = (SortScope) (i ("sortScope") + 1);
    s.primaryMode = i ("primaryMode") == 0 ? PrimaryMode::CustomSequence : PrimaryMode::FamilyPriority;
    s.orderMode = (OrderMode) (i ("orderMode") + 1);
    s.mixWeights = { f ("mixA"), f ("mixB"), f ("mixC"), f ("mixD") };

    s.instrumentSequence = juce::StringArray::fromTokens (x.getProperty ("instrSeq").toString(), ",;\n", "");
    s.instrumentSequence.trim();
    s.instrumentSequence.removeEmptyStrings();

    // "drum kit:40, pad:30" -> normalised weights
    double total = 0.0;
    for (auto& pair : juce::StringArray::fromTokens (x.getProperty ("instrPct").toString(), ",;\n", ""))
    {
        if (! pair.contains (":")) continue;
        const auto key = Lexicon::get().canonicalise (pair.upToLastOccurrenceOf (":", false, false));
        const auto v = juce::jlimit (0.0, 100.0, pair.fromLastOccurrenceOf (":", false, false).trim().getDoubleValue());
        if (key.isNotEmpty() && v > 0) { s.instrumentWeights[key] += v; total += v; }
    }
    for (auto& kv : s.instrumentWeights) kv.second /= total;

    s.silenceTrimDb = f ("trimDb");
    s.removeInternalSilenceMs = f ("internalSilMs");
    s.gateDb = f ("gateDb");
    s.gateReduction = f ("gateReduction");
    s.centerCut = b ("centerCut");
    s.excludePercussive = b ("excludePerc");

    s.targetPeak = f ("targetPeak");
    s.softClip = b ("softClip");
    s.declick = b ("declick");
    s.dcBlock = b ("dcBlock");
    s.sampleRate = rateChoices[i ("renderRate")].getDoubleValue();
    s.format = (OutputFormat) i ("outFormat");
    s.writeConsolidated = true;
    s.writeParts = b ("writeParts");
    return s;
}

void RemixProcessor::startRender (bool exportWhenDone)
{
    cancelRender();
    auto s = buildSettings();
    if (s.inputs.isEmpty())
    {
        appendLog ("Add WAV files or folders first.");
        return;
    }
    progress.store (0.0);
    appendLog ("---- Render started ----");
    job = std::make_unique<RenderJob> (*this, std::move (s), exportWhenDone);
    job->startThread();
}

void RemixProcessor::cancelRender()
{
    if (job != nullptr)
        job->stopThread (30000);
    job.reset();
}

bool RemixProcessor::isRendering() const
{
    return job != nullptr && job->isThreadRunning();
}

bool RemixProcessor::exportTo (const juce::File& folder, juce::String& error)
{
    auto r = getRendered();
    if (r == nullptr) { error = "Render something first."; return false; }

    auto s = buildSettings();
    s.outputDir = folder;
    RenderResult result;
    result.ok = true;
    result.audio = r->audio;          // copy; export runs on the message thread
    result.sampleRate = r->sampleRate;

    Callbacks cb;
    cb.log = [this] (const juce::String& m) { appendLog (m); };
    return ! RemixEngine::writeOutputs (s, result, cb, error).isEmpty();
}

//==============================================================================
void RemixProcessor::publish (std::shared_ptr<const RenderedRemix> r)
{
    {
        const juce::SpinLock::ScopedLockType sl (renderedLock);
        std::swap (rendered, r);
    }
    freePos.store (0.0);
    if (r != nullptr)
    {
        const juce::ScopedLock sl (retiredLock);
        retired.push_back (std::move (r)); // never freed on the audio thread
    }
    purgeRetired();
}

void RemixProcessor::purgeRetired()
{
    const juce::ScopedLock sl (retiredLock);
    retired.erase (std::remove_if (retired.begin(), retired.end(), [] (auto& p) { return p.use_count() == 1; }), retired.end());
}

std::shared_ptr<const RenderedRemix> RemixProcessor::getRendered() const
{
    const juce::SpinLock::ScopedLockType sl (renderedLock);
    return rendered;
}

void RemixProcessor::setPlaying (bool shouldPlay)
{
    playing.store (shouldPlay);
    purgeRetired();
}

void RemixProcessor::seekSeconds (double seconds)
{
    seekRequest.store (std::max (0.0, seconds));
}

//==============================================================================
void RemixProcessor::appendLog (const juce::String& line)
{
    const juce::ScopedLock sl (logLock);
    logLines.add (line);
    if (logLines.size() > 4000)
    {
        logLines.removeRange (0, 1000);
        logBase += 1000;
    }
}

void RemixProcessor::clearLog()
{
    const juce::ScopedLock sl (logLock);
    logBase += logLines.size();
    logLines.clear();
}

juce::StringArray RemixProcessor::getLogLines (int fromIndex, int& nextIndex) const
{
    const juce::ScopedLock sl (logLock);
    juce::StringArray out;
    for (int i = std::max (fromIndex, logBase); i < logBase + logLines.size(); ++i)
        out.add (logLines[i - logBase]);
    nextIndex = logBase + logLines.size();
    return out;
}

//==============================================================================
void RemixProcessor::prepareToPlay (double sampleRate, int)
{
    hostRate = sampleRate;
    levelSmoothed.reset (sampleRate, 0.05);
    levelSmoothed.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (params.getRawParameterValue ("level")->load()));
}

void RemixProcessor::releaseResources() {}

bool RemixProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;
    const auto in = layouts.getMainInputChannelSet();
    return in.isDisabled() || in == juce::AudioChannelSet::stereo();
}

void RemixProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    const int numOut = getTotalNumOutputChannels();

    if (params.getRawParameterValue ("passInput")->load() < 0.5f)
        buffer.clear();
    for (int c = getTotalNumInputChannels(); c < numOut; ++c)
        buffer.clear (c, 0, n);

    std::shared_ptr<const RenderedRemix> r;
    {
        const juce::SpinLock::ScopedTryLockType tl (renderedLock);
        if (tl.isLocked()) r = rendered;
    }
    if (r == nullptr || r->audio.empty() || numOut < 2)
        return;

    const auto len = (double) r->audio.size();
    const double step = r->sampleRate / hostRate;
    const bool loop = params.getRawParameterValue ("loopPlay")->load() >= 0.5f;
    levelSmoothed.setTargetValue (juce::Decibels::decibelsToGain (params.getRawParameterValue ("level")->load(), -60.0f));

    if (const auto seek = seekRequest.exchange (-1.0); seek >= 0.0)
        freePos.store (seek * r->sampleRate);

    double pos = 0.0;
    bool active = false, followingHost = false;

    if (params.getRawParameterValue ("syncHost")->load() >= 0.5f && wrapperType != wrapperType_Standalone)
    {
        if (auto* ph = getPlayHead())
            if (auto info = ph->getPosition(); info.hasValue() && info->getIsPlaying())
                if (auto t = info->getTimeInSeconds())
                {
                    pos = *t * r->sampleRate;
                    active = followingHost = true;
                }
    }
    if (! followingHost && playing.load())
    {
        pos = freePos.load();
        active = true;
    }

    if (! active)
    {
        displayPos.store (freePos.load() / r->sampleRate);
        return;
    }

    auto* outL = buffer.getWritePointer (0);
    auto* outR = buffer.getWritePointer (1);
    bool reachedEnd = false;

    for (int i = 0; i < n; ++i)
    {
        double p = pos;
        if (loop) p = std::fmod (p, len);
        const float g = levelSmoothed.getNextValue();
        if (p >= 0.0 && p < len)
        {
            outL[i] += g * hermite (r->audio.L, p);
            outR[i] += g * hermite (r->audio.R, p);
        }
        else if (! loop && p >= len)
        {
            reachedEnd = true;
        }
        pos += step;
    }

    const double shown = loop ? std::fmod (pos, len) : std::min (pos, len);
    displayPos.store (shown / r->sampleRate);

    if (! followingHost)
    {
        freePos.store (loop ? std::fmod (pos, len) : pos);
        if (reachedEnd)
        {
            playing.store (false);
            freePos.store (0.0);
        }
    }
}

//==============================================================================
void RemixProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    if (auto xml = params.copyState().createXml())
        copyXmlToBinary (*xml, dest);
}

void RemixProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName (params.state.getType()))
        {
            params.replaceState (juce::ValueTree::fromXml (*xml));

            // With a fixed seed the render is reproducible, so rebuild it in the
            // background when a session is reopened (no file export).
            const auto seed = extra().getProperty ("seed").toString().getLargeIntValue();
            if (seed != 0 && ! getInputs().isEmpty())
                juce::MessageManager::callAsync ([safe = juce::WeakReference<RemixProcessor> (this)]
                {
                    if (auto* p = safe.get())
                        p->startRender (false);
                });
        }
}

juce::AudioProcessorEditor* RemixProcessor::createEditor()
{
    return new RemixEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new RemixProcessor();
}
