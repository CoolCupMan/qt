#pragma once

#include <juce_core/juce_core.h>
#include <vector>

namespace remix
{

enum class Family { Melodic, Drums, Perc };

/** Instrument lexicon from the Python scripts, with safer matching:
    aliases match whole words only (so "clap" is no longer a clarinet via
    "cl"), letter/digit runs are split ("kick01" -> "kick 01", "tr808" ->
    "tr 808") and the longest alias wins ("synth pad" beats "pad"). */
class Lexicon
{
public:
    static const Lexicon& get();

    /** Canonical instrument detected in a file name, or empty. */
    juce::String detect (const juce::String& fileName) const;

    /** Maps user text (alias or canonical) to a canonical name; unknown text
        is returned lower-cased so custom tags still group by file name. */
    juce::String canonicalise (const juce::String& userText) const;

    Family familyOf (const juce::String& canon) const;

    /** melodic -> drums -> perc, alphabetical inside each family. */
    juce::StringArray familyPrioritySequence() const;

    juce::StringArray allCanonical() const;

    /** Lower-case, non-alphanumerics to spaces, letter/digit boundaries split,
        padded with single spaces so " word " searches are boundary-safe. */
    static juce::String normalise (const juce::String& s);

    static bool containsWord (const juce::String& normalisedHaystack, const juce::String& normalisedNeedle);

private:
    Lexicon();

    struct Entry { juce::String canon; std::vector<juce::String> aliases; Family family; };
    std::vector<Entry> entries;
    std::vector<std::pair<juce::String, juce::String>> aliasToCanon; // normalised alias -> canon, longest first
};

} // namespace remix
