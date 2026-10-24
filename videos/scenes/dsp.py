"""Inside the DSP: the explainer video's scenes on the TMS32010 program (src/speech/dsp_synth.c, REFERENCE.md s16,
dsp/dsp_synth_topology). Every trace is dsplog's data for the example sentence (videos/data.py)."""
import sys
from pathlib import Path

import numpy as np
from manim import (DOWN, LEFT, RIGHT, UP, Arrow, Circle, Create, DashedLine, FadeIn, FadeOut, LaggedStart, Line,
                   Rectangle, Text, Transform, VGroup, VMobject, Write)

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from scenes.common import (BLUE, FONT, INK, INK2, LINE_W, PAPER, RED, PaperScene, Scope, box,  # noqa: E402
                           glyphs, header)
import data                                     # noqa: E402
import make_video as mv                         # noqa: E402


def example():
    """dsplog of the example sentence: (S columns, phones)."""
    s, phones, _ = data.load_dsplog(mv.OUT / "data" / "dsp_example.tsv")
    return s, phones


def phone(phones, name):
    """The first phone of this name: (start sample, end sample)."""
    r = next(r for r in phones if r["name"] == name)
    return r["n"], r["n"] + r["samples"]


def label(text, size=20, color=INK2, **kw):
    return Text(text, font=FONT, color=color, font_size=size, **kw)


class SpectrumPanel(VGroup):
    """A dB-over-Hz plot, 0 to 5 kHz and 0 to -60 dB, in a ruled frame; curve(x) gives the spectrum of x."""

    def __init__(self, width, height, at):
        super().__init__()
        self.w, self.h, self.at = width, height, np.array(at, dtype=float)
        frame = Rectangle(width=width, height=height, stroke_color=INK, stroke_width=1).move_to(at)
        ticks = VGroup()
        for khz in range(6):
            x = self.x(khz * 1000)
            ticks.add(Line([x, self.y(-60), 0], [x, self.y(-60) - 0.08, 0], color=INK, stroke_width=1),
                      label("%d" % khz, 16).move_to([x, self.y(-60) - 0.25, 0]))
        ticks.add(label("kHz", 16).move_to([self.x(5000) + 0.3, self.y(-60) - 0.25, 0]))
        ticks.add(label("dB", 16).move_to([self.x(0) - 0.3, self.y(0), 0]))
        self.add(frame, ticks)

    def x(self, hz):
        return self.at[0] - self.w / 2 + self.w * hz / 5000

    def y(self, db):
        return self.at[1] + self.h / 2 + self.h * max(db, -60) / 60

    def curve(self, x, color=RED):
        hz, db = data.spectrum_db(x)
        pts = np.array([[self.x(f), self.y(d), 0] for f, d in zip(hz, db) if f <= 5000])
        return VMobject(color=color, stroke_width=LINE_W).set_points_as_corners(pts)

    def marker(self, hz, text, color=BLUE, raise_by=0.0):
        line = DashedLine([self.x(hz), self.y(-60), 0], [self.x(hz), self.y(0) + raise_by, 0], color=color,
                          stroke_width=1.5)
        return VGroup(line, label(text, 16, color).next_to(line, UP, buff=0.05))


class DspCascade(PaperScene):
    """The cascade branch (CASCADE_BRANCH 0x5E8): nasal zero, nasal pole, F5 ... F1 in a row, each section's output
    on the scope and its spectrum, over the vowel of "Paul"."""
    scene_id = "dsp_cascade"

    def construct(self):
        s, phones = example()
        lo, hi = phone(phones, "ao")
        mid = (lo + hi) // 2
        win = slice(mid - 256, mid + 256)
        T = self.timeline

        self.play(FadeIn(header("The cascade branch",
                                "CASCADE_BRANCH 0x5E8 · the vocal tract as seven sections in a row")), run_time=1.0)

        # the chain: source -> NZ NP F5 F4 F3 F2 F1 -> x2
        stages = [("NZ", "nasal zero", "np"), ("NP", "nasal pole", "np"), ("F5", "head size", "cf5"),
                  ("F4", "head size", "cf4"), ("F3", "%d Hz" % s["f3"][mid], "cf3"),
                  ("F2", "%d Hz" % s["f2"][mid], "cf2"), ("F1", "%d Hz" % s["f1"][mid], "cf1")]
        boxes = VGroup(*(box(n, sub) for n, sub, _ in stages)).arrange(RIGHT, buff=0.22).move_to([0.2, 1.55, 0])
        wires = VGroup(*(Line(a[0].get_right(), b[0].get_left(), color=INK, stroke_width=LINE_W)
                         for a, b in zip(boxes, boxes[1:])))
        src_scope = Scope(s["src"], lo, hi, 1.6, 0.8, INK2, at=boxes.get_left() + LEFT * 1.25)
        src_lbl = label("source", 16).next_to(src_scope, UP, buff=0.25)
        into = Arrow(src_scope.get_right() + RIGHT * 0.05, boxes[0][0].get_left(), buff=0.02, color=INK,
                     stroke_width=LINE_W, max_tip_length_to_length_ratio=0.25)
        double = VGroup(Circle(radius=0.32, color=INK, stroke_width=LINE_W, fill_color=PAPER, fill_opacity=1),
                        label("×2", 22, INK)).next_to(boxes, RIGHT, buff=0.45)
        out = Arrow(boxes[-1][0].get_right(), double[0].get_left(), buff=0.02, color=INK, stroke_width=LINE_W,
                    max_tip_length_to_length_ratio=0.25)
        to_sum = Arrow(double[0].get_right(), double[0].get_right() + RIGHT * 0.7, buff=0.02, color=INK,
                       stroke_width=LINE_W, max_tip_length_to_length_ratio=0.3)
        sum_lbl = label("to the sum", 16).next_to(to_sum, DOWN, buff=0.1)

        self.sync(T.line(0)[0])
        self.add(src_scope)
        self.play(FadeIn(src_lbl), Create(into), run_time=0.8)
        self.play(LaggedStart(*(FadeIn(b, shift=DOWN * 0.15) for b in boxes), lag_ratio=0.25),
                  Create(wires), run_time=3.0)
        self.play(Create(out), FadeIn(double), run_time=1.0)

        # one line of arithmetic, in Q12
        self.sync(T.line(1)[0])
        self.play(boxes[-1][0].animate.set_stroke(RED, width=4), run_time=0.6)
        eq = Text("y[n] = A·x[n] + B·y[n−1] + C·y[n−2]", font=FONT, color=INK, font_size=38).move_to([0, -0.2, 0])
        self.sync(T.line(2)[0])
        self.play(Write(eq), run_time=2.5)
        notes = VGroup(label("the input", 18, RED), label("the last output", 18, RED),
                       label("the one before", 18, RED))
        terms = [glyphs(eq, part) for part in ("A·x[n]", "B·y[n−1]", "C·y[n−2]")]
        for n, term in zip(notes, terms):
            n.next_to(term, DOWN, buff=0.25)
        self.play(LaggedStart(*(FadeIn(n, shift=UP * 0.1) for n in notes), lag_ratio=0.5), run_time=2.0)
        self.sync(T.line(3)[0])
        coef = label("A, B, C  ←  the formant's frequency and bandwidth (a cosine table in the DSP ROM)", 22, INK)
        coef.next_to(eq, DOWN, buff=0.95)
        self.play(FadeIn(coef, shift=UP * 0.1), run_time=1.0)
        self.sync(T.line(4)[0])
        q12 = label("Q12: 16-bit words, 12 bits after the binary point (4096 = 1.0)", 22, BLUE).next_to(coef, DOWN,
                                                                                                    buff=0.3)
        self.play(FadeIn(q12, shift=UP * 0.1), run_time=1.0)
        self.pulse_until(T.line(5)[0], terms)

        # section by section: the scope and the spectrum
        self.sync(T.line(5)[0])
        scope_frame = Rectangle(width=5.6, height=2.2, stroke_color=INK, stroke_width=1).move_to([-3.3, -1.2, 0])
        scope = Scope(s["np"], lo, hi, 5.3, 1.75, RED, at=scope_frame.get_center())
        probe = label("probe: after the nasal pole", 18).next_to(scope_frame, UP, buff=0.08).align_to(scope_frame,
                                                                                                     LEFT)
        spec = SpectrumPanel(5.4, 2.2, [3.4, -1.15, 0])
        curve = spec.curve(s["src"][win], INK2)
        self.play(FadeOut(VGroup(eq, notes, coef, q12)), boxes[-1][0].animate.set_stroke(INK, width=LINE_W),
                  run_time=0.8)
        self.play(FadeIn(scope_frame), FadeIn(probe), FadeIn(spec), Create(curve), run_time=1.0)
        self.add(scope)

        def visit(k, *, run_time=1.2):
            name, _, col = stages[k]
            new_probe = label("probe: after %s" % ("the nasal pole" if k < 2 else name), 18)
            new_probe.move_to(probe, aligned_edge=LEFT)
            scope.set_signal(s[col])
            return [boxes[k][0].animate.set_stroke(RED, width=4), Transform(curve, spec.curve(s[col][win])),
                    Transform(probe, new_probe)]

        self.play(*visit(0), boxes[1][0].animate.set_stroke(RED, width=4), run_time=1.5)
        self.sync(T.line(6)[0])
        self.play(*visit(2), run_time=1.5)
        self.play(*visit(3), run_time=1.5)
        self.sync(T.line(7)[0])
        for k in (4, 5, 6):
            self.play(*visit(k), run_time=1.6)
        marks = VGroup(*(spec.marker(s[f][mid], "%s %d" % (f.upper(), s[f][mid]), raise_by=0.3 * (f == "f2"))
                         for f in ("f1", "f2", "f3")))
        self.play(FadeIn(marks), run_time=1.0)

        # the buzz in, the vowel out
        self.sync(T.line(8)[0])
        self.play(src_scope.animate.set_stroke(INK, width=3), scope.animate.set_stroke(RED, width=3), run_time=1.0)
        self.play(src_scope.animate.set_stroke(INK2, width=LINE_W), scope.animate.set_stroke(RED, width=LINE_W),
                  run_time=1.0)

        # doubled, on to the sum
        self.sync(T.line(9)[0])
        self.play(double[0].animate.set_stroke(RED, width=4), Create(to_sum), FadeIn(sum_lbl), run_time=1.2)
        self.finish()
