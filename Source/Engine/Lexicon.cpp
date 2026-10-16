#include "Lexicon.h"
#include <algorithm>
#include <map>

namespace remix
{

const Lexicon& Lexicon::get()
{
    static const Lexicon instance;
    return instance;
}

Lexicon::Lexicon()
{
    constexpr auto M = Family::Melodic, D = Family::Drums, P = Family::Perc;

    entries = {
        { "grand piano",           { "grand piano", "acoustic piano", "apiano", "steinway" }, M },
        { "electric piano",        { "electric piano", "e-piano", "rhodes", "wurli", "mk1", "mk2" }, M },
        { "real honky tonk piano", { "real honky tonk piano", "honky tonk", "honky-tonk" }, M },
        { "upright piano",         { "upright piano" }, M },
        { "octave piano",          { "octave piano" }, M },
        { "clavinet",              { "clavinet", "clav" }, M },
        { "harpsichord",           { "harpsichord", "cembalo" }, M },
        { "celesta",               { "celesta" }, M },
        { "concert strings",       { "concert strings", "ensemble strings", "string ensemble", "strings ens", "real strings" }, M },
        { "real strings",          { "real strings", "orch strings", "orchestral strings" }, M },
        { "sweet violin",          { "sweet violin", "violin", "vln" }, M },
        { "viola",                 { "viola", "vla" }, M },
        { "cello",                 { "cello", "vc" }, M },
        { "contrabass",            { "contrabass", "double bass", "upright bass" }, M },
        { "flute",                 { "flute", "concert flute", "pan flute", "shakuhachi" }, M },
        { "piccolo",               { "piccolo" }, M },
        { "clarinet",              { "clarinet", "cl" }, M },
        { "oboe",                  { "oboe" }, M },
        { "english horn",          { "english horn", "cor anglais" }, M },
        { "bassoon",               { "bassoon", "bn" }, M },
        { "sweet tenor sax",       { "sweet tenor sax", "tenor sax", "tenor saxophone", "tsax" }, M },
        { "sweet alto sax",        { "sweet alto sax", "alto sax", "alto saxophone", "asax" }, M },
        { "sweet soprano sax",     { "sweet soprano sax", "soprano sax", "ssax" }, M },
        { "baritone sax",          { "baritone sax", "bari sax", "bsax" }, M },
        { "trumpet",               { "trumpet", "tpt" }, M },
        { "flugelhorn",            { "flugelhorn" }, M },
        { "trombone",              { "trombone", "tbn" }, M },
        { "french horn",           { "french horn", "horn", "hn" }, M },
        { "tuba",                  { "tuba" }, M },
        { "acoustic guitar",       { "acoustic guitar", "steel guitar", "nylon guitar", "classical guitar" }, M },
        { "electric guitar",       { "electric guitar", "eguitar", "strat", "tele", "les paul", "lp" }, M },
        { "jazz guitar",           { "jazz guitar" }, M },
        { "muted guitar",          { "muted guitar", "palm mute guitar" }, M },
        { "electric bass",         { "electric bass", "e-bass", "bass guitar", "precision", "j-bass", "fretless bass" }, M },
        { "synth bass",            { "synth bass", "sub bass" }, M },
        { "upright bass",          { "upright bass", "double bass" }, M },
        { "synth lead",            { "synth lead", "lead synth", "ld" }, M },
        { "synth pad",             { "synth pad", "pad", "warm pad", "string pad", "air pad" }, M },
        { "synth pluck",           { "synth pluck", "pluck" }, M },
        { "synth brass",           { "synth brass" }, M },
        { "synth keys",            { "synth keys", "poly synth", "polysynth" }, M },
        { "fm piano",              { "fm piano", "dx7 piano" }, M },
        { "supersaw",              { "supersaw" }, M },
        { "square lead",           { "square lead" }, M },
        { "saw lead",              { "saw lead" }, M },
        { "drum kit",              { "drum kit", "acoustic kit", "studio kit", "drum", "drums", "drum loop", "breakbeat" }, D },
        { "electronic kit",        { "electronic kit", "edm kit" }, D },
        { "909 kit",               { "909", "tr-909" }, D },
        { "808 kit",               { "808", "tr-808", "808s" }, P },
        { "snare",                 { "snare" }, D },
        { "kick",                  { "kick", "bd" }, D },
        { "hihat",                 { "hihat", "hi-hat", "hh" }, D },
        { "tom",                   { "tom" }, D },
        { "cymbal",                { "cymbal", "ride", "crash", "splash", "china" }, D },
        { "timpani",               { "timpani", "kettle drum" }, D },
        { "percussion",            { "percussion", "perc", "clap", "shaker", "tambourine", "cowbell", "guiro", "conga", "bongo", "timbale" }, P },
        { "rim",                   { "rim", "rimshot", "rim shot" }, P },
        { "bell",                  { "bell", "bell synth", "glockenspiel", "tubular bells" }, M },
        { "synth",                 { "synth", "synths" }, M },
    };

    // Later entries win for duplicate aliases (same as the Python dict build).
    std::map<juce::String, juce::String> map;
    for (auto& e : entries)
    {
        map[normalise (e.canon)] = e.canon;
        for (auto& a : e.aliases)
            map[normalise (a)] = e.canon;
    }
    // Make sure a canonical name always maps to itself.
    for (auto& e : entries)
        map[normalise (e.canon)] = e.canon;

    for (auto& kv : map)
        aliasToCanon.emplace_back (kv.first, kv.second);

    std::stable_sort (aliasToCanon.begin(), aliasToCanon.end(),
                      [] (auto& a, auto& b) { return a.first.length() > b.first.length(); });
}

juce::String Lexicon::normalise (const juce::String& s)
{
    juce::String out;
    out.preallocateBytes ((size_t) s.length() * 2 + 4);
    out << ' ';

    int prevKind = 0; // 0 = space, 1 = letter, 2 = digit
    for (auto c : s.toLowerCase())
    {
        int kind = juce::CharacterFunctions::isDigit (c) ? 2
                 : juce::CharacterFunctions::isLetter (c) ? 1 : 0;

        if (kind == 0)
        {
            if (prevKind != 0) out << ' ';
        }
        else
        {
            if (prevKind != 0 && prevKind != kind) out << ' ';
            out << juce::String::charToString (c);
        }
        prevKind = kind;
    }
    if (prevKind != 0) out << ' ';
    return out;
}

bool Lexicon::containsWord (const juce::String& hay, const juce::String& needle)
{
    // Both strings are already " padded " by normalise().
    return needle.trim().isNotEmpty() && hay.contains (needle);
}

juce::String Lexicon::detect (const juce::String& fileName) const
{
    auto norm = normalise (juce::File::createFileWithoutCheckingPath (fileName).getFileNameWithoutExtension());
    for (auto& [alias, canon] : aliasToCanon)
        if (containsWord (norm, alias))
            return canon;
    return {};
}

juce::String Lexicon::canonicalise (const juce::String& userText) const
{
    auto norm = normalise (userText);
    for (auto& [alias, canon] : aliasToCanon)
        if (alias == norm)
            return canon;
    return userText.trim().toLowerCase();
}

Family Lexicon::familyOf (const juce::String& canon) const
{
    for (auto& e : entries)
        if (e.canon == canon)
            return e.family;
    return Family::Melodic;
}

juce::StringArray Lexicon::familyPrioritySequence() const
{
    juce::StringArray seq;
    for (auto fam : { Family::Melodic, Family::Drums, Family::Perc })
    {
        juce::StringArray part;
        for (auto& e : entries)
            if (e.family == fam)
                part.add (e.canon);
        part.sort (false);
        seq.addArray (part);
    }
    return seq;
}

juce::StringArray Lexicon::allCanonical() const
{
    juce::StringArray all;
    for (auto& e : entries)
        all.add (e.canon);
    return all;
}

} // namespace remix
