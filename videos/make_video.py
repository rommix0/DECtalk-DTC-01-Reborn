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


def not_yet():
    raise SystemExit("stage not built yet")


STAGES = {"tools": build_tools, "data": make_data, "narration": not_yet, "scenes": not_yet, "music": not_yet,
          "assemble": not_yet}


def main(argv):
    if not argv or argv[0] not in list(STAGES) + ["all"]:
        raise SystemExit(__doc__)
    for name in STAGES if argv[0] == "all" else [argv[0]]:
        STAGES[name]()


if __name__ == "__main__":
    main(sys.argv[1:])
