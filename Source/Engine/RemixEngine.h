#pragma once

#include "Settings.h"
#include "Lexicon.h"
#include <functional>

namespace remix
{

struct Callbacks
{
    std::function<void (const juce::String&)> log;
    std::function<void (double)>              progress;   // 0..1
    std::function<bool()>                     cancelled;
};

struct RenderResult
{
    Clip         audio;
    double       sampleRate = 44100.0;
    juce::uint32 seedUsed   = 0;
    bool         ok         = false;
    bool         wasCancelled = false;
    juce::String error;
    juce::Array<juce::File> filesUsed;
};

/** The remixer. Builds a track purely out of the input material:

    - Loop 63/27 (default): every 63 s block plays three recurring 21 s motifs
      in rotating order (A B C, B C A, C A B ...), every 27 s block is a fresh
      rhythmic variant cut from the inputs (short slices, reverses, drum
      layering). Stutters are allowed but at least `stutterCooldownSec` apart.
    - Montage: segments are strung together in the chosen file order, with
      the 63/27 motif grid optionally laid over it.

    Every join is zero-cross aligned and equal-power crossfaded, segment
    edges get micro-fades, and the master pass removes DC, normalises,
    repairs spikes and optionally soft-clips. */
class RemixEngine
{
public:
    /** Expands folders, keeps supported audio, skips our own outputs. */
    static juce::Array<juce::File> gatherInputs (const Settings& s, const Callbacks& cb);

    static RenderResult render (const Settings& s, const Callbacks& cb);

    /** Writes the consolidated file and/or parts. Returns the files written. */
    static juce::Array<juce::File> writeOutputs (const Settings& s, const RenderResult& r,
                                                 const Callbacks& cb, juce::String& error);

    /** "seconds", "mm:ss" or "hh:mm:ss" -> seconds (<= 0 when invalid). */
    static double parseDuration (const juce::String& text);
};

} // namespace remix
