#pragma once

#include "Settings.h"

namespace remix
{

/** Streams a Clip to disk as WAV. Writes a normal RIFF file when it fits in
    4 GB and switches to RF64 (ds64 chunk) automatically when it does not,
    so long renders never get truncated or skipped. 16-bit output is TPDF
    dithered. */
struct WavWriter
{
    static bool write (const juce::File& file, const Clip& clip, size_t start, size_t numFrames,
                       double sampleRate, OutputFormat format, juce::String& error);

    /** Writes `<base>_partNN.wav` files of at most `partBytes` audio data each. */
    static juce::Array<juce::File> writeParts (const juce::File& dir, const juce::String& baseName,
                                               const Clip& clip, double sampleRate, OutputFormat format,
                                               juce::int64 partBytes, juce::String& error);

    static int bytesPerSample (OutputFormat f) noexcept { return f == OutputFormat::Pcm16 ? 2 : f == OutputFormat::Pcm24 ? 3 : 4; }
};

} // namespace remix
