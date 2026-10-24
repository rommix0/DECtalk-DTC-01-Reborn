"""The data behind the explainer video's pictures: the example texts, and loaders for what the tools in
videos/tools/ write (videos/PLAN.md)."""
import numpy as np

EXAMPLE = "Hello, I'm Perfect Paul."
LTS_WORD = "klatt"                      # spoken by letter-to-sound rules, not found in the dictionary

VOICES = ["np", "nb", "nh", "nf", "nk", "nr", "nu", "nd", "nv"]
VOICE_NAMES = {"np": "Perfect Paul", "nb": "Beautiful Betty", "nh": "Huge Harry", "nf": "Frail Frank",
               "nk": "Kit the Kid", "nr": "Rough Rita", "nu": "Uppity Ursula", "nd": "Doctor Dennis",
               "nv": "Variable Val"}
VOICE_LINES = {c: "I'm %s." % n for c, n in VOICE_NAMES.items()}

DSP_COLUMNS = ("n pcm lp tilt glot av_lin src noise np cf5 cf4 cf3 cf2 cf1 pf6 pf5 pf4 pf3 pf2 ab nper t0 nopen "
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
