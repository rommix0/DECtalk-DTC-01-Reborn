"""The data behind the explainer video's pictures: the example texts, and loaders for what the tools in
videos/tools/ write (videos/PLAN.md)."""
import numpy as np

EXAMPLE = "Hello, I'm Perfect Paul."

VOICES = ["np", "nb", "nh", "nf", "nk", "nr", "nu", "nd", "nv"]
VOICE_NAMES = {"np": "Perfect Paul", "nb": "Beautiful Betty", "nh": "Huge Harry", "nf": "Frail Frank",
               "nk": "Kit the Kid", "nr": "Rough Rita", "nu": "Uppity Ursula", "nd": "Doctor Dennis",
               "nv": "Variable Val"}
VOICE_LINES = {c: "I'm %s." % n for c, n in VOICE_NAMES.items()}

DSP_COLUMNS = ("n pcm lp tilt glot av_lin src noise np cf5 cf4 cf3 cf2 cf1 pf6 pf5 pf4 pf3 pf2 ab nper t0 nopen "
               "f1 f2 f3 fnz").split()


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
