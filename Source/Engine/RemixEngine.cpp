#include "RemixEngine.h"
#include "AudioLoader.h"
#include "WavWriter.h"
#include <cmath>
#include <numeric>
#include <random>

namespace remix
{

namespace
{
    constexpr float halfPi = juce::MathConstants<float>::halfPi;

    float dbToLin (float db) { return std::pow (10.0f, db / 20.0f); }

    //==========================================================================
    // Small DSP toolkit working on Clip
    //==========================================================================
    void applyGain (Clip& c, float g)
    {
        for (auto* ch : { &c.L, &c.R })
            for (auto& s : *ch) s *= g;
    }

    void reverseClip (Clip& c)
    {
        std::reverse (c.L.begin(), c.L.end());
        std::reverse (c.R.begin(), c.R.end());
    }

    /** Equal-power fade-in on the head and fade-out on the tail. */
    void edgeFade (Clip& c, size_t n)
    {
        n = std::min (n, c.size() / 2);
        if (n == 0) return;
        const size_t len = c.size();
        for (size_t i = 0; i < n; ++i)
        {
            const float t = ((float) i + 0.5f) / (float) n;
            const float gin = std::sin (t * halfPi);
            const float gout = std::cos (t * halfPi);
            c.L[i] *= gin;  c.R[i] *= gin;
            c.L[len - n + i] *= gout;
            c.R[len - n + i] *= gout;
        }
    }

    float mid (const Clip& c, size_t i) noexcept { return c.L[i] + c.R[i]; }

    /** Length to cut `c` to so it ends on the last zero crossing inside the
        final `search` frames (unchanged if there is none). */
    size_t zeroCrossTail (const Clip& c, size_t search)
    {
        const size_t n = c.size();
        if (search == 0 || n < 2 * search) return n;
        for (size_t i = n - 1; i > n - search; --i)
            if ((mid (c, i - 1) <= 0.0f) != (mid (c, i) <= 0.0f))
                return i;
        return n;
    }

    /** Offset of the first zero crossing within the first `search` frames. */
    size_t zeroCrossHead (const Clip& c, size_t search)
    {
        if (search == 0 || c.size() < 2 * search) return 0;
        for (size_t i = 1; i < search; ++i)
            if ((mid (c, i - 1) <= 0.0f) != (mid (c, i) <= 0.0f))
                return i;
        return 0;
    }

    /** dst = dst ⨯ src, joined with an equal-power crossfade of `cf` frames.
        Unlike the Python helper this works in place (no O(n²) copying) and
        never touches the head of `dst` again. */
    void appendCrossfade (Clip& dst, const Clip& src, size_t cf, size_t zcSearch, size_t microFade)
    {
        if (src.empty()) return;
        if (dst.empty())
        {
            dst.L.insert (dst.L.end(), src.L.begin(), src.L.end());
            dst.R.insert (dst.R.end(), src.R.begin(), src.R.end());
            return;
        }

        dst.resize (zeroCrossTail (dst, zcSearch));
        const size_t srcStart = zeroCrossHead (src, zcSearch);
        const size_t srcLen = src.size() - srcStart;
        const size_t c = std::min ({ cf, dst.size(), srcLen });
        const size_t base = dst.size() - c;

        if (c < 2)
        {
            // Butt joint: micro-fade both sides so there is no step.
            const size_t m = std::min ({ microFade, dst.size() / 2, srcLen / 2 });
            for (size_t i = 0; i < m; ++i)
            {
                const float g = std::cos (((float) i + 0.5f) / (float) m * halfPi);
                dst.L[dst.size() - m + i] *= g;
                dst.R[dst.size() - m + i] *= g;
            }
            const size_t old = dst.size();
            dst.L.insert (dst.L.end(), src.L.begin() + (std::ptrdiff_t) srcStart, src.L.end());
            dst.R.insert (dst.R.end(), src.R.begin() + (std::ptrdiff_t) srcStart, src.R.end());
            for (size_t i = 0; i < m; ++i)
            {
                const float g = std::sin (((float) i + 0.5f) / (float) m * halfPi);
                dst.L[old + i] *= g;
                dst.R[old + i] *= g;
            }
            return;
        }

        for (size_t i = 0; i < c; ++i)
        {
            const float t = ((float) i + 0.5f) / (float) c;
            const float go = std::cos (t * halfPi), gi = std::sin (t * halfPi);
            dst.L[base + i] = dst.L[base + i] * go + src.L[srcStart + i] * gi;
            dst.R[base + i] = dst.R[base + i] * go + src.R[srcStart + i] * gi;
        }
        dst.L.insert (dst.L.end(), src.L.begin() + (std::ptrdiff_t) (srcStart + c), src.L.end());
        dst.R.insert (dst.R.end(), src.R.begin() + (std::ptrdiff_t) (srcStart + c), src.R.end());
    }

    /** Replaces base[at, at + seg.size()) by seg, crossfading both seams.
        The length of `base` never changes (the Python version inserted and
        so silently pushed everything later). */
    void overwriteCrossfade (Clip& base, const Clip& seg, size_t at, size_t cf)
    {
        if (at >= base.size() || seg.empty()) return;
        const size_t len = std::min (seg.size(), base.size() - at);
        const size_t c = std::min (cf, len / 2);

        for (size_t i = 0; i < len; ++i)
        {
            float gNew = 1.0f;
            if (c > 0 && i < c)                 gNew = std::sin (((float) i + 0.5f) / (float) c * halfPi);
            else if (c > 0 && i >= len - c)     gNew = std::cos (((float) (i - (len - c)) + 0.5f) / (float) c * halfPi);
            const float gOld = std::sqrt (std::max (0.0f, 1.0f - gNew * gNew));
            base.L[at + i] = base.L[at + i] * gOld + seg.L[i] * gNew;
            base.R[at + i] = base.R[at + i] * gOld + seg.R[i] * gNew;
        }
    }

    /** Loops `src` to exactly `len` frames with crossfaded loop points
        (the Python tiling butted copies together -> a click every repeat). */
    Clip loopTile (const Clip& src, size_t len, size_t cf, size_t microFade)
    {
        Clip out;
        if (src.size() < 16 || len == 0) { out.resize (len); return out; }
        out.reserve (len + src.size());
        const size_t c = std::min (cf, src.size() / 4);
        while (out.size() < len)
            appendCrossfade (out, src, c, 0, microFade);
        out.resize (len);
        return out;
    }

    void stutterTail (Clip& c, size_t tail, int reps, size_t microFade)
    {
        if (c.size() <= tail || tail < 8) return;
        Clip chunk = c.slice (c.size() - tail, tail);
        for (int r = 0; r < reps; ++r)
            appendCrossfade (c, chunk, microFade, 0, microFade);
    }

    void sampleHold (Clip& c, int factor)
    {
        if (factor <= 1) return;
        for (size_t i = 0; i < c.size(); ++i)
        {
            const size_t h = i - (i % (size_t) factor);
            c.L[i] = c.L[h];
            c.R[i] = c.R[h];
        }
    }

    float peakOf (const Clip& c)
    {
        float p = 0.0f;
        for (auto* ch : { &c.L, &c.R })
            for (auto s : *ch) p = std::max (p, std::abs (s));
        return p;
    }

    //==========================================================================
    // Source cleanup
    //==========================================================================
    float frameAmp (const Clip& c, size_t i) noexcept { return 0.5f * (std::abs (c.L[i]) + std::abs (c.R[i])); }

    void trimSilence (Clip& c, double sr, float trimDb, float minSilMs)
    {
        const float thr = dbToLin (trimDb);
        const size_t block = std::max<size_t> (1, (size_t) (sr * minSilMs / 1000.0));
        const size_t pad = (size_t) (0.005 * sr);
        const size_t n = c.size();

        auto blockLoud = [&] (size_t a, size_t b)
        {
            for (size_t k = a; k < b; ++k)
                if (frameAmp (c, k) > thr) return true;
            return false;
        };

        size_t start = n;
        for (size_t i = 0; i < n; i += block)
            if (blockLoud (i, std::min (n, i + block))) { start = i > pad ? i - pad : 0; break; }

        if (start >= n) { c.clear(); return; }

        size_t end = n;
        for (size_t j = n; j > 0;)
        {
            const size_t a = j > block ? j - block : 0;
            if (blockLoud (a, j)) { end = std::min (n, j + pad); break; }
            j = a;
        }

        if (start > 0 || end < n)
            c = c.slice (start, end - start);
    }

    /** Smoothed downward gate (the Python gate switched gain per sample,
        which crackles on anything near the threshold). */
    void gate (Clip& c, double sr, float gateDb, float reduction)
    {
        if (reduction >= 1.0f) return;
        const float thr = dbToLin (gateDb);
        const float rel = std::exp (-1.0f / (float) (0.050 * sr));
        const float smooth = std::exp (-1.0f / (float) (0.005 * sr));
        float env = 0.0f, g = 1.0f;
        for (size_t i = 0; i < c.size(); ++i)
        {
            const float a = std::max (std::abs (c.L[i]), std::abs (c.R[i]));
            env = a > env ? a : env * rel;
            const float target = env < thr ? reduction : 1.0f;
            g = target + (g - target) * smooth;
            c.L[i] *= g; c.R[i] *= g;
        }
    }

    /** Removes silent stretches longer than `minMs` (joins are crossfaded).
        The Python version dropped every quiet sample, i.e. also the decays. */
    void removeInternalSilence (Clip& c, double sr, float silDb, float minMs)
    {
        if (minMs <= 0.0f || c.empty()) return;
        const float thr = dbToLin (silDb);
        const size_t minRun = (size_t) (sr * minMs / 1000.0);
        const size_t keep = (size_t) (0.010 * sr);
        if (minRun <= 2 * keep) return;

        Clip out;
        out.reserve (c.size());
        size_t segStart = 0, runStart = 0;
        bool inRun = false;

        for (size_t i = 0; i <= c.size(); ++i)
        {
            const bool quiet = i < c.size() && frameAmp (c, i) < thr;
            if (quiet && ! inRun) { inRun = true; runStart = i; }
            if ((! quiet || i == c.size()) && inRun)
            {
                inRun = false;
                if (i - runStart >= minRun)
                {
                    appendCrossfade (out, c.slice (segStart, runStart + keep - segStart), keep, 0, keep);
                    segStart = i - keep;
                }
            }
        }
        appendCrossfade (out, c.slice (segStart, c.size() - segStart), keep, 0, keep);
        c = std::move (out);
    }

    /** Vocal reduction: cancel the centre (mid) and return its low end. */
    void centerCut (Clip& c, double sr, float atten, float bassHz, float bassReturn)
    {
        const float a = std::exp (-juce::MathConstants<float>::twoPi * bassHz / (float) sr);
        float lp = 0.0f;
        for (size_t i = 0; i < c.size(); ++i)
        {
            const float m = 0.5f * (c.L[i] + c.R[i]);
            lp = (1.0f - a) * m + a * lp;
            c.L[i] = c.L[i] - atten * m + bassReturn * lp;
            c.R[i] = c.R[i] - atten * m + bassReturn * lp;
        }
    }

    //==========================================================================
    // Master
    //==========================================================================
    void removeDcMean (Clip& c)
    {
        if (c.empty()) return;
        for (auto* ch : { &c.L, &c.R })
        {
            const double mean = std::accumulate (ch->begin(), ch->end(), 0.0) / (double) ch->size();
            for (auto& s : *ch) s -= (float) mean;
        }
    }

    void dcBlock (Clip& c, double sr)
    {
        const float R = std::exp (-juce::MathConstants<float>::twoPi * 8.0f / (float) sr); // ~8 Hz high-pass
        for (auto* ch : { &c.L, &c.R })
        {
            float x1 = 0.0f, y1 = 0.0f;
            for (auto& s : *ch)
            {
                const float y = s - x1 + R * y1;
                x1 = s; y1 = y; s = y;
            }
        }
    }

    void normalise (Clip& c, float targetPeak)
    {
        const float p = peakOf (c);
        if (p > 1.0e-9f) applyGain (c, targetPeak / p);
    }

    /** Repairs isolated spikes and short bursts of huge sample-to-sample
        jumps (same detector as the RF64 Python version). */
    void declick (std::vector<float>& x, float spikeAbs, float derivTh, int maxBurst)
    {
        const size_t n = x.size();
        if (n < 3) return;

        for (size_t i = 1; i + 1 < n; ++i)
            if (std::abs (x[i]) >= spikeAbs && std::abs (x[i] - x[i - 1]) > derivTh && std::abs (x[i] - x[i + 1]) > derivTh)
                x[i] = 0.5f * (x[i - 1] + x[i + 1]);

        for (size_t i = 1; i + 1 < n;)
        {
            if (std::abs (x[i] - x[i - 1]) > derivTh)
            {
                size_t j = i + 1;
                while (j < n && std::abs (x[j] - x[j - 1]) > derivTh && (int) (j - i) <= maxBurst) ++j;
                const auto burst = (int) (j - i);
                if (burst >= 1 && burst <= maxBurst && j < n)
                {
                    const float x0 = x[i - 1], x1 = x[j];
                    const auto span = (float) (j - (i - 1));
                    for (size_t k = i; k < j; ++k)
                        x[k] = x0 + ((float) (k - (i - 1)) / span) * (x1 - x0);
                    i = j;
                    continue;
                }
            }
            ++i;
        }
    }

    void softClip (Clip& c)
    {
        for (auto* ch : { &c.L, &c.R })
            for (auto& s : *ch) s = std::tanh (1.4f * s);
    }

    //==========================================================================
    struct Segment
    {
        Clip audio;
        int fileRank = 0;
        juce::String tag;
        Family family = Family::Melodic;
        double weight = 1.0;
    };

    struct Source
    {
        juce::File file;
        juce::String tag;      // sequence entry or detected canonical name
        juce::String canon;    // detected canonical name
    };

    //==========================================================================
    class Renderer
    {
    public:
        Renderer (const Settings& st, const Callbacks& c) : s (st), cb (c), sr (st.sampleRate) {}

        RenderResult run()
        {
            RenderResult res;
            res.sampleRate = sr;
            res.seedUsed = s.seed != 0 ? s.seed : (juce::uint32) std::random_device{}();
            rng.seed (res.seedUsed);
            log ("Seed: " + juce::String (res.seedUsed) + "  (enter it as the seed to repeat this exact render)");

            auto files = RemixEngine::gatherInputs (s, cb);
            log ("Found " + juce::String (files.size()) + " audio file(s) across " + juce::String (s.inputs.size()) + " input path(s).");
            if (files.isEmpty()) { res.error = "No audio files found."; return res; }

            presenceReport (files);
            auto sources = orderSources (files);
            if (cancelled()) return cancelledResult (res);

            loadSegments (sources, res);
            if (cancelled()) return cancelledResult (res);
            if (segments.empty()) { res.error = "None of the inputs produced usable audio."; return res; }

            const auto target = (size_t) std::llround (s.targetSeconds * sr);
            log ("Building " + juce::String (s.engineMode == EngineMode::Loop63_27 ? "63/27 looped remix (21/21/21 rotation)" : "montage")
                 + " of " + juce::String (s.targetSeconds, 1) + " s ...");

            Clip out = s.engineMode == EngineMode::Loop63_27 ? buildLoop (target) : buildMontage (target);
            if (cancelled()) return cancelledResult (res);

            if (s.periodicFx) applyPeriodicFx (out);
            progress (0.95);

            master (out);
            progress (1.0);

            res.audio = std::move (out);
            res.ok = true;
            log ("Remix ready: " + juce::String ((double) res.audio.size() / sr, 1) + " s.");
            return res;
        }

    private:
        const Settings& s;
        const Callbacks& cb;
        const double sr;
        std::mt19937 rng;
        std::vector<Segment> segments;
        std::vector<size_t> melodic, drums, perc;
        std::vector<Clip> motifs;
        juce::int64 lastStutterAt = std::numeric_limits<juce::int64>::min() / 2;

        // ---- utilities -------------------------------------------------------
        void log (const juce::String& m) const        { if (cb.log) cb.log (m); }
        void progress (double p) const                { if (cb.progress) cb.progress (juce::jlimit (0.0, 1.0, p)); }
        bool cancelled() const                        { return cb.cancelled && cb.cancelled(); }
        RenderResult& cancelledResult (RenderResult& r) const { r.wasCancelled = true; r.error = "Cancelled."; return r; }

        size_t ms (double v) const                    { return (size_t) std::max (0.0, std::round (v * sr / 1000.0)); }
        double uniform (double a, double b)           { return std::uniform_real_distribution<double> (a, b) (rng); }
        juce::int64 randInt (juce::int64 a, juce::int64 b) { return b <= a ? a : std::uniform_int_distribution<juce::int64> (a, b) (rng); }
        bool chance (double p)                        { return p > 0.0 && uniform (0.0, 1.0) < p; }

        size_t crossfade() const   { return ms (s.crossfadeMs); }
        size_t zcSearch() const    { return ms (s.zeroCrossMs); }
        size_t microFade() const   { return std::max<size_t> (ms (s.edgeFadeMs), 8); }

        size_t pickWeighted (const std::vector<size_t>& idx)
        {
            double total = 0.0;
            for (auto i : idx) total += segments[i].weight;
            double r = uniform (0.0, total);
            for (auto i : idx)
                if ((r -= segments[i].weight) <= 0.0) return i;
            return idx.back();
        }

        bool stutterAllowed (juce::int64 at) const
        {
            return at - lastStutterAt >= (juce::int64) (s.stutterCooldownSec * sr);
        }

        void stutter (Clip& c)
        {
            const auto reps = (int) randInt (s.stutterRepeatsMin, std::max (s.stutterRepeatsMin, s.stutterRepeatsMax));
            stutterTail (c, ms (s.stutterMs), reps, microFade());
        }

        // ---- ordering --------------------------------------------------------
        static juce::String alphaKey (const juce::File& f)
        {
            return f.getFileName().toLowerCase().removeCharacters ("0123456789"); // digits ignored, as in the scripts
        }

        double estimatedBytes (const juce::File& f) const
        {
            if (auto info = AudioLoader::probe (f))
                if (info->sampleRate > 0)
                    return std::max (1.0, (double) info->numFrames * (sr / info->sampleRate) * 4.0);
            return 1.0;
        }

        juce::Array<juce::File> orderFiles (juce::Array<juce::File> files)
        {
            const int n = files.size();
            if (n <= 1) return files;

            auto byAlpha = [] (const juce::File& a, const juce::File& b) { return alphaKey (a) < alphaKey (b); };

            if (s.orderMode == OrderMode::Alphabetical)
            {
                std::stable_sort (files.begin(), files.end(), byAlpha);
                return files;
            }
            if (s.orderMode == OrderMode::Random)
            {
                std::shuffle (files.begin(), files.end(), rng);
                return files;
            }

            std::vector<double> est;
            for (auto& f : files) est.push_back (estimatedBytes (f));
            std::vector<int> idx ((size_t) n);
            std::iota (idx.begin(), idx.end(), 0);

            juce::Array<juce::File> out;
            if (s.orderMode == OrderMode::SizeDesc)
            {
                std::stable_sort (idx.begin(), idx.end(), [&] (int a, int b) { return est[(size_t) a] > est[(size_t) b]; });
                for (auto i : idx) out.add (files[i]);
                return out;
            }
            if (s.orderMode == OrderMode::RandomBySize)
            {
                while (! idx.empty())
                {
                    double total = 0.0;
                    for (auto i : idx) total += est[(size_t) i];
                    double r = uniform (0.0, total);
                    size_t pick = idx.size() - 1;
                    for (size_t k = 0; k < idx.size(); ++k)
                        if ((r -= est[(size_t) idx[k]]) <= 0.0) { pick = k; break; }
                    out.add (files[idx[pick]]);
                    idx.erase (idx.begin() + (std::ptrdiff_t) pick);
                }
                return out;
            }

            // MIX: weighted blend of alpha rank, size rank, random-by-alpha, random-by-size.
            std::vector<double> alphaRank ((size_t) n), sizeRank ((size_t) n), score ((size_t) n);
            {
                auto a = idx;
                std::stable_sort (a.begin(), a.end(), [&] (int x, int y) { return byAlpha (files[x], files[y]); });
                for (int r = 0; r < n; ++r) alphaRank[(size_t) a[(size_t) r]] = (double) r / (n - 1);
                auto b = idx;
                std::stable_sort (b.begin(), b.end(), [&] (int x, int y) { return est[(size_t) x] > est[(size_t) y]; });
                for (int r = 0; r < n; ++r) sizeRank[(size_t) b[(size_t) r]] = (double) r / (n - 1);
            }
            const double maxEst = *std::max_element (est.begin(), est.end());
            double w[4];
            const double wt = s.mixWeights[0] + s.mixWeights[1] + s.mixWeights[2] + s.mixWeights[3];
            for (int k = 0; k < 4; ++k) w[k] = wt > 0 ? s.mixWeights[(size_t) k] / wt : (k == 0 ? 1.0 : 0.0);

            for (size_t i = 0; i < (size_t) n; ++i)
            {
                const double randAlpha = uniform (0.0, 1.0) / std::max (1e-9, 1.0 - alphaRank[i]);
                const double randSize  = uniform (0.0, 1.0) / std::max (1e-9, est[i] / maxEst);
                score[i] = w[0] * alphaRank[i] + w[1] * sizeRank[i] + w[2] * randAlpha + w[3] * randSize;
            }
            std::stable_sort (idx.begin(), idx.end(), [&] (int a, int b) { return score[(size_t) a] < score[(size_t) b]; });
            for (auto i : idx) out.add (files[i]);
            return out;
        }

        static bool matchesTag (const juce::String& normName, const juce::String& canonDetected, const juce::String& entry)
        {
            if (entry == canonDetected) return true;
            const auto& lex = Lexicon::get();
            if (lex.allCanonical().contains (entry)) return false; // known instrument: rely on detection
            return Lexicon::containsWord (normName, Lexicon::normalise (entry));
        }

        std::vector<Source> orderSources (const juce::Array<juce::File>& files)
        {
            const auto& lex = Lexicon::get();
            std::vector<Source> out;

            juce::StringArray seq;
            const bool primary = s.sortScope != SortScope::SecondaryOnly;
            if (primary)
            {
                if (s.primaryMode == PrimaryMode::FamilyPriority)
                {
                    seq = lex.familyPrioritySequence();
                    log ("Music-type priority: melodic/harmonic -> drums -> perc/808/rims.");
                }
                else
                {
                    for (auto& e : s.instrumentSequence)
                        if (e.trim().isNotEmpty()) seq.addIfNotAlreadyThere (lex.canonicalise (e));
                    if (! seq.isEmpty()) log ("Instrument group order: " + seq.joinIntoString (", "));
                }
            }

            if (seq.isEmpty())
            {
                for (auto& f : orderFiles (files))
                    out.push_back ({ f, lex.detect (f.getFileName()), lex.detect (f.getFileName()) });
                return out;
            }

            std::vector<juce::Array<juce::File>> buckets ((size_t) seq.size() + 1);
            for (auto& f : files)
            {
                const auto norm = Lexicon::normalise (f.getFileNameWithoutExtension());
                const auto canon = lex.detect (f.getFileName());
                int slot = seq.size();
                for (int k = 0; k < seq.size(); ++k)
                    if (matchesTag (norm, canon, seq[k])) { slot = k; break; }
                buckets[(size_t) slot].add (f);
            }

            for (size_t k = 0; k < buckets.size(); ++k)
            {
                auto group = buckets[k];
                if (group.isEmpty()) continue;
                if (s.sortScope == SortScope::Both)
                    group = orderFiles (group);
                else
                    std::stable_sort (group.begin(), group.end(), [] (auto& a, auto& b) { return alphaKey (a) < alphaKey (b); });

                for (auto& f : group)
                    out.push_back ({ f, k < (size_t) seq.size() ? seq[(int) k] : lex.detect (f.getFileName()), lex.detect (f.getFileName()) });
            }
            return out;
        }

        void presenceReport (const juce::Array<juce::File>& files)
        {
            std::map<juce::String, int> counts;
            for (auto& f : files)
            {
                auto c = Lexicon::get().detect (f.getFileName());
                if (c.isNotEmpty()) ++counts[c];
            }
            if (counts.empty()) return;
            juce::StringArray lines;
            for (auto& [k, v] : counts) lines.add (k + ": " + juce::String (v));
            log ("Instrument hits (file names): " + lines.joinIntoString (", "));
        }

        // ---- loading ---------------------------------------------------------
        bool isPercussiveOneShot (const Source& src)
        {
            static const char* words[] = { "kick", "snare", "rim", "tom", "808", "909", "drum", "cymbal", "hihat", "hh", "hat",
                                           "perc", "shaker", "timbale", "conga", "bongo", "cowbell", "clap" };
            const auto norm = Lexicon::normalise (src.file.getFileNameWithoutExtension());
            for (auto* w : words)
                if (Lexicon::containsWord (norm, " " + juce::String (w) + " ") || Lexicon::containsWord (norm, " " + juce::String (w) + "s "))
                    return true;
            if (src.canon.isNotEmpty() && Lexicon::get().familyOf (src.canon) != Family::Melodic)
                return true;
            if (auto info = AudioLoader::probe (src.file))
                return info->seconds() <= s.excludeMaxSeconds;
            return false;
        }

        double instrumentWeight (const Source& src) const
        {
            auto find = [&] (const juce::String& k) -> double
            {
                auto it = s.instrumentWeights.find (k);
                return it != s.instrumentWeights.end() ? it->second : 0.0;
            };
            return std::max (find (src.tag), find (src.canon));
        }

        void loadSegments (const std::vector<Source>& sources, RenderResult& res)
        {
            const auto minLen = std::max<size_t> (ms (s.segMinMs), 64);
            const auto maxLen = std::max (minLen, ms (s.segMaxMs));
            const int n = (int) sources.size();

            for (int rank = 0; rank < n; ++rank)
            {
                if (cancelled()) return;
                progress (0.6 * rank / n);
                const auto& src = sources[(size_t) rank];
                const auto name = src.file.getFileName();

                if (s.excludePercussive && isPercussiveOneShot (src))
                {
                    log ("  - skipped percussive/one-shot: " + name);
                    continue;
                }

                juce::String err;
                auto clip = AudioLoader::load (src.file, sr, err);
                if (! clip) { log ("  ! skipping " + name + ": " + err); continue; }

                trimSilence (*clip, sr, s.silenceTrimDb, s.minSilenceMs);
                gate (*clip, sr, s.gateDb, s.gateReduction);
                removeInternalSilence (*clip, sr, s.silenceTrimDb, s.removeInternalSilenceMs);
                if (s.centerCut) centerCut (*clip, sr, s.centerAtten, s.centerBassHz, s.centerBassReturn);

                if (clip->size() < ms (250))
                {
                    log ("  - too short after trimming: " + name);
                    continue;
                }

                const auto family = src.canon.isNotEmpty() ? Lexicon::get().familyOf (src.canon) : Family::Melodic;
                const double instW = instrumentWeight (src);
                const double rankNorm = n > 1 ? (double) rank / (n - 1) : 0.0;
                // Instrument % gives up to 3x presence; earlier files in the chosen order get up to 3x more pick weight.
                const double pickWeight = (1.0 + 2.0 * instW) * (1.5 - rankNorm);

                const size_t avg = (minLen + maxLen) / 2;
                const auto aim = (int) std::clamp<double> (std::round (std::max<double> (1.0, (double) clip->size() / (double) avg) * (1.0 + 2.0 * instW)), 1.0, 96.0);

                for (int k = 0; k < aim; ++k)
                {
                    const auto hi = std::min (maxLen, clip->size());
                    const auto lo = std::min (minLen, hi);
                    const auto len = (size_t) randInt ((juce::int64) lo, (juce::int64) hi);
                    const auto start = (size_t) randInt (0, (juce::int64) (clip->size() - len));
                    Segment seg;
                    seg.audio = clip->slice (start, len);
                    edgeFade (seg.audio, ms (s.edgeFadeMs));
                    applyGain (seg.audio, s.mainSegGain);
                    seg.fileRank = rank;
                    seg.tag = src.tag;
                    seg.family = family;
                    seg.weight = pickWeight;
                    const size_t idx = segments.size();
                    segments.push_back (std::move (seg));
                    (family == Family::Drums ? drums : family == Family::Perc ? perc : melodic).push_back (idx);
                }

                res.filesUsed.add (src.file);
                log ("  + " + name + (src.tag.isNotEmpty() ? "  [" + src.tag + "]" : juce::String()) + "  -> " + juce::String (aim) + " segment(s)");
            }

            log ("Segments: " + juce::String ((int) melodic.size()) + " melodic, " + juce::String ((int) drums.size())
                 + " drums, " + juce::String ((int) perc.size()) + " perc.");
        }

        // ---- motifs ----------------------------------------------------------
        void buildMotifs()
        {
            const auto& pool = ! melodic.empty() ? melodic : ! drums.empty() ? drums : perc;
            const int count = std::max (1, s.subLoops);
            const auto subLen = (size_t) std::llround (s.longBlockSec / count * sr);

            std::vector<size_t> chosen;
            for (int m = 0; m < count; ++m)
            {
                size_t pick = pickWeighted (pool);
                for (int tries = 0; tries < 12 && std::find (chosen.begin(), chosen.end(), pick) != chosen.end(); ++tries)
                    pick = pickWeighted (pool);
                chosen.push_back (pick);

                Clip motif = loopTile (segments[pick].audio, subLen, crossfade(), microFade());
                edgeFade (motif, ms (s.edgeFadeMs));
                motifs.push_back (std::move (motif));
            }
        }

        Clip longBlock (int rotation)
        {
            Clip block;
            const int count = (int) motifs.size();
            for (int i = 0; i < count; ++i)
                appendCrossfade (block, motifs[(size_t) ((rotation + i) % count)], crossfade(), zcSearch(), microFade());
            return block;
        }

        /** 27 s rhythmic variant: short slices from the inputs, reversed now
            and then, optionally layered with a drum/perc slice. */
        Clip variantBlock (size_t len, juce::int64 absStart)
        {
            std::vector<const std::vector<size_t>*> pools;
            for (auto* p : { &drums, &perc, &melodic })
                if (! p->empty()) pools.push_back (p);

            Clip cur;
            cur.reserve (len + ms (1500));
            const auto cf = std::min (crossfade(), ms (100));

            while (cur.size() < len)
            {
                if (cancelled()) break;
                const auto& pool = *pools[(size_t) randInt (0, (juce::int64) pools.size() - 1)];
                const auto& srcSeg = segments[pickWeighted (pool)].audio;

                const auto want = (size_t) std::llround (uniform (0.4, 1.2) * sr);
                Clip frag = srcSeg.size() <= want ? srcSeg
                                                  : srcSeg.slice ((size_t) randInt (0, (juce::int64) (srcSeg.size() - want)), want);
                if (chance (s.reverseProb)) reverseClip (frag);

                if ((! drums.empty() || ! perc.empty()) && chance (s.layerDrumsProb))
                {
                    const auto& lp = (! drums.empty() && (perc.empty() || chance (0.7))) ? drums : perc;
                    Clip lay = loopTile (segments[pickWeighted (lp)].audio, frag.size(), ms (10), microFade());
                    edgeFade (lay, ms (s.edgeFadeMs));
                    for (size_t i = 0; i < frag.size(); ++i)
                    {
                        frag.L[i] += s.layerDrumGain * lay.L[i];
                        frag.R[i] += s.layerDrumGain * lay.R[i];
                    }
                }

                edgeFade (frag, ms (s.edgeFadeMs));
                appendCrossfade (cur, frag, cf, zcSearch(), microFade());

                if (chance (s.stutterProb) && stutterAllowed (absStart + (juce::int64) cur.size()))
                {
                    stutter (cur);
                    lastStutterAt = absStart + (juce::int64) cur.size();
                }
            }
            cur.resize (len);
            edgeFade (cur, ms (s.edgeFadeMs));
            return cur;
        }

        // ---- engines ---------------------------------------------------------
        Clip buildLoop (size_t target)
        {
            buildMotifs();
            const auto shortLen = (size_t) std::llround (s.shortBlockSec * sr);
            Clip out;
            out.reserve (target + (size_t) (s.longBlockSec * sr) + shortLen);
            int rotation = 0;

            while (out.size() < target && ! cancelled())
            {
                Clip block = longBlock (rotation);
                const auto blockEnd = (juce::int64) (out.size() + block.size());
                if (chance (s.stutterProb) && stutterAllowed (blockEnd))
                {
                    stutter (block);
                    lastStutterAt = blockEnd;
                }
                appendCrossfade (out, block, crossfade(), zcSearch(), microFade());
                progress (0.6 + 0.35 * std::min (1.0, (double) out.size() / (double) target));
                if (out.size() >= target) break;

                if (shortLen > 0)
                    appendCrossfade (out, variantBlock (shortLen, (juce::int64) out.size()), crossfade(), zcSearch(), microFade());

                rotation = (rotation + 1) % std::max (1, (int) motifs.size());
                progress (0.6 + 0.35 * std::min (1.0, (double) out.size() / (double) target));
            }
            out.resize (std::min (out.size(), target));
            return out;
        }

        Clip buildMontage (size_t target)
        {
            Clip out;
            out.reserve (target + ms (s.segMaxMs) * 2);

            // Segments are already in file order (sorting decides the running order here).
            while (out.size() < target && ! cancelled())
            {
                for (auto& seg : segments)
                {
                    Clip c = seg.audio;
                    if (chance (s.reverseProb)) reverseClip (c);
                    const auto at = (juce::int64) (out.size() + c.size());
                    if (chance (s.stutterProb) && stutterAllowed (at))
                    {
                        stutter (c);
                        lastStutterAt = at;
                    }
                    appendCrossfade (out, c, crossfade(), zcSearch(), microFade());
                    progress (0.6 + 0.3 * std::min (1.0, (double) out.size() / (double) target));
                    if (out.size() >= target || cancelled()) break;
                }
            }
            out.resize (std::min (out.size(), target));

            if (s.montageStructure && ! cancelled())
            {
                buildMotifs();
                const auto longLen = (size_t) std::llround (s.longBlockSec * sr);
                const auto shortLen = (size_t) std::llround (s.shortBlockSec * sr);
                const auto subLen = motifs.empty() ? 0 : motifs[0].size();
                int rotation = 0;
                for (size_t t = 0; t < out.size() && longLen > 0;)
                {
                    for (size_t k = 0; k < motifs.size(); ++k)
                        overwriteCrossfade (out, motifs[(k + (size_t) rotation) % motifs.size()], t + k * subLen, crossfade());
                    rotation = (rotation + 1) % (int) std::max<size_t> (1, motifs.size());
                    t += longLen + shortLen;
                }
            }
            return out;
        }

        /** Timed stutter (every ~33 s) and bit-crush glitch (every ~67 s)
            overlays, pulled from the input pool, never closer than 5 s. */
        void applyPeriodicFx (Clip& out)
        {
            if (segments.empty() || out.empty()) return;
            struct Event { double t; bool glitch; };
            std::vector<Event> events;
            const double total = (double) out.size() / sr;
            if (s.periodicStutterSec > 0) for (double t = s.periodicStutterSec; t < total; t += s.periodicStutterSec) events.push_back ({ t, false });
            if (s.periodicGlitchSec  > 0) for (double t = s.periodicGlitchSec;  t < total; t += s.periodicGlitchSec)  events.push_back ({ t, true });
            std::sort (events.begin(), events.end(), [] (auto& a, auto& b) { return a.t < b.t; });

            std::vector<size_t> all (segments.size());
            std::iota (all.begin(), all.end(), 0);
            const auto cf = ms (40);
            double last = -1.0e9;
            int applied = 0;

            for (auto& e : events)
            {
                if (e.t - last < 5.0) continue;
                const auto& src = segments[pickWeighted (all)].audio;
                Clip fx;
                if (e.glitch)
                {
                    fx = loopTile (src, ms (s.glitchOverlayMs), ms (5), microFade());
                    sampleHold (fx, s.glitchCrush);
                    if (chance (0.5)) reverseClip (fx);
                }
                else
                {
                    Clip chunk = loopTile (src, ms (s.stutterOverlayMs), ms (5), microFade());
                    fx = chunk;
                    for (int r = 0; r < s.stutterRepeatsMin; ++r)
                        appendCrossfade (fx, chunk, microFade(), 0, microFade());
                }
                overwriteCrossfade (out, fx, (size_t) (e.t * sr), cf);
                last = e.t;
                ++applied;
            }
            log ("Periodic FX overlays: " + juce::String (applied));
        }

        // ---- master ----------------------------------------------------------
        void master (Clip& c)
        {
            if (c.empty()) return;
            removeDcMean (c);
            if (s.dcBlock) dcBlock (c, sr);
            normalise (c, s.targetPeak);
            if (s.declick)
            {
                const float spike = 0.85f * s.targetPeak / 0.88f;
                declick (c.L, spike, 0.6f, 8);
                declick (c.R, spike, 0.6f, 8);
            }
            if (s.softClip)
            {
                softClip (c);
                normalise (c, s.targetPeak);
            }
            edgeFade (c, ms (10));
            for (auto* ch : { &c.L, &c.R })
                for (auto& v : *ch)
                    if (! std::isfinite (v)) v = 0.0f;
        }
    };
}

//==============================================================================
juce::Array<juce::File> RemixEngine::gatherInputs (const Settings& s, const Callbacks&)
{
    auto isOurOutput = [&] (const juce::File& f)
    {
        const auto name = f.getFileName();
        return name == s.outputName
            || name.startsWith (s.partsBaseName + "_part")
            || name.startsWith (".");
    };

    juce::Array<juce::File> files;
    auto addFile = [&] (const juce::File& f)
    {
        if (AudioLoader::isSupportedExtension (f) && ! isOurOutput (f))
            files.addIfNotAlreadyThere (f);
    };

    for (auto& path : s.inputs)
    {
        const juce::File p (path);
        if (p.isDirectory())
        {
            juce::Array<juce::File> found;
            for (const auto& entry : juce::RangedDirectoryIterator (p, true, "*", juce::File::findFiles))
                found.add (entry.getFile());
            found.sort();
            for (auto& f : found) addFile (f);
        }
        else if (p.existsAsFile())
        {
            addFile (p);
        }
    }
    return files;
}

RenderResult RemixEngine::render (const Settings& s, const Callbacks& cb)
{
    Renderer r (s, cb);
    return r.run();
}

juce::Array<juce::File> RemixEngine::writeOutputs (const Settings& s, const RenderResult& r,
                                                   const Callbacks& cb, juce::String& error)
{
    juce::Array<juce::File> written;
    if (! r.ok || r.audio.empty()) { error = "Nothing to write."; return written; }

    if (! s.outputDir.isDirectory() && ! s.outputDir.createDirectory())
    {
        error = "Cannot create output folder " + s.outputDir.getFullPathName();
        return written;
    }

    if (s.writeConsolidated)
    {
        auto f = s.outputDir.getChildFile (s.outputName.endsWithIgnoreCase (".wav") ? s.outputName : s.outputName + ".wav");
        if (! WavWriter::write (f, r.audio, 0, r.audio.size(), r.sampleRate, s.format, error))
            return written;
        written.add (f);
        if (cb.log) cb.log ("Wrote " + f.getFullPathName() + "  (" + juce::File::descriptionOfSizeInBytes (f.getSize()) + ")");
    }

    if (s.writeParts)
    {
        auto parts = WavWriter::writeParts (s.outputDir, s.partsBaseName, r.audio, r.sampleRate, s.format, s.partBytes, error);
        for (auto& f : parts)
            if (cb.log) cb.log ("Wrote " + f.getFullPathName() + "  (" + juce::File::descriptionOfSizeInBytes (f.getSize()) + ")");
        written.addArray (parts);
    }
    return written;
}

double RemixEngine::parseDuration (const juce::String& text)
{
    auto parts = juce::StringArray::fromTokens (text.trim(), ":", "");
    parts.removeEmptyStrings();
    if (parts.isEmpty() || parts.size() > 3) return -1.0;
    double total = 0.0;
    for (auto& p : parts)
    {
        if (! p.trim().containsOnly ("0123456789.")) return -1.0;
        total = total * 60.0 + p.trim().getDoubleValue();
    }
    return total;
}

} // namespace remix
