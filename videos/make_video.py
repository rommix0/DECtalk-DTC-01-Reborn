"""The DECtalk DTC-01 v1.8 explainer video (videos/DESIGN.md, videos/PLAN.md), built in stages:

    python videos/make_video.py tools|data|narration|scenes|music|assemble|all

Everything heard and shown comes from the speech engine in src/ with all of the library's fixes off (exact v1.8),
through the small tools in videos/tools/. Generated files go to videos/out/ and build/videos/.
"""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VIDEOS = ROOT / "videos"
OUT = VIDEOS / "out"
BUILD = ROOT / "build" / "videos"
GREETING = "DECtalk version 1.8 is running."
TOOLS = ("narrate", "dsplog", "ltslog")


def exe(name):
    return BUILD / (name + ".exe")


def build_tools():
    """Build the C tools (videos/tools/build_tools.bat) when one is missing or older than its source."""
    src = VIDEOS / "tools"
    if all(exe(t).exists() and exe(t).stat().st_mtime >= (src / (t + ".c")).stat().st_mtime for t in TOOLS):
        return
    subprocess.run(["cmd", "/c", str(src / "build_tools.bat")], check=True)
    missing = [t for t in TOOLS if not exe(t).exists()]
    if missing:
        raise SystemExit("build_tools.bat did not build " + ", ".join(missing))


def run_tool(name, out, text, lead=GREETING):
    """Run one of the tools (out file, text, optional lead-in)."""
    Path(out).parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([str(exe(name)), str(out), text] + ([lead] if lead else []), check=True)


def run_ltslog(out, words):
    """ltslog: dictionary lookups and letter-to-sound rules for these (lower-case) words."""
    Path(out).parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([str(exe("ltslog")), str(out)] + list(words), check=True)


def run_narrate(text, out_wav, lead=GREETING):
    """Speak text into a 10 kHz wave (after the unrecorded lead-in); its length in seconds."""
    out_wav = Path(out_wav)
    out_wav.parent.mkdir(parents=True, exist_ok=True)
    args = [str(exe("narrate")), str(out_wav), text] + ([lead] if lead else [])
    subprocess.run(args, check=True)
    with open(out_wav, "rb") as f:
        head = f.read(44)
    return int.from_bytes(head[40:44], "little") / 2 / 10000


def make_data():
    """The tools' logs the scenes draw from, in out/data/."""
    import data
    build_tools()
    d = OUT / "data"
    run_tool("dsplog", d / "dsp_example.tsv", data.EXAMPLE)
    for c in data.VOICES:
        run_tool("dsplog", d / ("voice_%s.tsv" % c), "[:%s] %s" % (c, data.VOICE_LINES[c]))
    run_ltslog(d / "lts.tsv", ["hello", "i'm", "perfect", "paul", data.LTS_WORD])
    for name, folder in (("pitchlog", "pitch_graphs"), ("formantlog", "formants_graphs")):
        tool = ROOT / "build" / name / (name + ".exe")
        if not tool.exists():
            subprocess.run(["cmd", "/c", str(ROOT / folder / ("build_%s.bat" % name))], check=True)
        out = d / ("%s_example.tsv" % name.replace("log", ""))
        subprocess.run([str(tool), str(out), data.EXAMPLE, GREETING], check=True)


def caption(line):
    """A line's caption: its show text, or what Paul says without the phonemic [...] parts."""
    import re
    return " ".join(re.sub(r"\[[^\]]*\]", " ", line.show or line.say).split())


def srt_time(t):
    ms = round(t * 1000)
    return "%02d:%02d:%02d,%03d" % (ms // 3600000, ms // 60000 % 60, ms // 1000 % 60, ms % 1000)


def make_narration():
    """Every line to its own wave (out/narration/), then the timeline that fixes each scene's length
    (out/timing.json), the captions (out/captions.srt) and the whole narration track at 10 kHz."""
    import json
    import wave
    from concurrent.futures import ThreadPoolExecutor
    import numpy as np
    from script import SCENES
    build_tools()
    jobs = [(s, k, ln, OUT / "narration" / ("%s_%02d.wav" % (s.id, k))) for s in SCENES for k, ln in enumerate(s.lines)]
    with ThreadPoolExecutor(8) as pool:
        secs = list(pool.map(lambda j: run_narrate(j[2].say, j[3], "" if j[2].boot else GREETING), jobs))
    dur = {(j[0].id, j[1]): d for j, d in zip(jobs, secs)}

    # a scene starts where the last ended; its first line after the lead; each next line after the last one's
    # pause; it ends its tail after the last line (no pause over 3 s: the user)
    scenes, at = [], 0.0
    for s in SCENES:
        t, lines = at + s.lead, []
        for k, ln in enumerate(s.lines):
            end = t + dur[(s.id, k)]
            lines.append({"start": t, "end": end, "wav": "narration/%s_%02d.wav" % (s.id, k), "caption": caption(ln)})
            t = end + ln.pause
        end = lines[-1]["end"] + s.tail
        scenes.append({"id": s.id, "start": at, "end": end, "lines": lines})
        at = end
    timing = {"total": at, "scenes": scenes}
    (OUT / "timing.json").write_text(json.dumps(timing, indent=1))

    entries = [ln for s in scenes for ln in s["lines"]]
    (OUT / "captions.srt").write_text("".join("%d\n%s --> %s\n%s\n\n" % (i + 1, srt_time(ln["start"]),
                                      srt_time(ln["end"]), ln["caption"]) for i, ln in enumerate(entries)),
                                      encoding="utf-8")
    track = np.zeros(int(round(at * 10000)) + 1, dtype=np.int16)
    for ln in entries:
        with wave.open(str(OUT / ln["wav"])) as w:
            x = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2")
        i = int(round(ln["start"] * 10000))
        track[i:i + len(x)] = x
    with wave.open(str(OUT / "narration_10k.wav"), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(10000)
        w.writeframes(track.astype("<i2").tobytes())
    print("narration: %d lines, %.1f s" % (len(entries), at))


def load_timing():
    import json
    return json.loads((OUT / "timing.json").read_text())


SCENE_MODULES = {"cold_open": "bookends", "big_picture": "bookends", "outro": "bookends",
                 "text_to_phonemes": "pipeline", "timing_pitch": "pipeline", "formants": "pipeline",
                 "frame": "pipeline"}        # the rest are in scenes/dsp.py


def render(module_path, cls, out_mp4, quality="final"):
    """Render one Manim scene class to out_mp4: 1920x1080 at 30 fps ("final") or manim's -ql ("draft")."""
    import shutil
    media = OUT / "media"
    q = ["-r", "1920,1080", "--fps", "30"] if quality == "final" else ["-ql"]
    subprocess.run([sys.executable, "-m", "manim", "render", *q, "--disable_caching", "--media_dir", str(media),
                    "-o", cls, str(module_path), cls], check=True, cwd=str(VIDEOS))
    made = max(media.glob("videos/%s/*/%s.mp4" % (Path(module_path).stem, cls)), key=lambda p: p.stat().st_mtime)
    Path(out_mp4).parent.mkdir(parents=True, exist_ok=True)
    shutil.move(str(made), str(out_mp4))
    return Path(out_mp4)


def render_scene(scene_id, quality="final"):
    from script import scene_class
    module = VIDEOS / "scenes" / (SCENE_MODULES.get(scene_id, "dsp") + ".py")
    return render(module, scene_class(scene_id), OUT / "scenes" / (scene_id + ".mp4"), quality)


def make_scenes(ids=None):
    from script import SCENES
    for sid in ids or [s.id for s in SCENES]:
        render_scene(sid)


def make_music():
    import music
    music.make_music()


def assemble():
    """The scenes in order, Paul's narration (resampled to 48 kHz only here) and the music, into the finished MP4,
    its peak at -1 dBFS, with the captions beside it."""
    import shutil
    import wave
    import numpy as np
    from script import SCENES
    lst = OUT / "scenes.txt"
    lst.write_text("".join("file '%s'\n" % (OUT / "scenes" / (s.id + ".mp4")).as_posix() for s in SCENES))
    video = OUT / "video_only.mp4"
    subprocess.run(["ffmpeg", "-y", "-v", "error", "-f", "concat", "-safe", "0", "-i", str(lst), "-c", "copy",
                    str(video)], check=True)
    n48 = OUT / "narration_48k.wav"
    subprocess.run(["ffmpeg", "-y", "-v", "error", "-i", str(OUT / "narration_10k.wav"), "-ar", "48000", str(n48)],
                   check=True)

    def read(p):
        with wave.open(str(p)) as w:
            return np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").astype(float) / 32768
    voice, music_ = read(n48), read(OUT / "music_48k.wav")
    n = max(len(voice), len(music_))
    mix = np.zeros(n)
    mix[:len(voice)] += voice
    mix[:len(music_)] += music_
    mix *= 10 ** (-1.5 / 20) / np.abs(mix).max()            # -1.5 dBFS: AAC may overshoot a little; -1 dBFS is the test
    mixed = OUT / "mix_48k.wav"
    with wave.open(str(mixed), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(48000)
        w.writeframes((mix * 32767).astype("<i2").tobytes())
    final = OUT / "dectalk_dtc01_v18_explainer.mp4"
    subprocess.run(["ffmpeg", "-y", "-v", "error", "-i", str(video), "-i", str(mixed), "-map", "0:v", "-map", "1:a",
                    "-c:v", "copy", "-c:a", "aac", "-b:a", "192k", "-ar", "48000", "-shortest", str(final)], check=True)
    shutil.copy(OUT / "captions.srt", OUT / "dectalk_dtc01_v18_explainer.srt")
    print("assembled:", final)


STAGES = {"tools": build_tools, "data": make_data, "narration": make_narration, "scenes": make_scenes, "music": make_music,
          "assemble": assemble}


def main(argv):
    if not argv or argv[0] not in list(STAGES) + ["all"]:
        raise SystemExit(__doc__)
    if argv[0] == "scenes" and argv[1:]:
        return make_scenes(argv[1:])
    for name in STAGES if argv[0] == "all" else [argv[0]]:
        STAGES[name]()


if __name__ == "__main__":
    main(sys.argv[1:])
