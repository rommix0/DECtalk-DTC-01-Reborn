"""The 68000's side: the explainer video's scenes from text to the DSP frame (REFERENCE.md s15.24-15.30 for the text,
PITCH_SYSTEM.md, FORMANT_SYSTEM.md and SMOOTHING_SYSTEM.md for the phonetic component). Data: ltslog, pitchlog and
formantlog for the example sentence (videos/data.py)."""
import sys
from pathlib import Path

import numpy as np
from manim import (DOWN, LEFT, RIGHT, UP, Arrow, Create, DashedLine, Dot, FadeIn, FadeOut, LaggedStart, Line,
                   Rectangle, Transform, VGroup, Write)

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from scenes.common import BLUE, INK, INK2, LINE_W, RED, PaperScene, box, header   # noqa: E402
from scenes.dsp import label, polyline                          # noqa: E402
import data                                                                       # noqa: E402
import make_video as mv                                                           # noqa: E402

WORDS = ["hello", "i'm", "perfect", "paul"]


class Axes(VGroup):
    """A frame-numbered x axis over [0, frames) and a y axis over [y0, y1], in a ruled panel."""

    def __init__(self, frames, y0, y1, width, height, at, ylabel, yticks):
        super().__init__()
        self.frames, self.y0, self.y1 = frames, y0, y1
        self.box = Rectangle(width=width, height=height, stroke_color=INK, stroke_width=1).move_to(at)
        self.add(self.box)
        for v in yticks:
            self.add(label(str(v), 14).move_to([self.box.get_left()[0] - 0.3, self.y(v), 0]))
        self.add(label(ylabel, 14).next_to(self.box, UP, buff=0.05).align_to(self.box, LEFT))

    def x(self, frame):
        return self.box.get_left()[0] + self.box.width * frame / self.frames

    def y(self, v):
        return self.box.get_bottom()[1] + self.box.height * (v - self.y0) / (self.y1 - self.y0)

    def curve(self, frames, values, color=RED, width=LINE_W):
        return polyline([self.x(f) for f in frames], [self.y(v) for v in values], color, width)

    def steps(self, frames, values, color=INK2, width=LINE_W):
        """A step function: values[i] from frames[i] to frames[i + 1]."""
        xs, ys = [], []
        for i, v in enumerate(values[:-1]):
            xs += [frames[i], frames[i + 1]]
            ys += [v, v]
        return self.curve(xs, ys, color, width)

    def phones(self, plist):
        """Phone names along the bottom, at their first frame."""
        return VGroup(*(label(p["name"], 13, INK2).move_to([self.x(p["frame"] + p["duration"] / 2),
                                                             self.box.get_bottom()[1] + 0.18, 0])
                        for p in plist if p["name"] != "_"))


class TextToPhonemes(PaperScene):
    """Letters to phonemes: the built-in dictionary, then the 373 letter-to-sound rules (tx_lts.c, REFERENCE s15.10)."""
    scene_id = "text_to_phonemes"

    def construct(self):
        T = self.timeline
        lts = data.load_ltslog(mv.OUT / "data" / "lts.tsv")
        self.play(FadeIn(header("Letters to phonemes", "dttask on the 68000 · the dictionary first, then 373 rules")),
                  run_time=1.0)
        words = VGroup(*(label(w, 40, INK) for w in ("Hello,", "I'm", "Perfect", "Paul."))).arrange(RIGHT, buff=0.6)
        words.move_to([0, 1.7, 0])
        self.sync(T.line(0)[0])
        self.play(Write(words), run_time=2.0)
        self.pulse_until(T.line(1)[0], list(words), color=BLUE)

        dic = box("DICT", "in ROM").scale(0.9).move_to([-4.6, -0.4, 0])
        self.play(FadeIn(dic, shift=UP * 0.2), run_time=0.8)
        prons = VGroup()
        self.sync(T.line(2)[0])
        for w, m in zip(WORDS[:3], words[:3]):
            pr = label(lts[w]["pron"], 26, BLUE).next_to(m, DOWN, buff=0.35)
            prons.add(pr)
            self.play(m.animate.set_color(BLUE), Create(Arrow(dic[0].get_top(), pr.get_bottom(), buff=0.1, color=BLUE,
                                                              stroke_width=1.5)), FadeIn(pr), run_time=1.2)
        self.sync(T.line(3)[0])
        rules_box = box("LTS", "373 rules").scale(0.9).move_to([4.6, -0.4, 0])
        self.play(words[3].animate.set_color(RED), FadeIn(rules_box, shift=UP * 0.2), run_time=1.0)
        self.play(Create(Arrow(words[3].get_bottom(), rules_box[0].get_top(), buff=0.1, color=RED, stroke_width=1.5)),
                  run_time=0.8)

        # the table: one tick per rule, in ROM order
        self.sync(T.line(4)[0])
        self.play(FadeOut(VGroup(*[m for m in self.mobjects if isinstance(m, Arrow)])), FadeOut(prons), run_time=0.6)
        x0, w = -6.2, 12.4
        ticks = VGroup(*(Line([x0 + w * i / 372, -1.75, 0], [x0 + w * i / 372, -1.45, 0], color=INK2, stroke_width=1)
                         for i in range(373)))
        cite = label("Hunnicutt 1976, re-encoded by DEC: 373 rules, tried in ROM order", 18).move_to([0, -1.2, 0])
        self.play(LaggedStart(*(Create(t) for t in ticks), lag_ratio=0.004), FadeIn(cite), run_time=2.5)
        self.pulse_until(T.line(5)[0], [cite])

        cursor = Line([x0, -1.9, 0], [x0, -1.3, 0], color=RED, stroke_width=3)
        self.sync(T.line(5)[0])
        self.add(cursor)
        self.play(cursor.animate.shift(RIGHT * w), run_time=3.2)

        self.sync(T.line(6)[0])
        secs = VGroup()
        for a, b, name in data.LTS_SECTIONS:
            xa, xb = x0 + w * a / 372, x0 + w * b / 372
            secs.add(VGroup(Line([xa, -2.0, 0], [xb, -2.0, 0], color=BLUE, stroke_width=3),
                            label(name, 13, BLUE).move_to([(xa + xb) / 2, -2.25 - 0.22 * (len(secs) % 2), 0])))
        self.play(FadeOut(cite), LaggedStart(*(FadeIn(s) for s in secs), lag_ratio=0.3), run_time=4.0)
        self.pulse_until(T.line(7)[0], [s[0] for s in secs])

        # "klatt": the list before and after each rule that changes it
        self.sync(T.line(7)[0])
        self.play(FadeOut(VGroup(words, dic, rules_box)), run_time=0.6)
        k = lts[data.LTS_WORD]
        names = ["#"] + [("A" if ch == "a" else ch) for ch in data.LTS_WORD] + ["#"]
        row = label("  ".join(names), 34, INK).move_to([0, 1.6, 0])
        self.play(FadeIn(row), run_time=0.6)
        shown = names
        y = 0.9
        for rule, _, codes in k["rules"]:
            now = [data.lts_code_name(c) for c in codes]
            if now == shown:
                continue
            n = rule - 1
            sec = next(name for a, b, name in data.LTS_SECTIONS if a <= n <= b)
            line = VGroup(label("rule %d (%s)" % (n, sec), 18, RED), label("  ".join(now), 34, INK)).arrange(RIGHT, buff=0.5)
            line.move_to([0, y, 0])
            hit = Rectangle(width=w / 372 * 3, height=0.5, stroke_color=RED, stroke_width=2).move_to(
                [x0 + w * n / 372, -1.6, 0])
            self.play(FadeIn(line, shift=DOWN * 0.15), cursor.animate.move_to([x0 + w * n / 372, -1.6, 0]),
                      FadeIn(hit), run_time=1.2)
            shown, y = now, y - 0.7

        self.sync(T.line(8)[0])
        out = label("   ".join(lts[w]["phonemes"] for w in WORDS), 34, BLUE).move_to([0, -0.8, 0])
        self.play(FadeIn(out, shift=UP * 0.2), run_time=1.2)
        self.pulse_until(T.duration - 0.2, [out])
        self.finish()


class TimingPitch(PaperScene):
    """Durations and the hat pattern (phtiming, pht0draw 0xc722): the example's phones and F0, frame by frame."""
    scene_id = "timing_pitch"

    def construct(self):
        T = self.timeline
        p = data.load_pitchlog(mv.OUT / "data" / "pitch_example.tsv")
        frames = [r["frame"] for r in p["F"]]
        n = frames[-1] + 1
        self.play(FadeIn(header("Timing and pitch", "phtiming · pht0draw 0xc722 · Klattalk's hat pattern")), run_time=1.0)

        # the phones, first all alike, then at their lengths
        ph = [r for r in p["P"]]
        x0, w = -6.2, 12.4
        same = VGroup(*(VGroup(Rectangle(width=w / len(ph) - 0.04, height=0.5, stroke_color=INK, stroke_width=1.2,
                                         fill_color="#fffdf7", fill_opacity=1), label(r["name"], 16, INK))
                        for r in ph)).arrange(RIGHT, buff=0.04).move_to([0, 1.7, 0])

        def sized():
            g = VGroup()
            for r in ph:
                cw = w * r["duration"] / n
                g.add(VGroup(Rectangle(width=max(cw - 0.03, 0.02), height=0.5, stroke_color=INK, stroke_width=1.2,
                                       fill_color="#fffdf7", fill_opacity=1).move_to([x0 + w * (r["frame"] + r["duration"] / 2) / n, 1.7, 0]),
                             label(r["name"], 16, INK).move_to([x0 + w * (r["frame"] + r["duration"] / 2) / n, 1.7, 0])))
            return g
        self.sync(T.line(0)[0])
        self.play(LaggedStart(*(FadeIn(b) for b in same), lag_ratio=0.05), run_time=1.6)
        self.sync(T.line(1)[0])
        self.play(Transform(same, sized()), run_time=2.5)
        self.pulse_until(T.line(2)[0], [b[1] for b in same if b[1].text != "_"][:8], color=BLUE)

        ruler = VGroup(*(Line([x0 + w * f / n, 1.32, 0], [x0 + w * f / n, 1.42 if f % 10 else 1.22, 0], color=INK2,
                              stroke_width=0.8) for f in range(0, n, 2)))
        self.sync(T.line(2)[0])
        self.play(Create(ruler), FadeIn(label("one tick = two frames = 12.8 ms", 16).move_to([0, 1.05, 0])), run_time=1.6)

        ax = Axes(n, 40, 200, w, 3.2, [0, -1.25, 0], "F0, Hz", [50, 100, 150])
        base = ax.curve(frames, [r["f0baseline"] / 10 for r in p["F"]], INK2, 1.5)
        self.sync(T.line(3)[0])
        self.play(FadeIn(ax), Create(base), run_time=1.5)
        hat = ax.steps(frames + [n], [(r["f0baseline"] + r["tarhat"]) / 10 for r in p["F"]] + [0], BLUE)
        self.sync(T.line(4)[0])
        self.play(Create(hat), run_time=2.5)
        self.play(FadeIn(label("the hat: up at the first stress, down at the last", 16, BLUE)
                         .move_to([0, ax.box.get_top()[1] - 0.25, 0])), run_time=0.8)

        target = ax.steps(frames + [n], [r["f0in"] / 10 for r in p["F"]] + [0], "#c08a1e", 1.5)
        self.sync(T.line(5)[0])
        self.play(Create(target), run_time=2.5)

        comma = next(r for r in ph if r["name"] == "_" and r["frame"] > 0)
        last = ph[-2]
        tags = VGroup(label("comma: rise", 16, RED).move_to([ax.x(comma["frame"]), ax.y(185), 0]),
                      label("period: fall", 16, RED).move_to([ax.x(last["frame"]), ax.y(185), 0]))
        self.sync(T.line(6)[0])
        self.play(LaggedStart(*(FadeIn(t, shift=DOWN * 0.1) for t in tags), lag_ratio=0.6), run_time=1.6)
        self.pulse_until(T.line(7)[0], list(tags))

        voiced = [(f, r["f0_tenths"] / 10) for f, r in zip(frames, p["F"])]
        f0 = ax.curve([f for f, _ in voiced], [v for _, v in voiced], INK, 3)
        self.play(Create(f0), run_time=2.5)
        self.play(FadeIn(label("T0 = 40000 / F0, to the DSP every frame", 18, INK).move_to([3.6, 2.85, 0])),
                  run_time=0.8)
        self.finish()


class Formants(PaperScene):
    """Formant targets, coarticulation, diphthong lines and the forward/backward ramps (phsettar, phdraw)."""
    scene_id = "formants"

    def construct(self):
        T = self.timeline
        f = data.load_formantlog(mv.OUT / "data" / "formant_example.tsv")
        frames = [r["frame"] for r in f["F"]]
        n = frames[-1] + 1
        ph = f["P"]
        self.play(FadeIn(header("Formants", "phsettar 0xaa08 · phdraw 0xa804 · targets, then ramps")), run_time=1.0)
        ax = Axes(n, 0, 3200, 12.4, 4.4, [0, -0.45, 0], "Hz", [500, 1000, 2000, 3000])
        names = ax.phones(ph)
        self.sync(T.line(0)[0])
        self.play(FadeIn(ax), FadeIn(names), run_time=1.2)
        self.pulse_until(T.line(1)[0], list(names)[:8], color=BLUE)

        cols = {"F1": RED, "F2": BLUE, "F3": INK}
        tags = VGroup(*(label(k, 18, c).move_to([ax.box.get_right()[0] + 0.3, ax.y(v), 0])
                        for (k, c), v in zip(cols.items(), (550, 1400, 2500))))
        self.sync(T.line(1)[0])
        self.play(FadeIn(tags), run_time=0.8)
        self.pulse_until(T.line(2)[0], list(tags))

        def target_steps(key):
            xs, ys = [], []
            for r in ph:
                v = r[key]["table"]
                if v > 0:
                    xs += [r["frame"], r["frame"] + r["duration"]]
                    ys += [v, v]
            return xs, ys
        tables = VGroup()
        for k, c in cols.items():
            xs, ys = target_steps(k)
            tables.add(VGroup(*(ax.curve(xs[i:i + 2], ys[i:i + 2], c, 2.5) for i in range(0, len(xs), 2))))
        self.sync(T.line(2)[0])
        self.play(LaggedStart(*(Create(t) for t in tables), lag_ratio=0.4), run_time=3.0)
        self.play(FadeIn(label("targets from the male table (Paul)", 16).move_to([0, ax.box.get_top()[1] - 0.25, 0])),
                  run_time=0.6)

        pulled = VGroup()
        for k, c in cols.items():
            g = VGroup()
            for r in ph:
                a, b = r[k]["table"], r[k]["tarcur"]
                if a > 0 and abs(a - b) > 5:
                    x = ax.x(r["frame"] + r["duration"] / 2)
                    g.add(Arrow([x, ax.y(a), 0], [x, ax.y(b), 0], buff=0, color=c, stroke_width=2,
                                max_tip_length_to_length_ratio=0.5))
            pulled.add(g)
        self.sync(T.line(3)[0])
        self.play(LaggedStart(*(FadeIn(g) for g in pulled), lag_ratio=0.3), run_time=2.0)

        ay = next(r for r in ph if r["name"] == "ay")
        span = Rectangle(width=ax.x(ay["frame"] + ay["duration"]) - ax.x(ay["frame"]), height=ax.box.height - 0.1,
                         stroke_width=0, fill_color=BLUE, fill_opacity=0.10).move_to(
            [ax.x(ay["frame"] + ay["duration"] / 2), ax.box.get_y(), 0])
        in_ay = [r for r in f["F"] if ay["frame"] <= r["frame"] < ay["frame"] + ay["duration"]]
        glide = VGroup(*(ax.curve([r["frame"] for r in in_ay], [r[k] for r in in_ay], c, 3.5) for k, c in cols.items()))
        self.sync(T.line(4)[0])
        self.play(FadeIn(span), FadeIn(label("ay: straight lines between break points", 16, BLUE)
                                       .next_to(span, UP, 0.05)), run_time=0.8)
        self.play(Create(glide), run_time=2.0)

        tracks = VGroup(*(ax.curve(frames, [r[k] for r in f["F"]], c, 2.5) for k, c in cols.items()))
        self.sync(T.line(5)[0])
        self.play(tables.animate.set_opacity(0.25), FadeOut(pulled), run_time=0.6)
        self.play(LaggedStart(*(Create(t) for t in tracks), lag_ratio=0.3), run_time=3.5)

        p_ = next(r for r in ph if r["name"] == "p" and r["frame"] > 230)
        locus = Dot([ax.x(p_["frame"] + p_["duration"]), ax.y(p_["F2"]["fbou"] or p_["F2"]["tarcur"]), 0], radius=0.09,
                    color=RED)
        self.sync(T.line(6)[0])
        ring = Rectangle(width=1.4, height=1.1, stroke_color=RED, stroke_width=2).move_to(
            [ax.x(p_["frame"] + p_["duration"]), ax.y(1000), 0])
        self.play(Create(ring), FadeIn(label("p → ao: F2 starts near the locus", 16, RED).next_to(ring, UP, 0.05)),
                  run_time=1.2)
        self.pulse_until(T.line(7)[0], [ring])

        cursor = DashedLine([ax.x(0), ax.box.get_bottom()[1], 0], [ax.x(0), ax.box.get_top()[1], 0], color=INK,
                            stroke_width=1.5)
        self.add(cursor)
        self.play(cursor.animate.shift(RIGHT * ax.box.width), run_time=max(1.0, T.duration - self.now() - 0.3))
        self.finish()


class Frame(PaperScene):
    """One speech frame: header 0x4000, the 17 parameter words, trailer 0x43D4 (dsp_post_frame 0x7b56)."""
    scene_id = "frame"

    def construct(self):
        T = self.timeline
        f = data.load_formantlog(mv.OUT / "data" / "formant_example.tsv")
        ao = next(r for r in f["P"] if r["name"] == "ao")
        k = next(i for i, r in enumerate(f["F"]) if r["frame"] == ao["frame"] + ao["duration"] // 2)
        self.play(FadeIn(header("One frame", "19 words, every 6.4 ms · dsp_post_frame 0x7b56")), run_time=1.0)
        names = ["hdr"] + data.FORMANT_F[1:] + ["end"]

        def values(i):
            r = f["F"][i]
            return ["4000"] + [str(r[x]) for x in data.FORMANT_F[1:]] + ["43D4"]
        cells = VGroup(*(Rectangle(width=0.6, height=0.6, stroke_color=INK, stroke_width=1.2, fill_color="#fffdf7",
                                   fill_opacity=1) for _ in names)).arrange(RIGHT, buff=0.04).move_to([0, 0.6, 0])
        heads = VGroup(*(label(nm, 15, INK2).next_to(c, UP, 0.1) for nm, c in zip(names, cells)))

        def texts(i):
            return VGroup(*(label(v, 15 if len(v) < 4 else 13, INK).move_to(c) for v, c in zip(values(i), cells)))
        self.sync(T.line(0)[0])
        self.play(LaggedStart(*(FadeIn(c) for c in cells), lag_ratio=0.05), FadeIn(heads), run_time=1.6)
        self.pulse_until(T.line(1)[0], list(heads)[:4], color=BLUE)

        vals = texts(k)
        self.sync(T.line(1)[0])
        self.play(LaggedStart(*(FadeIn(v) for v in vals[:9]), lag_ratio=0.3), run_time=3.0)
        self.sync(T.line(2)[0])
        self.play(LaggedStart(*(FadeIn(v) for v in vals[9:]), lag_ratio=0.3), run_time=3.0)
        self.pulse_until(T.line(3)[0], list(vals[9:]), color=BLUE)

        groups = [(1, 1, "steps of 25 µs"), (2, 8, "Hz"), (9, 16, "dB"), (17, 17, "tilt")]
        units = VGroup()
        for a, b, text in groups:
            l, r = cells[a].get_left()[0], cells[b].get_right()[0]
            units.add(VGroup(Line([l, -0.05, 0], [r, -0.05, 0], color=BLUE, stroke_width=2),
                             label(text, 16, BLUE).move_to([(l + r) / 2, -0.3, 0])))
        self.sync(T.line(3)[0])
        self.play(LaggedStart(*(FadeIn(u) for u in units), lag_ratio=0.4), run_time=1.8)
        self.pulse_until(T.line(4)[0], [u[1] for u in units])

        dsp = box("DSP", "TMS32010").move_to([0, -1.9, 0])
        self.sync(T.line(4)[0])
        self.play(FadeIn(dsp), Create(Arrow(cells.get_bottom() + DOWN * 0.5, dsp[0].get_top(), buff=0.05, color=INK,
                                             stroke_width=LINE_W)), run_time=1.0)
        i = k
        while T.duration - self.now() > 0.5:    # the frames that follow, one every 6.4 ms (slowed down)
            i += 1
            self.play(Transform(vals, texts(i)), run_time=0.4)
        self.finish()
