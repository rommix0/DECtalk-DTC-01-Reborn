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


def test_dv_names_and_dsp_tables():
    """Paul's V record by name matches REFERENCE s8.1's ROM-verified column; the DSP tables read from the source
    have their ROM sizes."""
    _, _, v = dsplog("[:np] Hello.", "paul")
    paul = dict(zip(data.DV_NAMES, v))
    assert len(data.DV_NAMES) == 28
    assert (paul["ap"], paul["pr"], paul["hs"], paul["f4"], paul["ef"]) == (120, 100, 100, 3300, 100), paul
    assert len(data.dsp_table("dsp_b0")) == 224 and len(data.dsp_table("dsp_amptable")) == 88


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


# ---- Task 4: the narration script ----

SCENE_IDS = ["cold_open", "big_picture", "text_to_phonemes", "timing_pitch", "formants", "frame", "dsp_rhythm",
             "dsp_glottal", "dsp_noise", "dsp_cascade", "dsp_parallel", "dsp_dac", "voices", "outro"]


def outside_brackets(s):
    import re
    return re.sub(r"\[[^\]]*\]", " ", s)


def caption(line):
    return " ".join(outside_brackets(line.show or line.say).split())


def test_script_scenes():
    import script
    assert [s.id for s in script.SCENES] == SCENE_IDS


def test_script_say_is_speakable():
    import re
    import script
    for s in script.SCENES:
        for ln in s.lines:
            assert ln.say.isascii(), ln.say
            assert not re.search(r"\b(?=\w*[A-Za-z])(?=\w*\d)\w+\b", outside_brackets(ln.say)), ln.say


def test_script_word_count():
    import script
    n = sum(len(outside_brackets(ln.say).split()) for s in script.SCENES for ln in s.lines)
    assert 1150 <= n <= 1400, n


def test_script_captions_fit():
    import script
    for s in script.SCENES:
        for ln in s.lines:
            assert len(caption(ln)) <= 110, (len(caption(ln)), caption(ln))


def test_script_voices_only_in_voices():
    import script
    for s in script.SCENES:
        for ln in s.lines:
            assert s.id == "voices" or "[:n" not in ln.say, (s.id, ln.say)


# ---- Task 5: narration and the timeline (after `make_video.py narration`) ----

def test_timing_monotonic():
    t = mv.load_timing()
    at = 0.0
    for s in t["scenes"]:
        assert abs(s["start"] - at) < 1e-9 and s["end"] > s["start"], s["id"]
        prev = s["start"]
        for ln in s["lines"]:
            assert prev <= ln["start"] < ln["end"] <= s["end"], (s["id"], ln)
            prev = ln["end"]
        at = s["end"]
    assert abs(t["total"] - at) < 1e-9


def test_no_long_silences():
    """No pause longer than 3 s (the user): before the first line, between any two lines, after the last."""
    t = mv.load_timing()
    lines = [ln for s in t["scenes"] for ln in s["lines"]]
    gaps = [lines[0]["start"]] + [b["start"] - a["end"] for a, b in zip(lines, lines[1:])] + [t["total"] - lines[-1]["end"]]
    assert max(gaps) <= 3.0 + 1e-6, max(gaps)


def test_srt_matches_timing():
    import re
    lines = [ln for s in mv.load_timing()["scenes"] for ln in s["lines"]]
    srt = (mv.OUT / "captions.srt").read_text(encoding="utf-8")
    stamps = re.findall(r"(\d+):(\d\d):(\d\d),(\d{3}) --> (\d+):(\d\d):(\d\d),(\d{3})", srt)
    assert len(stamps) == len(lines), (len(stamps), len(lines))
    sec = lambda h, m, s, ms: int(h) * 3600 + int(m) * 60 + int(s) + int(ms) / 1000
    for st, ln in zip(stamps, lines):
        assert abs(sec(*st[:4]) - ln["start"]) <= 0.0015 and abs(sec(*st[4:]) - ln["end"]) <= 0.0015, (st, ln)


# ---- Task 6: the house style, and checks on every rendered scene ----

def probe(path):
    import json
    import subprocess
    r = subprocess.run(["ffprobe", "-v", "error", "-show_streams", "-show_format", "-of", "json", str(path)],
                       capture_output=True, text=True, check=True)
    return json.loads(r.stdout)


def duration(path):
    return float(probe(path)["format"]["duration"])


def freezes(path):
    """Stretches of 4 s or more where no pixel visibly changes (-80 dB: a pulsing label counts as motion)."""
    import subprocess
    r = subprocess.run(["ffmpeg", "-hide_banner", "-i", str(path), "-vf", "freezedetect=n=0.0001:d=4", "-map", "0:v",
                        "-f", "null", "-"], capture_output=True, text=True)
    return [ln for ln in r.stderr.splitlines() if "freeze_start" in ln]


def rendered_scenes():
    return [(s["id"], s["end"] - s["start"], mv.OUT / "scenes" / (s["id"] + ".mp4"))
            for s in mv.load_timing()["scenes"] if (mv.OUT / "scenes" / (s["id"] + ".mp4")).exists()]


def test_scene_durations():
    for sid, length, mp4 in rendered_scenes():
        assert abs(duration(mp4) - length) <= 1 / 30 + 1e-6, (sid, duration(mp4), length)


def test_no_freezes():
    for sid, _, mp4 in rendered_scenes():
        assert not freezes(mp4), (sid, freezes(mp4))


def test_paper_smoke():
    import subprocess
    import matplotlib.image
    from scenes.common import PAPER, Timeline
    mp4 = mv.render(Path(__file__), "SmokeScene", TMP / "smoke.mp4", "draft")
    assert abs(duration(mp4) - Timeline("frame").duration) <= 1 / 15 + 1e-6, (duration(mp4), Timeline("frame").duration)
    png = TMP / "smoke.png"
    subprocess.run(["ffmpeg", "-y", "-v", "error", "-i", str(mp4), "-frames:v", "1", str(png)], check=True)
    px = matplotlib.image.imread(str(png))[2, 2, :3] * 255
    want = [int(PAPER[i:i + 2], 16) for i in (1, 3, 5)]
    assert all(abs(a - b) <= 3 for a, b in zip(px, want)), (px, want)


# ---- Task 9: the pitch and formant logs (after `make_video.py data`) ----

def test_pitch_formant_data():
    p = data.load_pitchlog(mv.OUT / "data" / "pitch_example.tsv")
    f = data.load_formantlog(mv.OUT / "data" / "formant_example.tsv")
    assert p["F"] and f["F"], (len(p["F"]), len(f["F"]))
    assert [r["T0"] for r in p["F"]] == [r["T0"] for r in f["F"]]


# ---- Task 11: music (after `make_video.py music`) ----

def test_hum_clear_of_narration():
    import json
    import music
    talk = music.narration_intervals(mv.load_timing())
    hums = json.loads((mv.OUT / "hums.json").read_text())
    assert hums
    for a, b in hums:
        for c, d in talk:
            assert b + 0.5 <= c or a - 0.5 >= d, ((a, b), (c, d))


def test_pad_ducked():
    import music
    rate, x = read_wav(mv.OUT / "pad_48k.wav")
    assert rate == 48000
    x = x.astype(float)
    inside = np.zeros(len(x), bool)
    for c, d in music.narration_intervals(mv.load_timing()):
        inside[int(c * rate):int(d * rate)] = True
    rms = lambda v: np.sqrt(np.mean(v ** 2))
    assert 20 * np.log10(rms(x[inside]) / rms(x[~inside])) <= -15, 20 * np.log10(rms(x[inside]) / rms(x[~inside]))


# ---- Task 12: the finished video (after `make_video.py assemble`) ----

FINAL = mv.OUT / "dectalk_dtc01_v18_explainer.mp4"


def test_final_format():
    info = probe(FINAL)
    v = next(s for s in info["streams"] if s["codec_type"] == "video")
    a = next(s for s in info["streams"] if s["codec_type"] == "audio")
    assert (v["codec_name"], v["width"], v["height"], v["r_frame_rate"]) == ("h264", 1920, 1080, "30/1"), v
    assert (a["codec_name"], a["sample_rate"]) == ("aac", "48000"), a
    total = mv.load_timing()["total"]
    assert abs(float(info["format"]["duration"]) - total) <= 0.1, (info["format"]["duration"], total)
    assert abs(float(v["duration"]) - float(a["duration"])) <= 0.1, (v["duration"], a["duration"])


def test_final_peak():
    import re
    import subprocess
    r = subprocess.run(["ffmpeg", "-hide_banner", "-i", str(FINAL), "-map", "0:a", "-af", "volumedetect", "-f", "null",
                        "-"], capture_output=True, text=True)
    peak = float(re.search(r"max_volume: (-?[\d.]+) dB", r.stderr).group(1))
    assert peak <= -1.0, peak


def test_final_no_freezes():
    assert FINAL.exists(), FINAL
    assert not freezes(FINAL), freezes(FINAL)


try:                                    # the smoke test's scene; manim renders it from this file
    from scenes.common import PaperScene, box

    class SmokeScene(PaperScene):
        scene_id = "frame"

        def construct(self):
            self.add(box("F1", "700"))
            self.finish()
except ImportError:
    pass


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
