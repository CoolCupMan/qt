#include "AudioLoader.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <cmath>

namespace remix
{

namespace
{
    juce::AudioFormatManager& formats()
    {
        thread_local juce::AudioFormatManager fm;
        thread_local bool registered = false;
        if (! registered)
        {
            fm.registerBasicFormats();
            registered = true;
        }
        return fm;
    }

    //==========================================================================
    // Fallback WAV reader for what JUCE's WAV reader rejects:
    // G.711 mu-law / A-law (format tags 7 / 6) and 64-bit IEEE float.
    float decodeMuLaw (juce::uint8 u) noexcept
    {
        u = (juce::uint8) ~u;
        const int sign     = u & 0x80;
        const int exponent = (u >> 4) & 0x07;
        const int mantissa = u & 0x0F;
        int sample = (((mantissa << 3) + 0x84) << exponent) - 0x84;
        return (float) (sign ? -sample : sample) / 32768.0f;
    }

    float decodeALaw (juce::uint8 a) noexcept
    {
        a ^= 0x55;
        int t = (a & 0x0F) << 4;
        const int seg = (a & 0x70) >> 4;
        switch (seg)
        {
            case 0:  t += 8; break;
            case 1:  t += 0x108; break;
            default: t += 0x108; t <<= (seg - 1); break;
        }
        return (float) ((a & 0x80) ? t : -t) / 32768.0f;
    }

    struct FallbackWav
    {
        int formatTag = 0, channels = 0, bits = 0;
        double sampleRate = 0;
        juce::int64 dataOffset = -1, dataSize = 0;
    };

    std::optional<FallbackWav> parseFallbackHeader (juce::InputStream& in)
    {
        char id[4];
        if (in.read (id, 4) != 4) return {};
        const bool rf64 = std::memcmp (id, "RF64", 4) == 0;
        if (! rf64 && std::memcmp (id, "RIFF", 4) != 0) return {};
        in.readInt();
        if (in.read (id, 4) != 4 || std::memcmp (id, "WAVE", 4) != 0) return {};

        FallbackWav w;
        juce::int64 ds64DataSize = -1;

        while (! in.isExhausted())
        {
            if (in.read (id, 4) != 4) break;
            const auto size = (juce::int64) (juce::uint32) in.readInt();
            const auto start = in.getPosition();

            if (std::memcmp (id, "ds64", 4) == 0)
            {
                in.readInt64();                 // riff size
                ds64DataSize = in.readInt64();  // data size
            }
            else if (std::memcmp (id, "fmt ", 4) == 0)
            {
                w.formatTag  = (juce::uint16) in.readShort();
                w.channels   = (juce::uint16) in.readShort();
                w.sampleRate = (double) (juce::uint32) in.readInt();
                in.readInt(); in.readShort();
                w.bits = in.readShort();
                if (w.formatTag == 0xFFFE && size >= 40)
                {
                    in.readShort(); in.readShort(); in.readInt();
                    w.formatTag = (juce::uint16) in.readShort(); // first two bytes of SubFormat GUID
                }
            }
            else if (std::memcmp (id, "data", 4) == 0)
            {
                w.dataOffset = start;
                w.dataSize = (rf64 && size == 0xFFFFFFFF && ds64DataSize >= 0) ? ds64DataSize : size;
                break;
            }
            in.setPosition (start + size + (size & 1));
        }

        const bool g711 = (w.formatTag == 6 || w.formatTag == 7) && w.bits == 8;
        const bool f64  = w.formatTag == 3 && w.bits == 64;
        if ((g711 || f64) && w.channels > 0 && w.dataOffset >= 0 && w.sampleRate > 0)
            return w;
        return {};
    }

    bool readFallback (const juce::File& f, juce::AudioBuffer<float>& out, double& sr)
    {
        auto in = f.createInputStream();
        if (in == nullptr) return false;
        auto hdr = parseFallbackHeader (*in);
        if (! hdr) return false;

        const int bytesPerSample = hdr->bits / 8;
        const int blockAlign = bytesPerSample * hdr->channels;
        const auto frames = (int) std::min<juce::int64> (hdr->dataSize / blockAlign, std::numeric_limits<int>::max() / 16);
        out.setSize (hdr->channels, frames);
        in->setPosition (hdr->dataOffset);

        juce::HeapBlock<juce::uint8> raw ((size_t) frames * (size_t) blockAlign);
        const auto got = in->read (raw.get(), (size_t) frames * (size_t) blockAlign);
        const int gotFrames = (int) (got / blockAlign);

        for (int i = 0; i < gotFrames; ++i)
            for (int c = 0; c < hdr->channels; ++c)
            {
                const auto* b = raw.get() + (size_t) i * (size_t) blockAlign + (size_t) (c * bytesPerSample);
                float v;
                if (hdr->formatTag == 3)
                {
                    juce::uint64 bitsLE = juce::ByteOrder::littleEndianInt64 (b);
                    double d;
                    std::memcpy (&d, &bitsLE, 8);
                    v = (float) d;
                }
                else
                {
                    v = hdr->formatTag == 7 ? decodeMuLaw (*b) : decodeALaw (*b);
                }
                out.setSample (c, i, v);
            }
        out.setSize (hdr->channels, gotFrames, true);
        sr = hdr->sampleRate;
        return true;
    }

    //==========================================================================
    // Band-limited resampler: Kaiser-windowed sinc, polyphase table, cutoff
    // follows the ratio so downsampling does not alias.
    struct SincTable
    {
        static constexpr int zeroCrossings = 24;
        static constexpr int phases = 512;
        std::vector<float> table; // covers t in [0, zeroCrossings]

        SincTable()
        {
            const double beta = 8.6;
            auto i0 = [] (double x)
            {
                double sum = 1.0, term = 1.0;
                for (int k = 1; k < 32; ++k)
                {
                    term *= (x / (2.0 * k)) * (x / (2.0 * k));
                    sum += term;
                }
                return sum;
            };
            const double denom = i0 (beta);
            table.resize ((size_t) (zeroCrossings * phases + 2));
            for (size_t n = 0; n < table.size(); ++n)
            {
                const double t = (double) n / phases;
                const double sinc = n == 0 ? 1.0 : std::sin (juce::MathConstants<double>::pi * t) / (juce::MathConstants<double>::pi * t);
                const double r = t / zeroCrossings;
                const double w = r >= 1.0 ? 0.0 : i0 (beta * std::sqrt (1.0 - r * r)) / denom;
                table[n] = (float) (sinc * w);
            }
        }

        float at (double t) const noexcept
        {
            t = std::abs (t) * phases;
            const auto i = (size_t) t;
            if (i + 1 >= table.size()) return 0.0f;
            const float frac = (float) (t - (double) i);
            return table[i] + frac * (table[i + 1] - table[i]);
        }
    };

    std::vector<float> resample (const float* in, size_t n, double srcRate, double dstRate)
    {
        static const SincTable sinc;
        const double step = srcRate / dstRate;               // input samples per output sample
        const double fc = std::min (1.0, dstRate / srcRate) * 0.97; // cutoff relative to input Nyquist
        const double halfWidth = SincTable::zeroCrossings / fc;
        const auto outLen = (size_t) std::llround ((double) n / step);

        std::vector<float> out (outLen);
        for (size_t o = 0; o < outLen; ++o)
        {
            const double pos = (double) o * step;
            const auto first = (juce::int64) std::ceil (pos - halfWidth);
            const auto last  = (juce::int64) std::floor (pos + halfWidth);
            double acc = 0.0;
            for (auto k = std::max<juce::int64> (0, first); k <= last && k < (juce::int64) n; ++k)
                acc += (double) in[k] * sinc.at (((double) k - pos) * fc);
            out[o] = (float) (acc * fc);
        }
        return out;
    }

    void toStereo (const juce::AudioBuffer<float>& src, Clip& dst)
    {
        const int n = src.getNumSamples();
        const int ch = src.getNumChannels();
        dst.resize ((size_t) n);
        if (ch <= 0) return;

        if (ch == 1)
        {
            std::copy_n (src.getReadPointer (0), n, dst.L.data());
            std::copy_n (src.getReadPointer (0), n, dst.R.data());
            return;
        }

        std::copy_n (src.getReadPointer (0), n, dst.L.data());
        std::copy_n (src.getReadPointer (1), n, dst.R.data());
        if (ch == 2) return;

        // Fold-down for surround (L R C LFE Ls Rs ...); LFE is dropped.
        constexpr float k = 0.70710678f;
        for (int c = 2; c < ch; ++c)
        {
            if (c == 3 && ch >= 6) continue;
            const float* s = src.getReadPointer (c);
            const bool centre = (c == 2);
            const bool left   = ! centre && ((c - (ch >= 6 ? 4 : 3)) % 2 == 0);
            for (int i = 0; i < n; ++i)
            {
                if (centre || left)   dst.L[(size_t) i] += k * s[i];
                if (centre || ! left) dst.R[(size_t) i] += k * s[i];
            }
        }
    }
}

//==============================================================================
bool AudioLoader::isSupportedExtension (const juce::File& f)
{
    return f.hasFileExtension ("wav;wave;bwf;aif;aiff;aifc;flac;ogg;mp3");
}

std::optional<AudioLoader::Info> AudioLoader::probe (const juce::File& f)
{
    if (std::unique_ptr<juce::AudioFormatReader> r { formats().createReaderFor (f) })
        return Info { r->sampleRate, r->lengthInSamples, (int) r->numChannels };

    if (auto in = f.createInputStream())
        if (auto h = parseFallbackHeader (*in))
            return Info { h->sampleRate, h->dataSize / (h->channels * h->bits / 8), h->channels };

    return {};
}

std::optional<Clip> AudioLoader::load (const juce::File& f, double targetRate, juce::String& error)
{
    juce::AudioBuffer<float> buffer;
    double srcRate = 0;

    if (std::unique_ptr<juce::AudioFormatReader> r { formats().createReaderFor (f) })
    {
        const auto maxFrames = (juce::int64) std::numeric_limits<int>::max() / 4;
        if (r->lengthInSamples > maxFrames)
        {
            error = "file is too long";
            return {};
        }
        buffer.setSize ((int) juce::jmax (1u, r->numChannels), (int) r->lengthInSamples);
        if (! r->read (&buffer, 0, (int) r->lengthInSamples, 0, true, true))
        {
            error = "read error";
            return {};
        }
        srcRate = r->sampleRate;
    }
    else if (! readFallback (f, buffer, srcRate))
    {
        error = "unsupported or damaged audio file";
        return {};
    }

    if (srcRate <= 0 || buffer.getNumSamples() == 0)
    {
        error = "empty file";
        return {};
    }

    Clip clip;
    toStereo (buffer, clip);

    if (std::abs (srcRate - targetRate) >= 1.0)
    {
        clip.L = resample (clip.L.data(), clip.L.size(), srcRate, targetRate);
        clip.R = resample (clip.R.data(), clip.R.size(), srcRate, targetRate);
    }

    // Sanitize NaN / inf from broken float files.
    for (auto* ch : { &clip.L, &clip.R })
        for (auto& s : *ch)
            if (! std::isfinite (s)) s = 0.0f;

    return clip;
}

} // namespace remix
