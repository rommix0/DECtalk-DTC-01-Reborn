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
