"""The explainer video's music (videos/DESIGN.md, PLAN.md Task 11): a quiet synth pad in C under the whole video,
ducked under every narration line, and Perfect Paul humming with v1.8's singing syntax in the gaps: a phrase before
the greeting and a three-note motif at each new section. The gaps are at most 3 s (the user), so the hums are short."""
import json
import subprocess
import wave

import numpy as np

import make_video as mv

RATE = 48000
PAD_DBFS = -18.0                        # the pad's RMS in the gaps
DUCK_DB = 18.0                          # how far it drops under Paul
RAMP = 0.3                              # seconds
CLEAR = 0.5                             # a hum keeps this far from any narration
HUM_PEAK = 0.35                         # of full scale

# notes 1-37 = C2-C5 (REFERENCE s15.14): 20 G3, 25 C4, 29 E4, 32 G4
HUM_INTRO = "[m<400,20>m<400,25>m<400,29>m<800,32>]"
HUM_MOTIF = "[m<250,25>m<250,29>m<600,32>]"
SECTIONS = ["text_to_phonemes", "timing_pitch", "formants", "frame", "dsp_rhythm", "voices", "outro"]
CHORDS = [(48, 52, 55, 59), (45, 48, 52, 55), (41, 45, 48, 52), (43, 47, 50, 52)]  # Cmaj7 Am7 Fmaj7 G6 (MIDI)
CHORD_SECONDS = 6.0


def narration_intervals(timing):
    return [(ln["start"], ln["end"]) for s in timing["scenes"] for ln in s["lines"]]


def pad(total, talk):
    """The pad: four soft chords in turn, each note a sine with a quieter octave and a slow swell, then ducked."""
    t = np.arange(int(total * RATE)) / RATE
    x = np.zeros_like(t)
    for k, chord in enumerate(CHORDS * int(total / (CHORD_SECONDS * len(CHORDS)) + 1)):
        a = k * CHORD_SECONDS
        if a >= total:
            break
        i0, i1 = int(a * RATE), min(len(t), int((a + CHORD_SECONDS + 1.0) * RATE))
        tt = t[i0:i1] - a
        swell = np.minimum(1, tt / 1.0) * np.minimum(1, np.maximum(0, (CHORD_SECONDS + 1.0 - tt) / 1.0))
        for m in chord:
            f = 440 * 2 ** ((m - 69) / 12)
            x[i0:i1] += swell * (np.sin(2 * np.pi * f * tt) + 0.25 * np.sin(2 * np.pi * 2.003 * f * tt))
    # the ducking gain, worked out at 100 Hz: low during a line, back to 1 over RAMP seconds either side of it
    tc = np.arange(0, total + 0.01, 0.01)
    dist = np.full(len(tc), np.inf)
    for c, d in talk:
        dist = np.minimum(dist, np.maximum(0, np.maximum(c - tc, tc - d)))
    low = 10 ** (-DUCK_DB / 20)
    env = np.interp(t, tc, low + (1 - low) * np.minimum(1, dist / RAMP))
    x *= env
    gaps = env > 0.99
    x *= 10 ** (PAD_DBFS / 20) / np.sqrt(np.mean(x[gaps] ** 2))
    return x


def hum_48k(text, name):
    """A hummed line, rendered by narrate (exact v1.8) and resampled to 48 kHz by ffmpeg."""
    w10 = mv.OUT / "music" / (name + "_10k.wav")
    w48 = mv.OUT / "music" / (name + "_48k.wav")
    mv.run_narrate(text, w10)
    subprocess.run(["ffmpeg", "-y", "-v", "error", "-i", str(w10), "-ar", "48000", str(w48)], check=True)
    with wave.open(str(w48)) as w:
        x = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").astype(float)
    return x / 32768 * HUM_PEAK / (np.abs(x).max() / 32768)


def hum_spots(timing, intro_len, motif_len):
    """Where the hums go: the intro before the first line, a motif centred in the gap before each section."""
    spots = [(max(0.0, timing["scenes"][0]["lines"][0]["start"] - CLEAR - intro_len), "intro")]
    scenes = timing["scenes"]
    for prev, s in zip(scenes, scenes[1:]):
        if s["id"] in SECTIONS:
            a, b = prev["lines"][-1]["end"] + CLEAR, s["lines"][0]["start"] - CLEAR
            if b - a >= motif_len:
                spots.append(((a + b - motif_len) / 2, "motif"))
    return spots


def make_music():
    timing = mv.load_timing()
    total, talk = timing["total"], narration_intervals(timing)
    p = pad(total, talk)
    intro, motif = hum_48k(HUM_INTRO, "intro"), hum_48k(HUM_MOTIF, "motif")
    x, hums = p.copy(), []
    for at, kind in hum_spots(timing, len(intro) / RATE, len(motif) / RATE):
        h = intro if kind == "intro" else motif
        i = int(at * RATE)
        n = min(len(h), len(x) - i)
        x[i:i + n] += h[:n]
        hums.append((at, at + n / RATE))
    (mv.OUT / "hums.json").write_text(json.dumps(hums))
    for name, y in (("pad_48k.wav", p), ("music_48k.wav", x)):
        with wave.open(str(mv.OUT / name), "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(RATE)
            w.writeframes((np.clip(y, -1, 1) * 32767).astype("<i2").tobytes())
    print("music: %d hums, %.1f s" % (len(hums), total))
