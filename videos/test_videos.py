"""Checks for the explainer video's pipeline (videos/PLAN.md). Plain asserts, no pytest:

    python videos/test_videos.py            every test
    python videos/test_videos.py name ...   only these
"""
import sys
import tempfile
import traceback
import wave
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import make_video as mv
import data

TMP = Path(tempfile.mkdtemp(prefix="videos_test_"))


def read_wav(path):
    """(rate, int16 samples) of a mono 16-bit wave; asserts the format."""
    with wave.open(str(path)) as w:
        assert w.getnchannels() == 1 and w.getsampwidth() == 2, (w.getnchannels(), w.getsampwidth())
        return w.getframerate(), np.frombuffer(w.readframes(w.getnframes()), dtype="<i2")


# ---- Task 1: narrate ----

def test_narrate_format():
    secs = mv.run_narrate("Hello.", TMP / "hello.wav")
    rate, x = read_wav(TMP / "hello.wav")
    assert rate == 10000, rate
    assert 0.2 < secs < 2.0 and abs(secs - len(x) / rate) < 1e-9, (secs, len(x))


def test_narrate_deterministic():
    mv.run_narrate("Hello.", TMP / "a.wav")
    mv.run_narrate("Hello.", TMP / "b.wav")
    assert (TMP / "a.wav").read_bytes() == (TMP / "b.wav").read_bytes()


def test_narrate_lead_not_written():
    with_lead = mv.run_narrate("Hello.", TMP / "lead.wav")
    in_text = mv.run_narrate(mv.GREETING + " Hello.", TMP / "nolead.wav", lead="")
    assert with_lead < in_text - 1.0, (with_lead, in_text)


def test_no_rom_paths():
    here = Path(__file__).resolve().parent
    bad = ("merges", "dectalk_v1.8_full", "dsp_v1.8")
    for p in here.rglob("*"):
        if not p.is_file() or p.suffix == ".md" or "out" in p.relative_to(here).parts or p == Path(__file__).resolve():
            continue
        text = p.read_text(encoding="utf-8", errors="replace")
        assert not any(b in text for b in bad), p


# ---- Task 2: dsplog ----

def dsplog(text, name, lead=mv.GREETING):
    tsv = TMP / (name + ".tsv")
    mv.run_tool("dsplog", tsv, text, lead)
    return data.load_dsplog(tsv)


def test_dsplog_matches_narration():
    s, _, _ = dsplog(data.EXAMPLE, "example")
    mv.run_narrate(data.EXAMPLE, TMP / "example.wav")
    _, x = read_wav(TMP / "example.wav")
    assert len(s["pcm"]) == len(x), (len(s["pcm"]), len(x))
    assert np.array_equal(s["pcm"], x)


def test_dsplog_taps_rebuild_sample():
    s, _, _ = dsplog(data.EXAMPLE, "example")
    i16 = data.i16
    for k in range(len(s["pcm"])):
        out = i16(2 * int(s["cf1"][k]))
        for p in ("pf6", "pf5", "pf4", "pf3", "pf2"):
            out = i16(int(s[p][k]) - out)
        out = i16(out - i16((int(s["ab"][k]) * int(s["noise"][k])) >> 15))
        assert out & 0xFFF0 == int(s["pcm"][k]) & 0xFFF0, k


def test_dsplog_phones():
    s, p, _ = dsplog(data.EXAMPLE, "example")
    n = [r["n"] for r in p]
    assert n and all(a < b for a, b in zip(n, n[1:])) and n[-1] < len(s["pcm"]), n


def test_voices():
    v = {c: dsplog("[:%s] %s" % (c, data.VOICE_LINES[c]), "voice_" + c)[2] for c in data.VOICES}
    seven = ["np", "nb", "nh", "nf", "nk", "nr", "nu"]
    assert all(len(v[c]) == 28 for c in data.VOICES)
    assert len({tuple(v[c]) for c in seven}) == 7, v
    assert v["nd"] == v["nf"] and v["nv"] == v["np"]


# ---- Task 3: ltslog ----

def ltslog():
    tsv = TMP / "lts.tsv"
    if not tsv.exists():
        mv.run_ltslog(tsv, ["hello", data.LTS_WORD])
    return data.load_ltslog(tsv)


def test_ltslog_dictionary_word():
    w = ltslog()["hello"]
    assert w["dict"] == 1 and not w["rules"], w


def test_ltslog_rule_word():
    w = ltslog()[data.LTS_WORD]
    assert w["dict"] == 0 and len(w["rules"]) >= 3, w
    assert all(1 <= r <= 373 for r, _, _ in w["rules"]) and w["phonemes"].strip(), w


def main(names):
    tests = {k: v for k, v in globals().items() if k.startswith("test_") and callable(v)}
    failed = 0
    for name in names or tests:
        try:
            tests[name]()
            print("PASS", name)
        except Exception:
            failed += 1
            print("FAIL", name)
            traceback.print_exc()
    print(f"{len(names or tests) - failed}/{len(names or tests)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
