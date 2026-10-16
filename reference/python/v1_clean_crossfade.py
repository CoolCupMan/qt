#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import os, sys, wave, struct, io, traceback, random, re, math

# --- stdout flush for Pydroid ---
try:
    sys.stdout.reconfigure(write_through=True, line_buffering=True)
except Exception:
    pass

# ====== Config ======

REMIX_OUTPUT_BASENAME = "remix_output"  # remix_output_part01.wav, etc.
SUPPORTED_EXTS = (".wav",)

# Silence/Noise handling for source-cleanup
# -> Disable gating & internal silence removal so we never cut real content.
SILENCE_TRIM_DB = -80          # keep only super-silent lead/trail trim; won't touch quiet material
MIN_SILENCE_MS = 30
REMOVE_INTERNAL_SILENCE_MS = 0 # do NOT remove internal "silence" blocks
NOISE_GATE_DB = -120           # effectively off
NOISE_GATE_REDUCTION = 1.0     # no attenuation (gate disabled)

# Target format
TARGET_SR = 44100
TARGET_CHANNELS = 2
TARGET_SAMPWIDTH = 2          # 16-bit PCM

# Remix engine settings (pure-Python only; no external modules)
SEG_MIN_MS = 800
SEG_MAX_MS = 4000
SEG_REVERSE_PROB = 0.15

# Glitch/stutter control — allowed but with cooldown >= 30s
SEG_STUTTER_PROB = 0.07       # occasional
STUTTER_MS = 120
STUTTER_REPEATS = (2, 3)
STUTTER_COOLDOWN_SEC = 30

# Crossfade a bit longer to de-click edits; still transparent.
CROSSFADE_MS = 120
LAYER_DRUMS_PROB = 0.35
LAYER_DRUM_GAIN = 0.55
MAIN_SEG_GAIN = 0.92

# Mastering — no soft clipping to avoid “kratzen/blechern”
MASTER_TARGET_PEAK = 0.88
SOFT_CLIP = False

# Split huge outputs safely below RIFF ~4 GB
PART_ROLLOVER_BYTES = 1_800_000_000

# === NEW: consolidated output settings ===
CONSOLIDATED_NAME = "output.wav"  # final requested render name

# ---- Instrument lexicon ----
INSTRUMENT_LEXICON = {
    "grand piano": ["grand piano","acoustic piano","apiano","steinway"],
    "electric piano": ["electric piano","e-piano","rhodes","wurli","mk1","mk2"],
    "real honky tonk piano": ["real honky tonk piano","honky tonk","honky-tonk"],
    "upright piano": ["upright piano"],
    "octave piano": ["octave piano"],
    "clavinet": ["clavinet","clav"],
    "harpsichord": ["harpsichord","cembalo"],
    "celesta": ["celesta"],
    "concert strings": ["concert strings","ensemble strings","string ensemble","strings ens","real strings"],
    "real strings": ["real strings","orch strings","orchestral strings"],
    "sweet violin": ["sweet violin","violin","vln"],
    "viola": ["viola","vla"],
    "cello": ["cello","vc"],
    "contrabass": ["contrabass","double bass","upright bass"],
    "flute": ["flute","concert flute","pan flute","shakuhachi"],
    "piccolo": ["piccolo"],
    "clarinet": ["clarinet","cl"],
    "oboe": ["oboe"],
    "english horn": ["english horn","cor anglais"],
    "bassoon": ["bassoon","bn"],
    "sweet tenor sax": ["sweet tenor sax","tenor sax","tenor saxophone","tsax"],
    "sweet alto sax": ["sweet alto sax","alto sax","alto saxophone","asax"],
    "sweet soprano sax": ["sweet soprano sax","soprano sax","ssax"],
    "baritone sax": ["baritone sax","bari sax","bsax"],
    "trumpet": ["trumpet","tpt"],
    "flugelhorn": ["flugelhorn"],
    "trombone": ["trombone","tbn"],
    "french horn": ["french horn","horn","hn"],
    "tuba": ["tuba"],
    "acoustic guitar": ["acoustic guitar","steel guitar","nylon guitar","classical guitar"],
    "electric guitar": ["electric guitar","eguitar","strat","tele","les paul","lp"],
    "jazz guitar": ["jazz guitar"],
    "muted guitar": ["muted guitar","palm mute guitar"],
    "electric bass": ["electric bass","e-bass","bass guitar","precision","j-bass","fretless bass"],
    "synth bass": ["synth bass","sub bass"],
    "upright bass": ["upright bass","double bass"],
    "synth lead": ["synth lead","lead synth","ld"],
    "synth pad": ["synth pad","pad","warm pad","string pad","air pad"],
    "synth pluck": ["synth pluck","pluck"],
    "synth brass": ["synth brass"],
    "synth keys": ["synth keys","poly synth","polysynth"],
    "fm piano": ["fm piano","dx7 piano"],
    "supersaw": ["supersaw"],
    "square lead": ["square lead"],
    "saw lead": ["saw lead"],
    "drum kit": ["drum kit","acoustic kit","studio kit"],
    "electronic kit": ["electronic kit","edm kit"],
    "909 kit": ["909","tr-909"],
    "808 kit": ["808","tr-808","808s"],
    "snare": ["snare"],
    "kick": ["kick","bd"],
    "hihat": ["hihat","hi-hat","hh"],
    "tom": ["tom"],
    "cymbal": ["cymbal","ride","crash","splash","china"],
    "timpani": ["timpani","kettle drum"],
    "percussion": ["percussion","perc","shaker","tambourine","cowbell","guiro","conga","bongo","timbale"],
    "rim": ["rim","rimshot","rim shot"],
    "bell": ["bell","bell synth","glockenspiel","tubular bells"],
    "synth": ["synth","synths"],
}
_ALIAS_TO_CANON = {a.lower(): canon for canon, aliases in INSTRUMENT_LEXICON.items() for a in aliases}

FAMILY_MAP = {
    "grand piano":"melodic","upright piano":"melodic","electric piano":"melodic",
    "real honky tonk piano":"melodic","octave piano":"melodic","clavinet":"melodic","harpsichord":"melodic","celesta":"melodic",
    "concert strings":"melodic","real strings":"melodic","sweet violin":"melodic","viola":"melodic","cello":"melodic","contrabass":"melodic",
    "flute":"melodic","piccolo":"melodic","clarinet":"melodic","oboe":"melodic","english horn":"melodic","bassoon":"melodic",
    "sweet tenor sax":"melodic","sweet alto sax":"melodic","sweet soprano sax":"melodic","baritone sax":"melodic",
    "trumpet":"melodic","flugelhorn":"melodic","trombone":"melodic","french horn":"melodic","tuba":"melodic",
    "acoustic guitar":"melodic","electric guitar":"melodic","jazz guitar":"melodic","muted guitar":"melodic",
    "electric bass":"melodic","synth bass":"melodic","upright bass":"melodic",
    "synth lead":"melodic","synth pad":"melodic","synth pluck":"melodic","synth brass":"melodic","synth keys":"melodic","fm piano":"melodic","supersaw":"melodic","square lead":"melodic","saw lead":"melodic",
    "bell":"melodic","synth":"melodic",
    "drum kit":"drums","electronic kit":"drums","909 kit":"drums","snare":"drums","kick":"drums","hihat":"drums","tom":"drums","cymbal":"drums","timpani":"drums",
    "percussion":"perc","808 kit":"perc","rim":"perc",
}
FAMILY_ORDER = ["melodic", "drums", "perc"]

# ------- Prompting -------

def prompt_directories():
    print("Enter one or more directories containing WAVs.")
    print("• Type a path and press Enter to add it.")
    print("• Press Enter on a blank line OR type 'done' to finish.\n")
    dirs, seen, idx = [], set(), 1
    while True:
        try:
            inp = input(f"[{idx}] Path (Enter or 'done' to finish): ").strip()
        except EOFError:
            print("\nNo input received. Exiting."); sys.exit(1)
        inp = inp.strip('"').strip("'")
        if not inp or inp.lower() == "done":
            if dirs:
                return dirs
            print("You haven't entered any directories yet. Please add at least one.\n")
            continue
        path = os.path.abspath(os.path.expanduser(inp))
        if os.path.isdir(path):
            if path in seen:
                print("Already added. Enter a different path or finish.\n"); continue
            dirs.append(path); seen.add(path); idx += 1
        else:
            print(f"Path not found or not a directory: {path}\nPlease try again.\n")

def prompt_output_directory(default_dir):
    while True:
        try:
            inp = input(f"Enter the OUTPUT directory (Press Enter to use input dir: {default_dir}): ").strip()
        except EOFError:
            print("\nNo input received. Exiting."); sys.exit(1)
        inp = inp.strip('"').strip("'")
        path = default_dir if not inp else os.path.abspath(os.path.expanduser(inp))
        if os.path.isdir(path):
            return path
        print(f"Path not found or not a directory: {path}\nPlease try again.\n")

# === NEW: prompt for maximum output length ===
def _parse_len_to_seconds(s):
    s = s.strip()
    if not s: return None
    if ":" in s:
        parts = s.split(":")
        if len(parts) == 2:
            m, sec = parts
            return int(m)*60 + int(sec)
        if len(parts) == 3:
            h, m, sec = parts
            return int(h)*3600 + int(m)*60 + int(sec)
    return int(s)

def prompt_max_output_seconds():
    while True:
        try:
            raw = input("Enter MAXIMUM output.wav length (seconds or mm:ss or hh:mm:ss): ").strip()
        except EOFError:
            print("\nNo input received. Exiting."); sys.exit(1)
        try:
            secs = _parse_len_to_seconds(raw)
            if secs and secs > 0:
                return secs
        except Exception:
            pass
        print("Please enter a positive number of seconds or a valid time like 6:30 or 01:10:00.")

def _yn(prompt, default="n"):
    d = default.lower()
    opt = "Y/n" if d == "y" else "y/N"
    while True:
        try:
            raw = input(f"{prompt} ({opt}): ").strip().lower()
        except EOFError:
            print("\nNo input received. Exiting."); sys.exit(1)
        if raw == "":
            return d == "y"
        if raw in ("y","yes"):
            return True
        if raw in ("n","no"):
            return False
        print("Please enter y or n.")

def prompt_sort_scope():
    print("\nSorting scope:")
    print("  1) BOTH: Primary (instrument/grouping) + Secondary (alpha/size/random/MIX)")
    print("  2) PRIMARY only: instrument/grouping only")
    print("  3) SECONDARY only: ignore instruments")
    while True:
        try:
            raw = input("Select 1/2/3 (Enter = 1): ").strip()
        except EOFError:
            print("\nNo input received. Exiting."); sys.exit(1)
        if not raw:
            return 1
        if raw in ("1","2","3"):
            return int(raw)
        print("Please enter 1, 2, or 3.\n")

def _normalize_label_for_match(s: str) -> str:
    s = s.lower()
    s = s.replace("*", " ").replace("-", " ").replace(".", " ")
    s = re.sub(r"\s+", " ", s).strip()
    return s

def _strip_digits_for_alpha(s: str) -> str:
    return re.sub(r"\d", "", s)

def prompt_primary_mode():
    print("\nPrimary (instrument) mode:")
    print("  1) Custom instrument sequence (you type instrument names in order)")
    print("  2) Music-type priority (melodic/harmonic → drums → perc/808/rims)")
    while True:
        try:
            raw = input("Select 1/2 (Enter = 1): ").strip()
        except EOFError:
            print("\nNo input received. Exiting."); sys.exit(1)
        if not raw:
            return 1
        if raw in ("1","2"):
            return int(raw)
        print("Please enter 1 or 2.\n")

def prompt_instrument_sequence():
    print("\nEnter instrument names in the EXACT order you want grouped (top→bottom).")
    print("Type instrument names (alias or canonical). Type 'done' to finish.\n")
    ordered, used, idx = [], set(), 1
    while True:
        try:
            raw = input(f"Instrument #{idx} (or 'done'): ").strip()
        except EOFError:
            print("\nNo input received. Exiting."); sys.exit(1)
        if not raw or raw.lower() == "done":
            if ordered:
                return ordered
            print("Please enter at least one instrument or type 'done' after adding some.")
            continue
        key = raw.lower().strip()
        canon = _ALIAS_TO_CANON.get(key) or (key if key in INSTRUMENT_LEXICON else key)
        if canon in used:
            print("Already added. Choose another or type 'done'."); continue
        ordered.append(canon); used.add(canon); idx += 1

def auto_instrument_sequence_by_family():
    fam_to_list = {f: [] for f in FAMILY_ORDER}
    for canon in INSTRUMENT_LEXICON.keys():
        fam = FAMILY_MAP.get(canon, "melodic")
        if fam in fam_to_list:
            fam_to_list[fam].append(canon)
    seq = []
    for fam in FAMILY_ORDER:
        seq.extend(sorted(fam_to_list[fam]))
    return seq

def prompt_order_mode():
    print("\nSecondary ordering (applies globally or inside groups):")
    print("  1) numeric/alphabetical (0..Z)")
    print("  2) most big from top to bottom (by estimated WAV size)")
    print("  3) random")
    print("  4) random-by-size (weighted randomness by estimated size)")
    print("  5) MIX: blend A/B/C/D with percentages")
    while True:
        try:
            raw = input("Select 1/2/3/4/5 (Enter = 1): ").strip()
        except EOFError:
            print("\nNo input received. Exiting."); sys.exit(1)
        if not raw:
            return 1
        if raw in ("1","2","3","4","5"):
            return int(raw)
        print("Please enter 1, 2, 3, 4, or 5.\n")

def _read_pct(label, default="0"):
    while True:
        try:
            raw = input(f"{label} percentage 0-100 (Enter = {default}): ").strip()
        except EOFError:
            print("\nNo input received. Exiting."); sys.exit(1)
        if not raw:
            raw = default
        try:
            v = int(raw)
            v = 0 if v<0 else (100 if v>100 else v)
            return v
        except ValueError:
            print("Please enter an integer 0..100.")

def prompt_mix_percentages():
    print("\nMIX mode: set how much each function influences the order.")
    print("A = numeric/alphabetical (0..Z)")
    print("B = sort by .wav size (largest first)")
    print("C = randomize by alphabetical (biased random)")
    print("D = randomize by .wav size (biased random)\n")
    A = _read_pct("A (alpha 0..Z)","100")
    B = _read_pct("B (size desc)","0")
    C = _read_pct("C (random-by-alpha)","0")
    D = _read_pct("D (random-by-size)","0")
    total = A+B+C+D
    if total == 0:
        return (1.0,0.0,0.0,0.0)
    return (A/total, B/total, C/total, D/total)

# === NEW: per-instrument percentages (tied to custom instrument sequence) ===
def prompt_per_instrument_percentages(instr_sequence):
    print("\nSet percentages for each instrument (0–100). Type just the number (e.g., 25).")
    weights = {}
    total = 0
    for canon in instr_sequence:
        key = canon
        try:
            raw = input(f'  {canon}: ').strip()
        except EOFError:
            print("\nNo input received. Exiting."); sys.exit(1)
        try:
            v = int(raw)
        except Exception:
            v = 0
        if v < 0: v = 0
        if v > 100: v = 100
        weights[key] = v
        total += v
    if total <= 0:
        return {}
    return {k: (v/float(total)) for k, v in weights.items()}

def prompt_start_part():
    while True:
        try:
            raw = input("Start writing at which PART number? (Press Enter for 1): ").strip()
        except EOFError:
            print("\nNo input received. Exiting."); sys.exit(1)
        if not raw:
            return 1
        try:
            part = int(raw)
            if 1 <= part <= 99:
                return part
            print("Please enter a number between 1 and 99.")
        except ValueError:
            print("Please enter a valid integer (e.g., 1, 2, 3, ...).")

# ------- FS walk / gather --------

def is_our_temp_or_output(path: str) -> bool:
    name = os.path.basename(path)
    return name.startswith(REMIX_OUTPUT_BASENAME + "_part") or name == CONSOLIDATED_NAME

def fast_walk(root):
    stack = [root]
    while stack:
        cur = stack.pop()
        try:
            with os.scandir(cur) as it:
                for e in it:
                    if e.is_dir(follow_symlinks=False):
                        stack.append(e.path)
                    elif e.is_file(follow_symlinks=False):
                        yield e.path
        except PermissionError:
            pass

def gather_candidates(root):
    files = []
    for p in fast_walk(root):
        if p.lower().endswith(SUPPORTED_EXTS) and not is_our_temp_or_output(p):
            files.append(p)
    files.sort()
    return files

# ----- Quick header read -----

def _read_exact(f, n):
    b = f.read(n)
    if len(b) != n:
        raise EOFError("Unexpected EOF")
    return b

def quick_wav_header(path):
    with open(path, "rb") as f:
        if _read_exact(f, 4) != b"RIFF": raise ValueError("Not RIFF")
        _ = _read_exact(f, 4)
        if _read_exact(f, 4) != b"WAVE": raise ValueError("Not WAVE")
        fmt = None; data_off = None; data_size = None
        while True:
            hdr = f.read(8)
            if len(hdr) < 8: break
            chunk_id, size = hdr[:4], struct.unpack("<I", hdr[4:])[0]
            if chunk_id == b"fmt ":
                raw = _read_exact(f, size)
                if size < 16: raise ValueError("fmt chunk too small")
                (wFormatTag, nChannels, nSamplesPerSec, _avg, nBlockAlign, wBitsPerSample) = struct.unpack("<HHIIHH", raw[:16])
                fmt = (wFormatTag, nChannels, nSamplesPerSec, wBitsPerSample, nBlockAlign)
            elif chunk_id == b"data":
                data_off = f.tell(); data_size = size; f.seek(size, io.SEEK_CUR)
            else:
                f.seek(size, io.SEEK_CUR)
        if not fmt or data_off is None:
            raise ValueError("Missing fmt or data chunk")
        format_tag, channels, sr, bps, blockalign = fmt
        if blockalign == 0:
            raise ValueError("Invalid blockalign=0")
        nframes = data_size // blockalign
        return {
            "format_tag": format_tag,
            "channels": channels,
            "sr": sr,
            "bits_per_sample": bps,
            "blockalign": blockalign,
            "data_offset": data_off,
            "data_size": data_size,
            "nframes": nframes,
        }

# ----- Byte conversions -----

def pcm24_to_int16le_bytes(raw_bytes):
    out = bytearray(len(raw_bytes) // 3 * 2); o = 0
    for i in range(0, len(raw_bytes), 3):
        b0, b1, b2 = raw_bytes[i], raw_bytes[i+1], raw_bytes[i+2]
        val = b0 | (b1 << 8) | (b2 << 16)
        if val & 0x800000: val -= 0x1000000
        s16 = max(-32768, min(32767, val // 256))
        out[o:o+2] = struct.pack("<h", s16); o += 2
    return bytes(out)

def float32_to_int16le_bytes(raw_bytes):
    out = bytearray(len(raw_bytes) // 2); o = 0
    for (x,) in struct.iter_unpack("<f", raw_bytes):
        x = 1.0 if x > 1.0 else (-1.0 if x < -1.0 else x)
        s16 = int(x * 32767.0)
        s16 = -32768 if s16 < -32768 else (32767 if s16 > 32767 else s16)
        out[o:o+2] = struct.pack("<h", s16); o += 2
    return bytes(out)

def mono_to_stereo(raw16, channels):
    if channels == 2: return raw16
    out = bytearray(len(raw16) * 2); o = 0
    for i in range(0, len(raw16), 2):
        s = raw16[i:i+2]
        out[o:o+2] = s; out[o+2:o+4] = s; o += 4
    return bytes(out)

def nch_to_stereo_take_first_two(raw16, channels):
    if channels == 2: return raw16
    if channels == 1: return mono_to_stereo(raw16, 1)
    frame_count = len(raw16) // (2 * channels)
    out = bytearray(frame_count * 4); o = 0; step = 2 * channels
    for i in range(0, len(raw16), step):
        out[o:o+2] = raw16[i:i+2]
        out[o+2:o+4] = raw16[i+2:i+4]; o += 4
    return bytes(out)

def resample_linear_int16(raw16_stereo, src_sr, dst_sr):
    if src_sr == dst_sr or len(raw16_stereo) < 4:
        return raw16_stereo
    total_samples = len(raw16_stereo) // 2
    frames_src = total_samples // 2
    samples = list(struct.iter_unpack("<{}h".format(total_samples), raw16_stereo))
    L_src = list(samples[0::2]); R_src = list(samples[1::2])
    ratio = dst_sr / float(src_sr)
    frames_dst = int(round(frames_src * ratio))
    if frames_src <= 1 or frames_dst <= 1: return raw16_stereo
    out = bytearray(frames_dst*4); o = 0
    step = src_sr / float(dst_sr); pos = 0.0; last = frames_src - 1
    for _ in range(frames_dst):
        i0 = int(pos); i1 = i0 + 1
        if i1 > last: i1 = last
        frac = pos - i0
        l = int(L_src[i0] + (L_src[i1] - L_src[i0]) * frac)
        r = int(R_src[i0] + (R_src[i1] - R_src[i0]) * frac)
        l = -32768 if l < -32768 else (32767 if l > 32767 else l)
        r = -32768 if r < -32768 else (32767 if r > 32767 else r)
        out[o:o+2] = struct.pack("<h", l); out[o+2:o+4] = struct.pack("<h", r); o += 4
        pos += step
    return bytes(out)

def _db_to_lin(db): return 10.0 ** (db / 20.0)

def trim_and_gate_int16_stereo(raw16_stereo, sr, silence_db=SILENCE_TRIM_DB, min_sil_ms=MIN_SILENCE_MS,
                               gate_db=NOISE_GATE_DB, gate_mult=NOISE_GATE_REDUCTION,
                               remove_internal_sil_ms=REMOVE_INTERNAL_SILENCE_MS):
    if len(raw16_stereo) < 4: return raw16_stereo, 0, 0
    samples = list(struct.iter_unpack("<hh", raw16_stereo))
    frames = len(samples)
    if frames == 0: return raw16_stereo, 0, 0

    def frame_abs(i):
        l,r = samples[i]; return (abs(l)+abs(r))//2

    sil_th = int(32767 * _db_to_lin(silence_db))
    gate_th = int(32767 * _db_to_lin(gate_db))
    min_sil_frames = max(1, int(sr * (min_sil_ms/1000.0)))

    # leading trim
    start = 0; i = 0
    while i < frames:
        block_end = min(frames, i + min_sil_frames)
        if max(frame_abs(k) for k in range(i, block_end)) > sil_th:
            start = max(0, i - int(0.005*sr)); break
        i = block_end
    else:
        return b"", 0, frames

    # trailing trim
    end = frames; j = frames
    while j > 0:
        block_start = max(0, j - min_sil_frames)
        if max(frame_abs(k) for k in range(block_start, j)) > sil_th:
            end = min(frames, j + int(0.005*sr)); break
        j = block_start

    work = samples[start:end]

    # light gate (disabled via params)
    if gate_mult < 1.0:
        scaled=[]
        for (l, r) in work:
            if (abs(l)+abs(r))//2 < gate_th:
                l = int(l*gate_mult); r = int(r*gate_mult)
            l = -32768 if l < -32768 else (32767 if l > 32767 else l)
            r = -32768 if r < -32768 else (32767 if r > 32767 else r)
            scaled.append((l, r))
        work = scaled

    # remove internal long silences (disabled via params)
    if remove_internal_sil_ms and remove_internal_sil_ms > 0:
        min_block = int(sr * (remove_internal_sil_ms/1000.0))
        out_frames = []
        acc_sil = 0
        for (l, r) in work:
            if (abs(l)+abs(r))//2 < sil_th:
                acc_sil += 1
                if acc_sil >= min_block:
                    acc_sil = 0
                    continue
            else:
                out_frames.append((l, r))
                acc_sil = 0
        if out_frames:
            work = out_frames

    out = bytearray(len(work)*4); o=0
    for (l,r) in work:
        out[o:o+2]=struct.pack("<h", l); out[o+2:o+4]=struct.pack("<h", r); o+=4
    return bytes(out), start, end

def read_convert_to_target(path, hdr=None):
    if hdr is None: hdr = quick_wav_header(path)
    fmt = hdr["format_tag"]; sr = hdr["sr"]; ch = hdr["channels"]; bps = hdr["bits_per_sample"]; nframes = hdr["nframes"]
    if fmt == 1:
        with wave.open(path, "rb") as r:
            r.setpos(0); raw = r.readframes(nframes); sampwidth = bps // 8
            if sampwidth == 3:
                raw16 = pcm24_to_int16le_bytes(raw)
            elif sampwidth == 2:
                raw16 = raw
            elif sampwidth == 1:
                out = bytearray(nframes * ch * 2); o=0
                for (u,) in struct.iter_unpack("<B", raw):
                    s = (u - 128) << 8; out[o:o+2] = struct.pack("<h", s); o+=2
                raw16 = bytes(out)
            else:
                raise wave.Error(f"Unsupported PCM bit depth: {bps} bit")
    elif fmt == 3 and bps == 32:
        bytes_per_frame = ch * 4; to_read = nframes * bytes_per_frame
        with open(path, "rb") as f:
            f.seek(hdr["data_offset"], io.SEEK_SET); buf = f.read(to_read)
            raw16 = float32_to_int16le_bytes(buf)
    else:
        raise wave.Error(f"Unsupported WAV format (format_tag={fmt}, bits={bps}).")

    if ch == 1:
        raw16 = mono_to_stereo(raw16, 1); ch = 2
    elif ch != 2:
        raw16 = nch_to_stereo_take_first_two(raw16, ch); ch = 2
    if sr != TARGET_SR:
        raw16 = resample_linear_int16(raw16, sr, TARGET_SR); sr = TARGET_SR
    return raw16, sr, 2, 16

# --- Estimate output bytes (after convert, pre-trim) ---
def estimate_output_bytes_after_convert(hdr):
    frames_src = hdr["nframes"]; sr_src = hdr["sr"]
    if sr_src <= 0: return 0
    frames_dst = frames_src if sr_src == TARGET_SR else int(round(frames_src * (TARGET_SR / float(sr_src))))
    return frames_dst * TARGET_CHANNELS * TARGET_SAMPWIDTH

# ---- Ordering helpers ----
def _normalized_ranks(items, key_vals, reverse=False):
    idxs = list(range(len(items)))
    idxs.sort(key=lambda i: key_vals[i], reverse=reverse)
    ranks = [0]*len(items)
    for r,i in enumerate(idxs):
        ranks[i]=r
    if len(items) <= 1: return [0.0]*len(items)
    return [r/(len(items)-1) for r in ranks]

def _safe_norm(vals):
    if not vals: return []
    m = max(vals)
    if m <= 0: return [0.0]*len(vals)
    return [v/m for v in vals]

def alpha_key(name: str):
    return _strip_digits_for_alpha(name.lower())

def order_files(files, mode, mix_weights=None):
    if mode == 1:
        return sorted(files, key=lambda p: alpha_key(os.path.basename(p)))
    need_est = mode in (2,4,5)
    est = None
    if need_est:
        est = []
        for p in files:
            try:
                hdr = quick_wav_header(p); e = estimate_output_bytes_after_convert(hdr); e = 1 if e<=0 else e
            except Exception:
                e = 1
            est.append(e)
    if mode == 2:
        return [fp for _, fp in sorted(zip(est, files), key=lambda t: t[0], reverse=True)]
    if mode == 3:
        shuffled = list(files); random.shuffle(shuffled); return shuffled
    if mode == 4:
        pool = list(files); weights = list(est); ordered = []
        while pool:
            total = sum(weights)
            if total <= 0:
                idx = random.randrange(len(pool))
            else:
                r = random.uniform(0, total); acc = 0.0; idx = 0
                for i,w in enumerate(weights):
                    acc += w
                    if r <= acc:
                        idx = i; break
            ordered.append(pool.pop(idx)); weights.pop(idx)
        return ordered
    if mode == 5:
        alpha_sorted = sorted(range(len(files)), key=lambda i: alpha_key(os.path.basename(files[i])))
        alpha_rank = [0]*len(files)
        for r,i in enumerate(alpha_sorted):
            alpha_rank[i]=r
        alpha_rank = [r/(len(files)-1) if len(files)>1 else 0.0 for r in alpha_rank]
        if est is None: est = [1]*len(files)
        size_rank = _normalized_ranks(files, est, reverse=True)
        eps = 1e-9
        rand_alpha = []
        for ar in alpha_rank:
            denom = max(eps, 1.0 - ar); rand_alpha.append(random.random()/denom)
        norm_size = _safe_norm(est)
        rand_size = []
        for ns in norm_size:
            denom = max(1e-9, ns); rand_size.append(random.random()/denom)
        wA,wB,wC,wD = mix_weights if mix_weights else (1.0,0.0,0.0,0.0)
        scores = []
        for i in range(len(files)):
            s = (wA*alpha_rank[i]) + (wB*size_rank[i]) + (wC*rand_alpha[i]) + (wD*rand_size[i])
            scores.append((s,i))
        scores.sort(key=lambda t: t[0])
        return [files[i] for _,i in scores]
    return files

# ---- Instrument detection & grouping ----
def detect_canonical_instrument(filename_lower_norm: str):
    for alias, canon in _ALIAS_TO_CANON.items():
        if alias in filename_lower_norm:
            return canon
    for canon in INSTRUMENT_LEXICON.keys():
        if canon in filename_lower_norm:
            return canon
    return None

def build_presence_report(files):
    counts = {}
    for p in files:
        name = os.path.basename(p)
        norm = _normalize_label_for_match(name)
        norm_nodigits = _strip_digits_for_alpha(norm)
        found = detect_canonical_instrument(norm) or detect_canonical_instrument(norm_nodigits)
        if found:
            counts[found] = counts.get(found, 0) + 1
    if counts:
        print("\n📈 Instrument hits (filename matches):")
        for k in sorted(counts.keys()):
            print(f"  • {k}: {counts[k]}")

def group_files_by_instruments(files, user_order):
    buckets = {g: [] for g in user_order}
    others = []
    for p in files:
        name = os.path.basename(p)
        norm = _normalize_label_for_match(name)
        norm_nodigits = _strip_digits_for_alpha(norm)
        found = detect_canonical_instrument(norm) or detect_canonical_instrument(norm_nodigits)
        if found and found in buckets:
            buckets[found].append(p)
        else:
            others.append(p)
    ordered_groups = []
    for g in user_order:
        ordered_groups.append((g, buckets[g]))
    if others:
        ordered_groups.append(("(other)", others))
    return ordered_groups

# ==== Remix engine helpers (pure Python) ====
def _bytes_to_frames(raw_bytes):  # -> list[(l,r)]
    return [xy for xy in struct.iter_unpack("<hh", raw_bytes)]

def _frames_to_bytes(frames):
    out = bytearray(len(frames)*4); o = 0
    for (l, r) in frames:
        out[o:o+2] = struct.pack("<h", l)
        out[o+2:o+4] = struct.pack("<h", r)
        o += 4
    return bytes(out)

def _pp_clip(v):  # clip int to 16-bit
    return -32768 if v < -32768 else (32767 if v > 32767 else v)

def _pp_apply_gain(frames, gain):
    if gain == 1.0: return frames
    out = []
    for (l, r) in frames:
        out.append((_pp_clip(int(l*gain)), _pp_clip(int(r*gain))))
    return out

def _pp_reverse(frames): return list(reversed(frames))

def _pp_lin_ramp(n): return [i/(n-1) if n>1 else 1.0 for i in range(n)]

def _pp_crossfade(a, b, cross_frames):
    if cross_frames <= 0 or not a:
        return a + b
    cross_frames = min(cross_frames, len(a), len(b))
    fade_out = _pp_lin_ramp(cross_frames)[::-1]
    fade_in  = _pp_lin_ramp(cross_frames)
    out = a[:-cross_frames]
    for i in range(cross_frames):
        l = _pp_clip(int(a[-cross_frames+i][0]*fade_out[i] + b[i][0]*fade_in[i]))
        r = _pp_clip(int(a[-cross_frames+i][1]*fade_out[i] + b[i][1]*fade_in[i]))
        out.append((l, r))
    out.extend(b[cross_frames:])
    return out

def _pp_stutter_tail(frames, sr):
    tail = max(1, int(sr*(STUTTER_MS/1000.0)))
    if len(frames) <= tail: return frames
    chunk = frames[-tail:]
    reps = random.randint(STUTTER_REPEATS[0], STUTTER_REPEATS[1])
    out = frames[:]
    for _ in range(reps):
        out.extend(chunk)
    return out

def _pp_normalize(frames, target_peak=MASTER_TARGET_PEAK, soft_clip=SOFT_CLIP):
    peak = 1
    for (l, r) in frames:
        a = abs(l); b = abs(r)
        if a > peak: peak = a
        if b > peak: peak = b
    if peak == 0: return frames
    scale = (target_peak * 32767.0) / float(peak)
    out = []
    if soft_clip:
        def sc(x):
            y = math.tanh(1.4 * (x/32768.0))
            return _pp_clip(int(y * 32767.0))
        for (l, r) in frames:
            l2 = sc(int(l*scale)); r2 = sc(int(r*scale))
            out.append((l2, r2))
    else:
        for (l, r) in frames:
            out.append((_pp_clip(int(l*scale)), _pp_clip(int(r*scale))))
    return out

def _pick_segments_from_clip_bytes(cleaned_bytes, sr):
    frames = _bytes_to_frames(cleaned_bytes)
    min_len = int(sr*(SEG_MIN_MS/1000.0))
    max_len = int(sr*(SEG_MAX_MS/1000.0))
    if len(frames) < min_len: return []
    segs = []
    aim = max(1, int(len(frames) / ((min_len+max_len)/2)))
    for _ in range(aim):
        seg_len = random.randint(min_len, max_len)
        if seg_len >= len(frames):
            start = 0
        else:
            start = random.randint(0, len(frames)-seg_len)
        segs.append(frames[start:start+seg_len])
    return segs

def _class_of_canon(canon): return FAMILY_MAP.get(canon, "melodic")

def _instrument_of_path(path):
    name = os.path.basename(path)
    norm = _normalize_label_for_match(name)
    norm_nodigits = _strip_digits_for_alpha(norm)
    return detect_canonical_instrument(norm) or detect_canonical_instrument(norm_nodigits)

def _sec_to_frames(sec, sr=TARGET_SR): return int(round(sec * sr))

# === NEW: 63/27 loop builder with 21/21/21 inheritance ===
def build_looped_mix(files, inst_weight_map=None, out_frames_target=None):
    """
    Pattern: 63s, 27s, 63s, 27s, 63s, ... until target length.
    Each 63s block = three recurring 21s subloops; their order rotates each 63s block.
    27s blocks = rhythmic variants using only input material.
    Only glitches/stutters allowed, with >=30s cooldown between occurrences.
    """
    sr = TARGET_SR
    cross_frames = int(sr * (CROSSFADE_MS/1000.0))

    drum_segs = []; perc_segs = []; melodic_segs = []
    file_segments = []

    def weight_for(canon):
        if not inst_weight_map or not canon: return 0.0
        return inst_weight_map.get(canon, 0.0)

    # Collect segments from files (cleaned, gated, trimmed)
    for path in files:
        try:
            hdr = quick_wav_header(path)
            raw16, sr_, ch, bps = read_convert_to_target(path, hdr)
            cleaned, _, _ = trim_and_gate_int16_stereo(raw16, sr_)
            if not cleaned: continue
            canon = _instrument_of_path(path)
            cls = _class_of_canon(canon) if canon else "melodic"
            allsegs = _pick_segments_from_clip_bytes(cleaned, sr)
            if not allsegs: continue

            # Emphasis multiplier per instrument
            mult = 1.0 + 2.0*weight_for(canon)  # 1x..3x
            keep = max(1, int(round(len(allsegs) * mult)))
            max_keep = 64
            if keep > max_keep: keep = max_keep
            segs = allsegs[:keep]

            # Apply main gain
            segs = [_pp_apply_gain(s, MAIN_SEG_GAIN) for s in segs]

            if cls == "drums": drum_segs.extend(segs)
            elif cls == "perc": perc_segs.extend(segs)
            else: melodic_segs.extend(segs)

            file_segments.append((cls, canon, len(segs)))
        except KeyboardInterrupt:
            print("\n🛑 Interrupted by user.", flush=True); raise
        except Exception as e:
            print(f"   ⚠️  Skipping {os.path.basename(path)}: {e}", flush=True)

    # Motif pools
    motif_pool = melodic_segs or drum_segs or perc_segs
    base_pool  = (melodic_segs + drum_segs + perc_segs) or [[]]

    if not motif_pool:
        # Fallback: 1s silence
        return [(0,0)] * sr, {"total_frames": sr}

    # Pick three recurring 21s motifs (A,B,C) from motif_pool, reused across all 63s blocks
    def take_exact_len(frames, n):
        if not frames: return []
        if len(frames) >= n: return frames[:n]
        reps = int(math.ceil(n/len(frames)))
        out = (frames * reps)[:n]
        return out

    recurring = []
    need_21 = _sec_to_frames(21, sr)
    random.shuffle(motif_pool)
    for i in range(3):
        src = random.choice(motif_pool)
        recurring.append(take_exact_len(src, need_21))

    # Helper: create a 27s variant block by slicing/reordering only from input segs
    def make_27_variant():
        need = _sec_to_frames(27, sr)
        segs = []
        # build from short rhythmic chunks selected from drums/perc/melodic
        pools = []
        if drum_segs: pools.append(drum_segs)
        if perc_segs: pools.append(perc_segs)
        if melodic_segs: pools.append(melodic_segs)
        if not pools:
            return [(0,0)] * need
        cur = []
        last_stutter_at = -10**9
        assembled_len = 0
        while assembled_len < need:
            pool = random.choice(pools)
            s = random.choice(pool)
            # take a rhythmic slice (0.4s..1.2s)
            slen = _sec_to_frames(random.uniform(0.4, 1.2), sr)
            if len(s) <= slen:
                frag = s[:]
            else:
                start = random.randint(0, max(0, len(s)-slen))
                frag = s[start:start+slen]
            # sometimes reverse tiny slice
            if random.random() < 0.2:
                frag = _pp_reverse(frag)
            # maybe layer with a drum/percussion slice
            if random.random() < 0.35 and (drum_segs or perc_segs):
                lp = drum_segs if random.random() < 0.7 and drum_segs else (perc_segs or drum_segs)
                lay = random.choice(lp)
                lay = take_exact_len(lay, len(frag))
                lay = _pp_apply_gain(lay, LAYER_DRUM_GAIN)
                mixed = []
                for (l1,r1),(l2,r2) in zip(frag, lay):
                    mixed.append((_pp_clip(l1+l2), _pp_clip(r1+r2)))
                frag = mixed
            # crossfade append
            cur = _pp_crossfade(cur, frag, cross_frames) if cur else frag[:]
            assembled_len = len(cur)

            # enforce stutter cooldown (≥30s)
            if random.random() < SEG_STUTTER_PROB:
                if assembled_len - last_stutter_at >= _sec_to_frames(STUTTER_COOLDOWN_SEC, sr):
                    cur = _pp_stutter_tail(cur, sr)
                    assembled_len = len(cur)
                    last_stutter_at = assembled_len
        return cur[:need]

    # Assemble timeline: [63,27,63,27,...] rotating A/B/C inside each 63
    assembled = []
    rotation = 0
    total_target = out_frames_target if out_frames_target else _sec_to_frames(600, sr)  # default 10 min if not provided
    last_stutter_global = -10**9

    while len(assembled) < total_target:
        # 63 block = 21+21+21 with rotation of (A,B,C)
        order = [(rotation + i) % 3 for i in range(3)]
        block63 = []
        for idx in order:
            block63 = _pp_crossfade(block63, recurring[idx], cross_frames) if block63 else recurring[idx][:]
        # occasional stutter on block boundary with global cooldown
        if random.random() < SEG_STUTTER_PROB:
            if len(assembled) - last_stutter_global >= _sec_to_frames(STUTTER_COOLDOWN_SEC, sr):
                block63 = _pp_stutter_tail(block63, sr)
                last_stutter_global = len(assembled) + len(block63)

        assembled = _pp_crossfade(assembled, block63, cross_frames) if assembled else block63[:]
        if len(assembled) >= total_target: break

        # 27 variant
        block27 = make_27_variant()
        assembled = _pp_crossfade(assembled, block27, cross_frames)
        rotation = (rotation + 1) % 3

    assembled = assembled[:total_target]
    assembled = _pp_normalize(assembled, MASTER_TARGET_PEAK, SOFT_CLIP)
    return assembled, {"total_frames": len(assembled)}

# --- Write WAV in parts (RIFF size safety) ---
def write_wav_parts(arr_frames, outdir, basename=REMIX_OUTPUT_BASENAME):
    bytes_per_frame = TARGET_CHANNELS * TARGET_SAMPWIDTH
    total_frames = len(arr_frames)
    start = 0; part_idx = 1
    while start < total_frames:
        max_frames = PART_ROLLOVER_BYTES // bytes_per_frame
        end = min(total_frames, start + int(max_frames))
        chunk = arr_frames[start:end]
        out_path = os.path.join(outdir, f"{basename}_part{part_idx:02d}.wav")
        with wave.open(out_path, "wb") as wf:
            wf.setnchannels(TARGET_CHANNELS)
            wf.setsampwidth(TARGET_SAMPWIDTH)
            wf.setframerate(TARGET_SR)
            wf.writeframes(_frames_to_bytes(chunk))
        mb = ((end-start) * bytes_per_frame) / 1_000_000.0
        print(f"💾 Wrote {os.path.basename(out_path)}  ({mb:.1f} MB)", flush=True)
        start = end; part_idx += 1

def write_consolidated_wav(arr_frames, outdir, filename):
    bytes_per_frame = TARGET_CHANNELS * TARGET_SAMPWIDTH
    total_frames = len(arr_frames)
    total_bytes = total_frames * bytes_per_frame
    RIFF_MAX_BYTES = (0xFFFFFFFF - 44)
    if total_bytes >= RIFF_MAX_BYTES:
        print(f"⚠️  Consolidated WAV (~{total_bytes/1_000_000:.1f} MB) exceeds RIFF limit; skipping single-file render.", flush=True)
        return None
    out_path = os.path.join(outdir, filename)
    with wave.open(out_path, "wb") as wf:
        wf.setnchannels(TARGET_CHANNELS)
        wf.setsampwidth(TARGET_SAMPWIDTH)
        wf.setframerate(TARGET_SR)
        # Write in 5s chunks
        chunk_frames = TARGET_SR * 5
        for start in range(0, total_frames, chunk_frames):
            wf.writeframes(_frames_to_bytes(arr_frames[start:start+chunk_frames]))
    est_mb = total_bytes / 1_000_000.0
    print(f"✅ Wrote {os.path.basename(out_path)} (~{est_mb:.1f} MB)", flush=True)
    try:
        real_size = os.path.getsize(out_path)
        print(f"   • Actual file size: {real_size/1_000_000:.3f} MB ({real_size} bytes)", flush=True)
    except Exception:
        pass
    return out_path

# --- Main pipeline ---
def main(root_dirs, output_dir, sort_scope, primary_mode, instr_sequence, order_mode, mix_weights, out_seconds):
    # normalize roots list
    if isinstance(root_dirs, (str, bytes, os.PathLike)):
        root_dirs = [str(root_dirs)]

    print("🔎 Scanning:", flush=True)
    all_files, seen_paths = [], set()
    for rd in root_dirs:
        print(f"   • {rd}", flush=True)
        files = gather_candidates(rd)
        for p in files:
            if p not in seen_paths:
                seen_paths.add(p); all_files.append(p)

    print(f"📂 Found {len(all_files)} WAV candidates across {len(root_dirs)} input path(s).", flush=True)
    if not all_files:
        print("❌ No audio files found.", flush=True); return

    build_presence_report(all_files)

    # Primary / Secondary sorting
    if sort_scope in (1, 2):
        if primary_mode == 2:
            instr_sequence = auto_instrument_sequence_by_family()
            print("\n🎼 Music-type priority engaged: melodic/harmonic → drums → perc/808/rims (bottom).")

        print("\n🪄 Instrument-aware grouping prepared.")
        groups = group_files_by_instruments(all_files, instr_sequence)

        if not _yn("Accept primary sorting by detected instrument groups?", default="y"):
            if sort_scope == 2:
                print("↪️  Primary declined; using alphabetical (digits ignored).")
                all_files = sorted(all_files, key=lambda p: alpha_key(os.path.basename(p)))
            else:
                print("↪️  Primary declined; proceeding with Secondary only.")
                sort_scope = 3
        else:
            ordered = []
            for _, gfiles in groups:
                if not gfiles: continue
                if sort_scope == 1:
                    gf_sorted = order_files(gfiles, order_mode, mix_weights=mix_weights)
                else:
                    gf_sorted = sorted(gfiles, key=lambda p: alpha_key(os.path.basename(p)))
                ordered.extend(gf_sorted)
            all_files = ordered

    if sort_scope == 3:
        all_files = order_files(all_files, order_mode, mix_weights=mix_weights)

    # Per-instrument percentages after custom sequence
    inst_weight_map = None
    if sort_scope in (1, 2) and primary_mode == 1 and instr_sequence:
        print("\n🎚️  Per-instrument percentages (influences the remix itself):")
        inst_weight_map = prompt_per_instrument_percentages(instr_sequence)
        if inst_weight_map:
            print("\n🎯 Target emphasis:")
            for k in instr_sequence:
                if k in inst_weight_map:
                    print(f"  • {k}: {inst_weight_map[k]*100:.1f}%")
        else:
            print("No emphasis set (all instruments equal).")

    # Build to target length using 63/27 pattern
    print("\n🎛️  Building 63/27 looped remix (21/21/21 inheritance) ...", flush=True)
    target_frames = _sec_to_frames(out_seconds, TARGET_SR)
    remix_data, stats = build_looped_mix(all_files, inst_weight_map=inst_weight_map, out_frames_target=target_frames)

    print("\n📦 Writing consolidated WAV ...", flush=True)
    path_visible = write_consolidated_wav(remix_data, output_dir, filename=CONSOLIDATED_NAME)

    print("\n📦 Writing output parts (rollover safety) ...", flush=True)
    write_wav_parts(remix_data, output_dir, basename=REMIX_OUTPUT_BASENAME)

    if path_visible and os.path.isfile(path_visible):
        try:
            sz = os.path.getsize(path_visible)
            print(f"ℹ️  {os.path.basename(path_visible)} size: {sz/1_000_000:.3f} MB ({sz} bytes)", flush=True)
        except Exception:
            pass

    print("✅ Remix complete.", flush=True)

# ====== Entrypoint ======
if __name__ == "__main__":
    try:
        roots = prompt_directories()
        outdir = prompt_output_directory(default_dir=roots[0])
        # === NEW PROMPT (placed right after paths) ===
        max_len_sec = prompt_max_output_seconds()

        sort_scope = prompt_sort_scope()

        primary_mode = 1
        instr_seq = []
        if sort_scope in (1, 2):
            primary_mode = prompt_primary_mode()
            if primary_mode == 1:
                instr_seq = prompt_instrument_sequence()
                print("\nYour instrument group order:")
                for i, it in enumerate(instr_seq, 1):
                    print(f"  {i:02d}. {it}")
            else:
                instr_seq = []  # auto-built later

        order_mode = 1
        mix_weights = None
        if sort_scope in (1, 3):
            order_mode = prompt_order_mode()
            if order_mode == 5:
                mix_weights = prompt_mix_percentages()

        main(roots, outdir, sort_scope, primary_mode, instr_seq, order_mode, mix_weights, max_len_sec)

    except KeyboardInterrupt:
        print("\n🛑 Aborted by user.")
    except Exception as e:
        print(f"\n❌ Fatal error: {e}")
        traceback.print_exc(limit=2)
