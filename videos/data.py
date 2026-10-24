"""The data behind the explainer video's pictures: the example texts, and loaders for what the tools in
videos/tools/ write (videos/PLAN.md)."""
import numpy as np

EXAMPLE = "Hello, I'm Perfect Paul."
LTS_WORD = "klatt"                      # spoken by letter-to-sound rules, not found in the dictionary
# the rule table's sections, numbered from 0 as REFERENCE s15.10 numbers them (ltslog's R records count from 1)
LTS_SECTIONS = [(0, 35, "suffix stripping"), (36, 91, "consonants"), (92, 205, "suffixes"), (206, 231, "prefixes"),
                (232, 262, "vowel pairs"), (263, 335, "single vowels"), (336, 361, "stress"), (362, 372, "cleanup")]

VOICES = ["np", "nb", "nh", "nf", "nk", "nr", "nu", "nd", "nv"]
VOICE_NAMES = {"np": "Perfect Paul", "nb": "Beautiful Betty", "nh": "Huge Harry", "nf": "Frail Frank",
               "nk": "Kit the Kid", "nr": "Rough Rita", "nu": "Uppity Ursula", "nd": "Doctor Dennis",
               "nv": "Variable Val"}
VOICE_LINES = {c: "I'm %s." % n for c, n in VOICE_NAMES.items()}

# the 28 [:dv] values of a voice, in its record's order (REFERENCE s8.1), and what the video calls the main ones
DV_NAMES = ("sex sm as ap pr br ri nf la hs f4 b4 f5 b5 p4 p5 gf gh gv gn g1 g2 g3 g4 g5 ft bf ef").split()
DV_SHOWN = {"sex": "sex (1 male)", "ap": "average pitch, Hz", "pr": "pitch range, %", "hs": "head size, %",
            "br": "breathiness, dB", "ri": "richness, %", "la": "laryngealization, %", "sm": "smoothness, %",
            "f4": "F4, Hz", "f5": "F5, Hz"}

DSP_COLUMNS =("n pcm lp tilt glot av_lin src noise np cf5 cf4 cf3 cf2 cf1 pf6 pf5 pf4 pf3 pf2 ab nper t0 nopen "
               "f1 f2 f3 fnz").split()


def _sym_names():
    """The 121 symbol codes by name, from sym_table in the generated source (src/speech/tx_rom.c): phonemes 0-55
    (REFERENCE s8.3), stress and punctuation, the letters coded 70-79 (REFERENCE s15.10), and the rest."""
    import re
    from pathlib import Path
    src = (Path(__file__).resolve().parent.parent / "src" / "speech" / "tx_rom.c").read_text()
    table = src[src.index("sym_table[121]"):]
    names = re.findall(r'\{ *(?:\'(?:\\.|[^\'])\'|\d+), *\d+, *(?:"((?:\\.|[^"\\])*)"|NULL)', table)[:121]
    return [n.replace('\\"', '"') for n in names]


SYM_NAMES = _sym_names()


def lts_code_name(code):
    """An LTS list item by name; the word boundary (62, a blank in sym_table) shows as '#'."""
    if code == 62:
        return "#"
    return SYM_NAMES[code] if 0 <= code < len(SYM_NAMES) else str(code)


def load_ltslog(path):
    """ltslog's records by word: dict (0/1), pron, rules [(rule, pos, codes)], phonemes."""
    words = {}
    with open(path) as f:
        for line in f:
            r = line.rstrip("\n").split("\t")
            if r[0] == "W":
                words[r[1]] = {"dict": int(r[2]), "pron": r[3], "rules": [], "phonemes": ""}
            elif r[0] == "R":
                words[r[1]]["rules"].append((int(r[2]), int(r[3]), [int(c) for c in r[4].split(",") if c]))
            elif r[0] == "X":
                words[r[1]]["phonemes"] = r[2] if len(r) > 2 else ""
    return words


PITCH_F = "frame f0_tenths f0 f0baseline tarhat tarimp f0seg f0in phone stress f0mode T0 AV dip smoothed".split()
FORMANT_F = "frame T0 F1 F2 F3 FNZ B1 B2 B3 AV AH A2 A3 A4 A5 A6 AB TLT".split()
FORMANT_TRACKS = "F1 F2 F3 FNZ B1 B2 B3".split()
TRACK_FIELDS = "table tarlas tarcur tarend tarnex fbou fdur bbou bdur breaks".split()


def load_pitchlog(path):
    """pitch_graphs/pitchlog.c's records: {"F": frames, "E": F0 commands, "P": phones, "C": clauses}."""
    r = {"F": [], "E": [], "P": [], "C": []}
    for line in open(path):
        p = line.rstrip("\n").split("\t")
        if p[0] == "F":
            r["F"].append(dict(zip(PITCH_F, map(int, p[1:]))))
        elif p[0] == "E":
            r["E"].append({"frame": int(p[1]), "command": int(p[2])})
        elif p[0] == "P":
            r["P"].append({"frame": int(p[1]), "name": p[2], "duration": int(p[3]), "allofeats": int(p[4])})
        elif p[0] == "C":
            r["C"].append({"frame": int(p[1]), "clause": int(p[2])})
    return r


def load_formantlog(path):
    """formants_graphs/formantlog.c's records: {"F": frames (the 17 words, and per F1-F3 the terms tar, dip, fwd,
    bwd, special), "P": phones (with each track's set-up), "C": clauses}."""
    r = {"F": [], "P": [], "C": []}
    for line in open(path):
        p = line.rstrip("\n").split("\t")
        if p[0] == "F":
            f = dict(zip(FORMANT_F, map(int, p[1:19])))
            for name, terms in zip(("F1", "F2", "F3"), p[19:22]):
                f[name + "_terms"] = dict(zip("tar dip fwd bwd special".split(), map(int, terms.split(","))))
            r["F"].append(f)
        elif p[0] == "P":
            ph = {"frame": int(p[1]), "name": p[2], "duration": int(p[3]), "allofeats": int(p[4]), "sex": int(p[5])}
            for name, fields in zip(FORMANT_TRACKS, p[6:13]):
                v = fields.split(",")
                ph[name] = dict(zip(TRACK_FIELDS, [int(x) for x in v[:9]] + [v[9]]))
            r["P"].append(ph)
        elif p[0] == "C":
            r["C"].append({"frame": int(p[1]), "clause": int(p[2])})
    return r


def dsp_table(name):
    """A table of the DSP program ROM as the generated source holds it (src/speech/dsp_rom.c)."""
    import re
    from pathlib import Path
    src = (Path(__file__).resolve().parent.parent / "src" / "speech" / "dsp_rom.c").read_text()
    body = re.search(r"\b%s\[\d+\] = \{(.*?)\};" % name, src, re.S).group(1)
    return np.array([int(v, 0) for v in re.sub(r"/\*.*?\*/", "", body, flags=re.S).replace(",", " ").split()])


def spectrum_db(x, n=512):
    """(Hz, dB) of a Hann-windowed FFT of n samples at 10 kHz, the peak at 0 dB."""
    x = np.asarray(x[:n], dtype=float)
    mag = np.abs(np.fft.rfft((x - x.mean()) * np.hanning(len(x))))
    db = 20 * np.log10(mag / (mag.max() or 1.0) + 1e-9)
    return np.fft.rfftfreq(len(x), 1 / 10000), db


def i16(x):
    """x wrapped to a 16-bit two's-complement value, as the DSP's (int16_t) casts do."""
    return ((x + 0x8000) & 0xFFFF) - 0x8000


def load_dsplog(path):
    """dsplog's records: (S columns as int arrays, P records as dicts, the V record's 28 values)."""
    rows, phones, voice = [], [], []
    with open(path) as f:
        for line in f:
            r = line.rstrip("\n").split("\t")
            if r[0] == "S":
                rows.append([int(v) for v in r[1:]])
            elif r[0] == "P":
                phones.append({"n": int(r[1]), "code": int(r[2]), "samples": int(r[3]), "name": r[4]})
            elif r[0] == "V":
                voice = [int(v) for v in r[1:]]
    a = np.array(rows, dtype=np.int64).reshape(-1, len(DSP_COLUMNS))
    return {c: a[:, i] for i, c in enumerate(DSP_COLUMNS)}, phones, voice
