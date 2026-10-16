#include "WavWriter.h"
#include <random>

namespace remix
{

namespace
{
    void put16 (juce::OutputStream& o, int v)          { o.writeShort ((short) v); }
    void put32 (juce::OutputStream& o, juce::uint32 v) { o.writeInt ((int) v); }
    void put64 (juce::OutputStream& o, juce::uint64 v) { o.writeInt64 ((juce::int64) v); }
    void tag   (juce::OutputStream& o, const char* t)  { o.write (t, 4); }
}

bool WavWriter::write (const juce::File& file, const Clip& clip, size_t start, size_t numFrames,
                       double sampleRate, OutputFormat format, juce::String& error)
{
    start = std::min (start, clip.size());
    numFrames = std::min (numFrames, clip.size() - start);

    const int bps = bytesPerSample (format);
    const int channels = 2;
    const int blockAlign = bps * channels;
    const bool isFloat = format == OutputFormat::Float32;
    const auto dataBytes = (juce::uint64) numFrames * (juce::uint64) blockAlign;
    const juce::uint32 fmtSize = isFloat ? 18 : 16;
    const juce::uint32 factSize = isFloat ? 12 : 0;      // "fact" chunk incl. header
    const auto padByte = (juce::uint64) (dataBytes & 1);

    const juce::uint64 riffPayload = 4 + (8 + fmtSize) + factSize + 8 + dataBytes + padByte;
    const bool rf64 = riffPayload + 36 > 0xFFFFFFFFull;

    file.deleteFile();
    juce::FileOutputStream out (file);
    if (! out.openedOk())
    {
        error = "cannot open " + file.getFullPathName() + " for writing";
        return false;
    }

    const auto sr = (juce::uint32) std::lround (sampleRate);

    if (rf64)
    {
        tag (out, "RF64"); put32 (out, 0xFFFFFFFF); tag (out, "WAVE");
        tag (out, "ds64"); put32 (out, 28);
        put64 (out, riffPayload + 36);  // RIFF size incl. ds64 chunk
        put64 (out, dataBytes);
        put64 (out, numFrames);
        put32 (out, 0);
    }
    else
    {
        tag (out, "RIFF"); put32 (out, (juce::uint32) riffPayload); tag (out, "WAVE");
    }

    tag (out, "fmt "); put32 (out, fmtSize);
    put16 (out, isFloat ? 3 : 1);
    put16 (out, channels);
    put32 (out, sr);
    put32 (out, sr * (juce::uint32) blockAlign);
    put16 (out, blockAlign);
    put16 (out, bps * 8);
    if (isFloat) put16 (out, 0);

    if (isFloat)
    {
        tag (out, "fact"); put32 (out, 4);
        put32 (out, rf64 ? 0xFFFFFFFF : (juce::uint32) numFrames);
    }

    tag (out, "data"); put32 (out, rf64 ? 0xFFFFFFFF : (juce::uint32) dataBytes);

    std::mt19937 rng (0x5eed);
    std::uniform_real_distribution<float> uni (-0.5f, 0.5f);

    constexpr size_t chunk = 1 << 15;
    juce::HeapBlock<char> buf (chunk * (size_t) blockAlign);

    for (size_t pos = 0; pos < numFrames; pos += chunk)
    {
        const size_t n = std::min (chunk, numFrames - pos);
        char* p = buf.get();

        for (size_t i = 0; i < n; ++i)
        {
            for (const auto* ch : { &clip.L, &clip.R })
            {
                const float x = (*ch)[start + pos + i];
                if (format == OutputFormat::Float32)
                {
                    const float v = juce::jlimit (-1.0f, 1.0f, x);
                    juce::uint32 bits;
                    std::memcpy (&bits, &v, 4);
                    p[0] = (char) (bits & 0xff); p[1] = (char) ((bits >> 8) & 0xff);
                    p[2] = (char) ((bits >> 16) & 0xff); p[3] = (char) ((bits >> 24) & 0xff);
                    p += 4;
                }
                else if (format == OutputFormat::Pcm24)
                {
                    const auto v = (int) juce::jlimit (-8388608.0, 8388607.0, std::round ((double) x * 8388607.0));
                    p[0] = (char) (v & 0xff); p[1] = (char) ((v >> 8) & 0xff); p[2] = (char) ((v >> 16) & 0xff);
                    p += 3;
                }
                else
                {
                    const float dither = uni (rng) + uni (rng); // TPDF, +-1 LSB
                    const auto v = (int) juce::jlimit (-32768.0f, 32767.0f, std::round (x * 32767.0f + dither));
                    p[0] = (char) (v & 0xff); p[1] = (char) ((v >> 8) & 0xff);
                    p += 2;
                }
            }
        }

        if (! out.write (buf.get(), (size_t) (p - buf.get())))
        {
            error = "write failed (disk full?) for " + file.getFullPathName();
            return false;
        }
    }

    if (padByte) out.writeByte (0);
    out.flush();
    if (out.getStatus().failed())
    {
        error = out.getStatus().getErrorMessage();
        return false;
    }
    return true;
}

juce::Array<juce::File> WavWriter::writeParts (const juce::File& dir, const juce::String& baseName,
                                               const Clip& clip, double sampleRate, OutputFormat format,
                                               juce::int64 partBytes, juce::String& error)
{
    juce::Array<juce::File> written;
    const auto framesPerPart = (size_t) std::max<juce::int64> (1, partBytes / (2 * bytesPerSample (format)));

    int part = 1;
    for (size_t start = 0; start < clip.size(); start += framesPerPart, ++part)
    {
        auto f = dir.getChildFile (baseName + "_part" + juce::String (part).paddedLeft ('0', 2) + ".wav");
        if (! write (f, clip, start, framesPerPart, sampleRate, format, error))
            break;
        written.add (f);
    }
    return written;
}

} // namespace remix
