// Headless renderer: same engine as the plugin, driven from the command line.
//
//   RemixCli --in <file|folder> [--in ...] --out <folder> [--len 5:00]
//            [--mode loop|montage] [--seed N] [--format 16|24|32f] [--rate 44100]
//            [--order alpha|size|random|randsize] [--family|--sequence "kick, pad, ..."]
//            [--fx] [--center-cut] [--exclude-perc] [--parts] [--soft-clip]

#include <juce_core/juce_core.h>
#include "Engine/RemixEngine.h"
#include <iostream>

using namespace remix;

int main (int argc, char* argv[])
{
    juce::ArgumentList args (argc, argv);
    Settings s;

    for (int i = 0; i < args.size(); ++i)
    {
        const auto a = args[i].text;
        auto next = [&] { return i + 1 < args.size() ? args[++i].text : juce::String(); };

        if      (a == "--in")           s.inputs.add (juce::File::getCurrentWorkingDirectory().getChildFile (next()).getFullPathName());
        else if (a == "--out")          s.outputDir = juce::File::getCurrentWorkingDirectory().getChildFile (next());
        else if (a == "--name")         s.outputName = next();
        else if (a == "--len")          s.targetSeconds = RemixEngine::parseDuration (next());
        else if (a == "--mode")         s.engineMode = next() == "montage" ? EngineMode::Montage : EngineMode::Loop63_27;
        else if (a == "--seed")         s.seed = (juce::uint32) next().getLargeIntValue();
        else if (a == "--rate")         s.sampleRate = next().getDoubleValue();
        else if (a == "--fx")           s.periodicFx = true;
        else if (a == "--center-cut")   s.centerCut = true;
        else if (a == "--exclude-perc") s.excludePercussive = true;
        else if (a == "--parts")        s.writeParts = true;
        else if (a == "--soft-clip")    s.softClip = true;
        else if (a == "--family")       { s.sortScope = SortScope::Both; s.primaryMode = PrimaryMode::FamilyPriority; }
        else if (a == "--sequence")     { s.primaryMode = PrimaryMode::CustomSequence; s.instrumentSequence = juce::StringArray::fromTokens (next(), ",", ""); }
        else if (a == "--format")
        {
            const auto f = next();
            s.format = f == "24" ? OutputFormat::Pcm24 : f.startsWith ("32") ? OutputFormat::Float32 : OutputFormat::Pcm16;
        }
        else if (a == "--order")
        {
            const auto o = next();
            s.orderMode = o == "size" ? OrderMode::SizeDesc : o == "random" ? OrderMode::Random
                        : o == "randsize" ? OrderMode::RandomBySize : OrderMode::Alphabetical;
        }
        else
        {
            std::cerr << "Unknown argument: " << a << "\n";
            return 2;
        }
    }

    if (s.inputs.isEmpty() || s.targetSeconds <= 0)
    {
        std::cerr << "usage: RemixCli --in <file|folder> [--in ...] --out <folder> --len <seconds|mm:ss|hh:mm:ss> [options]\n";
        return 2;
    }
    if (s.outputDir == juce::File())
        s.outputDir = juce::File (s.inputs[0]).isDirectory() ? juce::File (s.inputs[0]) : juce::File (s.inputs[0]).getParentDirectory();

    Callbacks cb;
    cb.log = [] (const juce::String& m) { std::cout << m << std::endl; };

    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    auto result = RemixEngine::render (s, cb);
    if (! result.ok)
    {
        std::cerr << "Error: " << result.error << "\n";
        return 1;
    }

    juce::String err;
    auto written = RemixEngine::writeOutputs (s, result, cb, err);
    if (written.isEmpty())
    {
        std::cerr << "Error: " << err << "\n";
        return 1;
    }
    std::cout << "Done in " << juce::String ((juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0, 1) << " s" << std::endl;
    return 0;
}
