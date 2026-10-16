#pragma once

#include "Settings.h"
#include <optional>

namespace remix
{

/** Reads any source file into stereo float at the engine sample rate.

    Covers everything the Python versions handled and more:
    PCM 8/16/24/32, IEEE float 32, WAVE_FORMAT_EXTENSIBLE, RF64 (via JUCE),
    64-bit float and G.711 mu-law / A-law (own fallback reader), plus AIFF, FLAC, Ogg and MP3.
    Mono is duplicated, surround is folded down (L/R + 0.707 C + surrounds,
    LFE dropped) and resampling uses a windowed-sinc interpolator instead of
    linear interpolation. */
struct AudioLoader
{
    struct Info
    {
        double       sampleRate = 0;
        juce::int64  numFrames  = 0;
        int          numChannels = 0;
        double seconds() const { return sampleRate > 0 ? (double) numFrames / sampleRate : 0.0; }
    };

    static bool isSupportedExtension (const juce::File& f);
    static std::optional<Info> probe (const juce::File& f);
    static std::optional<Clip> load (const juce::File& f, double targetSampleRate, juce::String& error);
};

} // namespace remix
