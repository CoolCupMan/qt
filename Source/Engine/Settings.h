#pragma once

#include <juce_core/juce_core.h>
#include <array>
#include <map>
#include <vector>

namespace remix
{

/** Stereo float audio. All engine processing happens at one sample rate. */
struct Clip
{
    std::vector<float> L, R;

    size_t size() const noexcept   { return L.size(); }
    bool   empty() const noexcept  { return L.empty(); }
    void   resize (size_t n)       { L.resize (n, 0.0f); R.resize (n, 0.0f); }
    void   reserve (size_t n)      { L.reserve (n); R.reserve (n); }
    void   clear()                 { L.clear(); R.clear(); }

    Clip slice (size_t start, size_t len) const
    {
        Clip c;
        start = std::min (start, size());
        len   = std::min (len, size() - start);
        c.L.assign (L.begin() + (std::ptrdiff_t) start, L.begin() + (std::ptrdiff_t) (start + len));
        c.R.assign (R.begin() + (std::ptrdiff_t) start, R.begin() + (std::ptrdiff_t) (start + len));
        return c;
    }
};

enum class EngineMode   { Loop63_27 = 0, Montage = 1 };
enum class SortScope    { Both = 1, PrimaryOnly = 2, SecondaryOnly = 3 };
enum class PrimaryMode  { CustomSequence = 1, FamilyPriority = 2 };
enum class OrderMode    { Alphabetical = 1, SizeDesc = 2, Random = 3, RandomBySize = 4, Mix = 5 };
enum class OutputFormat { Pcm16 = 0, Pcm24 = 1, Float32 = 2 };

/** Every knob of the remixer. Defaults are the "clean" settings that the
    later Python versions converged on (no gate, -80 dB trim, 120 ms
    equal-power crossfades, 0.88 peak, no soft clip, de-click on). */
struct Settings
{
    // ---- inputs / output ----------------------------------------------------
    juce::StringArray inputs;                 // files and/or folders (scanned recursively)
    juce::File        outputDir;
    juce::String      outputName      = "output.wav";
    juce::String      partsBaseName   = "remix_output";
    bool              writeConsolidated = true;
    bool              writeParts        = false;
    juce::int64       partBytes         = 1'800'000'000;
    OutputFormat      format            = OutputFormat::Pcm16;
    double            sampleRate        = 44100.0;
    double            targetSeconds     = 300.0;

    // ---- source cleanup -----------------------------------------------------
    float silenceTrimDb         = -80.0f;
    float minSilenceMs          = 30.0f;
    float removeInternalSilenceMs = 0.0f;   // 0 = keep internal pauses
    float gateDb                = -120.0f;
    float gateReduction         = 1.0f;     // 1 = gate off
    bool  centerCut             = false;    // vocal-reduction (mid cancel + bass return)
    float centerAtten           = 0.90f;
    float centerBassHz          = 160.0f;
    float centerBassReturn      = 0.35f;
    bool  excludePercussive     = false;    // skip one-shots/drum files entirely
    float excludeMaxSeconds     = 1.8f;

    // ---- ordering -----------------------------------------------------------
    SortScope   sortScope   = SortScope::Both;
    PrimaryMode primaryMode = PrimaryMode::FamilyPriority;
    juce::StringArray instrumentSequence;          // canonical names, top -> bottom
    std::map<juce::String, double> instrumentWeights; // canon -> 0..1 (normalised)
    OrderMode   orderMode   = OrderMode::Alphabetical;
    std::array<double, 4> mixWeights { 1.0, 0.0, 0.0, 0.0 };

    // ---- engine -------------------------------------------------------------
    EngineMode engineMode   = EngineMode::Loop63_27;
    float segMinMs          = 800.0f;
    float segMaxMs          = 4000.0f;
    float reverseProb       = 0.20f;
    float stutterProb       = 0.07f;
    float stutterMs         = 120.0f;
    int   stutterRepeatsMin = 2;
    int   stutterRepeatsMax = 3;
    float stutterCooldownSec = 30.0f;
    float crossfadeMs       = 120.0f;
    float layerDrumsProb    = 0.35f;
    float layerDrumGain     = 0.55f;
    float mainSegGain       = 0.92f;
    float edgeFadeMs        = 6.0f;
    float zeroCrossMs       = 12.0f;
    float longBlockSec      = 63.0f;        // split into `subLoops` recurring motifs
    int   subLoops          = 3;
    float shortBlockSec     = 27.0f;
    bool  montageStructure  = true;         // montage mode: overlay the 63/27 grid
    bool  periodicFx        = false;        // timed stutter + bit-crush glitch overlays
    float periodicStutterSec = 33.0f;
    float periodicGlitchSec  = 67.0f;
    float stutterOverlayMs   = 140.0f;
    float glitchOverlayMs    = 90.0f;
    int   glitchCrush        = 6;

    // ---- master -------------------------------------------------------------
    float targetPeak = 0.88f;
    bool  softClip   = false;
    bool  dcBlock    = true;
    bool  declick    = true;

    juce::uint32 seed = 0;                  // 0 = new random seed every render
};

} // namespace remix
