# WAV Remixer — VST3 plugin + standalone app

One C++/JUCE codebase, two outputs:

| Output | File (Windows) | Use it for |
|---|---|---|
| **VST3 plugin** | `WAV Remixer.vst3` | Any VST3 host (Ableton, FL Studio, Reaper, Cubase, Bitwig, Studio One…) |
| **Standalone app** | `WAV Remixer.exe` | Runs on its own, no DAW needed |

macOS (`.vst3` + `.app`) and Linux builds come out of the same project. An optional
command-line renderer, `RemixCli`, is included as well.

The original Python scripts it replaces are kept in [`reference/python/`](reference/python).

## What it does

You drop in WAV files or folders. It cuts them into segments and builds a new track
**only from your own material** (nothing is synthesised), then writes `output.wav`
and plays the result in the plugin/app.

* **Loop 63/27 engine (default):** every 63 s block plays three recurring 21 s motifs in
  rotating order (A B C → B C A → C A B …); every 27 s block in between is a fresh rhythmic
  variant made of short slices, with reverses and drum layering. Stutters are allowed, but
  never closer than the cooldown (30 s).
* **Montage engine:** segments run in the order you sorted the files, optionally with the
  63/27 motif grid laid over the top.
* **Sorting:** instrument groups (custom sequence or melodic → drums → perc), then
  alphabetical / largest first / random / random-by-size / MIX A-B-C-D, plus an
  "emphasis %" per instrument.
* **Cleanup:** silence trim, optional smoothed noise gate, removal of long pauses, vocal
  reduction (centre cut), skipping drum one-shots.
* **Master:** DC removal + DC blocker, peak normalisation (0.88 by default), de-click
  repair, optional soft clip.
* **Seed:** every render logs its seed. Enter that seed to get the bit-identical render again.
  With a fixed seed the plugin rebuilds the audio automatically when a project is reopened.

## Taking the best of the five scripts

| Kept from | What |
|---|---|
| all versions | 63/27 structure with 21/21/21 rotation, instrument lexicon, sorting and MIX modes, per-instrument % |
| `v2` (equal-power) | equal-power crossfades, zero-crossing aligned joins, micro edge fades |
| `v3` (RF64) | RF64 for files over 4 GB, WAVE_FORMAT_EXTENSIBLE, 32-bit PCM, 64-bit float, µ-law, A-law, multichannel downmix, DC block + de-click pass |
| `v4` (centre cut) | vocal centre cut, skipping drum one-shots, periodic stutter/glitch FX, montage mode, DC-offset fix |
| `v5` (PyQt GUI) | GUI with drag & drop, file/folder pickers, live log, all settings in one window |

### Bugs from the Python versions that are fixed here

* Every crossfade re-applied the edge fade to the **start** of the whole track as well, so the first milliseconds were faded again on every join.
* Short segments were looped by butting copies together, which clicked on every repeat. Loops are now crossfaded.
* The per-instrument % did nothing in the loop engine (`allsegs[:keep]` cannot keep more segments than exist).
* The 63/27 overlay in `v4` *inserted* audio and so pushed the rest of the track later. It now replaces audio and keeps the length.
* "Remove internal silence" dropped every quiet sample (including note decays), not just long pauses.
* Name matching was by substring, so `clap` became a clarinet (`cl`) and any name containing `hat` (e.g. `that`) counted as a drum one-shot. It now matches whole words and prefers the longest alias.
* The µ-law and A-law decoders were slightly off (missing µ-law bias, inverted A-law polarity).
* `v1`/`v2` skipped the consolidated file above 4 GB, and `v3` wrote *every* file as RF64, which many players can't open. Now a normal WAV is written whenever it fits, and RF64 only when it doesn't.

Rendering is native code, so it is far faster than the pure-Python loops: a 4-minute
remix took about 1.5 s on a 4-core test machine, and a 95-minute 96 kHz render about 1 minute.

## Getting the .exe and .vst3

**From GitHub Actions (no setup):** every push runs `.github/workflows/build.yml`. Open the
run on the *Actions* tab and download `WAV-Remixer-VST3-Windows` and `WAV-Remixer-App-Windows`.
If you push a tag like `v1.0.0`, the zips are also attached to a GitHub Release.

**Build it yourself (Windows):** install Visual Studio 2022 (C++ workload) and CMake, then:

```bat
cmake -S . -B build
cmake --build build --config Release
```

JUCE is downloaded automatically. To use a local checkout, add `-DJUCE_DIR=C:\path\to\JUCE`.
To also build the command-line tool, add `-DREMIX_BUILD_CLI=ON`.

Outputs:

```
build\WavRemixer_artefacts\Release\VST3\WAV Remixer.vst3      <- copy to C:\Program Files\Common Files\VST3
build\WavRemixer_artefacts\Release\Standalone\WAV Remixer.exe
```

## Using it

1. Drag WAV files or folders onto the window (or use *Add files… / Add folder…*).
2. Set the length (type `5:00`, `300` or `1:10:00` into the Length box) and adjust the tabs.
3. Press **Render**. With *Write file after render* on, `output.wav` goes to the output folder
   (by default, the folder of the first input).
4. Press **Play**, or in a DAW just start the transport: with *Follow host transport* on, the
   remix plays in sync with the timeline. Click the waveform to seek.
5. **Export…** writes the current render to any folder in the chosen format
   (16-bit dithered / 24-bit / 32-bit float, 44.1–96 kHz, optional split parts).

In a DAW it's an effect with a stereo input and output: put it on an audio track (or an empty
one). *Pass input through* mixes the track's own audio underneath.

### Command line

```
RemixCli --in "D:\Samples" --in extra.wav --out D:\Renders --len 6:30 --seed 1234
         [--mode loop|montage] [--order alpha|size|random|randsize]
         [--sequence "synth pad, electric piano, drum kit"] [--family]
         [--fx] [--center-cut] [--exclude-perc] [--soft-clip] [--parts]
         [--format 16|24|32f] [--rate 48000] [--name mix.wav]
```

## Supported input

WAV (PCM 8/16/24/32-bit, float 32/64-bit, WAVE_FORMAT_EXTENSIBLE, RF64, µ-law, A-law, mono to 7.1),
plus AIFF, FLAC, Ogg Vorbis and MP3. Everything is converted to stereo at the render sample rate
with a band-limited (windowed-sinc) resampler.

## Notes

* The output is **VST3**. VST2 cannot be built any more, because Steinberg no longer licenses the VST2 SDK.
* The plugin keeps the input list and settings in the project. The rendered audio is not
  stored in the project: with a fixed seed it is rebuilt automatically on load, otherwise press Render again.
